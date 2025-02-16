/******************************************************************************
 *
 * Name:	skgesirq.c
 * Project:	GEnesis, PCI Gigabit Ethernet Adapter
 * Version:	$Revision: 1.1.1.1 $
 * Date:	$Date: 2010/03/05 07:31:24 $
 * Purpose:	Special IRQ module
 *
 ******************************************************************************/

/******************************************************************************
 *
 *	(C)Copyright 1998-2000 SysKonnect GmbH.
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	The information in this file is provided "AS IS" without warranty.
 *
 ******************************************************************************/


/*
 *	Special Interrupt handler
 *
 *	The following abstract should show how this module is included
 *	in the driver path:
 *
 *	In the ISR of the driver the bits for frame transmission complete and
 *	for receive complete are checked and handled by the driver itself.
 *	The bits of the slow path mask are checked after this and then the
 *	entry into the so-called "slow path" is prepared. It is an implemetors
 *	decision whether this is executed directly or just scheduled by
 *	disabling the mask. In the interrupt service routine events may be
 *	generated, so it would be a good idea to call the EventDispatcher
 *	right after this ISR.
 *
 *	The Interrupt service register of the adapter is NOT read by this
 *	module. SO if the drivers implemetor needs a while loop around the
 *	slow data paths Interrupt bits, he needs to call the SkGeIsr() for
 *	each loop entered.
 *
 *	However, the XMAC Interrupt status registers are read in a while loop.
 *
 */
 
static const char SysKonnectFileId[] =
	"$Id: skgesirq.c,v 1.1.1.1 2010/03/05 07:31:24 reynolds Exp $" ;

#include "h/skdrv1st.h"		/* Driver Specific Definitions */
#include "h/skgepnmi.h"		/* PNMI Definitions */
#include "h/skrlmt.h"		/* RLMT Definitions */
#include "h/skdrv2nd.h"		/* Adapter Control- and Driver specific Def. */

/* local function prototypes */
static int	SkGePortCheckUpXmac(SK_AC*, SK_IOC, int);
static int	SkGePortCheckUpBcom(SK_AC*, SK_IOC, int);
static int	SkGePortCheckUpLone(SK_AC*, SK_IOC, int);
static int	SkGePortCheckUpNat(SK_AC*, SK_IOC, int);
static void	SkPhyIsrBcom(SK_AC*, SK_IOC, int, SK_U16);
static void	SkPhyIsrLone(SK_AC*, SK_IOC, int, SK_U16);

/*
 * Define an array of RX counter which are checked
 * in AutoSense mode to check whether a link is not able to autonegotiate.
 */
static const SK_U32 SkGeRxOids[]= {
	OID_SKGE_STAT_RX_64,
	OID_SKGE_STAT_RX_127,
	OID_SKGE_STAT_RX_255,
	OID_SKGE_STAT_RX_511,
	OID_SKGE_STAT_RX_1023,
	OID_SKGE_STAT_RX_MAX,
} ;

#ifdef __C2MAN__
/*
 *	Special IRQ function
 *
 *	General Description:
 *
 */
intro()
{}
#endif

/* Define return codes of SkGePortCheckUp and CheckShort. */
#define	SK_HW_PS_NONE		0	/* No action needed */
#define	SK_HW_PS_RESTART	1	/* Restart needed */
#define	SK_HW_PS_LINK		2	/* Link Up actions needed */

/******************************************************************************
 *
 *	SkHWInitDefSense() - Default Autosensing mode initialization
 *
 * Description:
 *	This function handles the Hardware link down signal
 *
 * Note:
 *
 */
void	SkHWInitDefSense(
SK_AC	*pAC,	/* adapter context */
SK_IOC	IoC,	/* IO context */
int	Port)		/* Port Index (MAC_1 + n) */
{
	SK_GEPORT	*pPrt;

	pPrt = &pAC->GIni.GP[Port];

	pPrt->PAutoNegTimeOut = 0;

	if (pPrt->PLinkModeConf != SK_LMODE_AUTOSENSE) {
		pPrt->PLinkMode = pPrt->PLinkModeConf;
		return;
	}

	SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_IRQ,
		("AutoSensing: First mode %d on Port %d\n",
		(int)SK_LMODE_AUTOFULL,
		 Port));

	pPrt->PLinkMode = SK_LMODE_AUTOFULL;

	return;
}	/* SkHWInitDefSense */


/******************************************************************************
 *
 *	SkHWSenseGetNext() - GetNextAutosensing Mode
 *
 * Description:
 *	This function handles the AutoSensing
 *
 * Note:
 *
 */
SK_U8	SkHWSenseGetNext(
SK_AC	*pAC,	/* adapter context */
SK_IOC	IoC,	/* IO context */
int	Port)		/* Port Index (MAC_1 + n) */
{
	SK_GEPORT	*pPrt;

	pPrt = &pAC->GIni.GP[Port];

	pPrt->PAutoNegTimeOut = 0;

	if (pPrt->PLinkModeConf != SK_LMODE_AUTOSENSE) {
		/* Leave all as configured */
		return (pPrt->PLinkModeConf);
	}

	if (pPrt->PLinkMode == SK_LMODE_AUTOFULL) {
		/* Return next mode AUTOBOTH */
		return (SK_LMODE_AUTOBOTH);
	}

	/* Return default autofull */
	return (SK_LMODE_AUTOFULL);
}	/* SkHWSenseGetNext */


/******************************************************************************
 *
 *	SkHWSenseSetNext() - Autosensing Set next mode
 *
 * Description:
 *	This function sets the appropriate next mode.
 *
 * Note:
 *
 */
void	SkHWSenseSetNext(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int	Port,			/* Port Index (MAC_1 + n) */
SK_U8	NewMode)	/* New Mode to be written in sense mode */
{
	SK_GEPORT	*pPrt;

	pPrt = &pAC->GIni.GP[Port];

	pPrt->PAutoNegTimeOut = 0;

	if (pPrt->PLinkModeConf != SK_LMODE_AUTOSENSE) {
		return;
	}

	SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_IRQ,
		("AutoSensing: next mode %d on Port %d\n", (int)NewMode, Port));
	pPrt->PLinkMode = NewMode;

	return;
}	/* SkHWSenseSetNext */


/******************************************************************************
 *
 *	SkHWLinkDown() - Link Down handling
 *
 * Description:
 *	This function handles the Hardware link down signal
 *
 * Note:
 *
 */
void	SkHWLinkDown(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int		Port)		/* Port Index (MAC_1 + n) */
{
	SK_GEPORT	*pPrt;
	SK_U16		Word;

	pPrt = &pAC->GIni.GP[Port];

	/* Disable all XMAC interrupts. */
	XM_OUT16(IoC, Port, XM_IMSK, 0xffff);

	/* Disable Receiver and Transmitter. */
	XM_IN16(IoC, Port, XM_MMU_CMD, &Word);
	XM_OUT16(IoC, Port, XM_MMU_CMD, Word & ~(XM_MMU_ENA_RX | XM_MMU_ENA_TX));
	
	/* Disable all PHY interrupts. */
	switch (pPrt->PhyType) {
		case SK_PHY_BCOM:
			/* Make sure that PHY is initialized. */
			if (pAC->GIni.GP[Port].PState) {
				/* NOT allowed if BCOM is in RESET state */
				/* Disable Power Management if link is down. */
				PHY_READ(IoC, pPrt, Port, PHY_BCOM_AUX_CTRL, &Word);
				PHY_WRITE(IoC, pPrt, Port, PHY_BCOM_AUX_CTRL,
					Word | PHY_B_AC_DIS_PM);
				PHY_WRITE(IoC, pPrt, Port, PHY_BCOM_INT_MASK, 0xffff);
			}
			break;
		case SK_PHY_LONE:
			PHY_WRITE(IoC, pPrt, Port, PHY_LONE_INT_ENAB, 0x0);
			break;
		case SK_PHY_NAT:
			/* todo: National
			PHY_WRITE(IoC, pPrt, Port, PHY_NAT_INT_MASK, 0xffff); */
			break;
	}

	/* Init default sense mode. */
	SkHWInitDefSense(pAC, IoC, Port);

	if (!pPrt->PHWLinkUp) {
		return;
	} 

	SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_IRQ,
		("Link down Port %d\n", Port));

	/* Set Link to DOWN. */
	pPrt->PHWLinkUp = SK_FALSE;

	/* Reset Port stati */
	pPrt->PLinkModeStatus = SK_LMODE_STAT_UNKNOWN;
	pPrt->PFlowCtrlStatus = SK_FLOW_STAT_NONE;

	/*
	 * Reinit Phy especially when the AutoSense default is set now.
	 */
	SkXmInitPhy(pAC, IoC, Port, SK_FALSE);


	/* Do NOT signal to RLMT. */

	/* Do NOT start the timer here. */
}	/* SkHWLinkDown */


/******************************************************************************
 *
 *	SkHWLinkUp() - Link Up handling
 *
 * Description:
 *	This function handles the Hardware link up signal
 *
 * Note:
 *
 */
void	SkHWLinkUp(
SK_AC	*pAC,	/* adapter context */
SK_IOC	IoC,	/* IO context */
int	Port)		/* Port Index (MAC_1 + n) */
{
	SK_GEPORT	*pPrt;

	pPrt = &pAC->GIni.GP[Port];

	if (pPrt->PHWLinkUp) {
		/* We do NOT need to proceed on active link */
		return;
	} 

	pPrt->PHWLinkUp = SK_TRUE;
	pPrt->PAutoNegFail = SK_FALSE;
	pPrt->PLinkModeStatus = SK_LMODE_STAT_UNKNOWN;

	if (pPrt->PLinkMode != SK_LMODE_AUTOHALF &&
	    pPrt->PLinkMode != SK_LMODE_AUTOFULL &&
	    pPrt->PLinkMode != SK_LMODE_AUTOBOTH) {
		/* Link is up and no Autonegotiation should be done */

		/* Configure Port */

		/* Set Link Mode */
		if (pPrt->PLinkMode == SK_LMODE_FULL) {
			pPrt->PLinkModeStatus = SK_LMODE_STAT_FULL;
		}
		else {
			pPrt->PLinkModeStatus = SK_LMODE_STAT_HALF;
		}

		/* No flow control without autonegotiation */
		pPrt->PFlowCtrlStatus = SK_FLOW_STAT_NONE;

		/* RX/TX enable */
		SkXmRxTxEnable(pAC, IoC, Port);
	}
}	/* SkHWLinkUp */


/******************************************************************************
 *
 * SkMacParity	- does everything to handle MAC parity errors correctly
 *
 */
static	void	SkMacParity(
SK_AC	*pAC,	/* adapter context */
SK_IOC	IoC,	/* IO context */
int	Port)		/* Port Index of the port failed */
{
	SK_EVPARA	Para;
	SK_GEPORT	*pPrt;		/* GIni Port struct pointer */
	SK_U64		TxMax;		/* TxMax Counter */
	unsigned	Len;

	pPrt = &pAC->GIni.GP[Port];

	/* Clear IRQ */
	SK_OUT16(IoC, MR_ADDR(Port,TX_MFF_CTRL1), MFF_CLR_PERR);

	if (pPrt->PCheckPar) {
		if (Port == MAC_1) {
			SK_ERR_LOG(pAC, SK_ERRCL_HW , SKERR_SIRQ_E016,
				SKERR_SIRQ_E016MSG);
		}
		else {
			SK_ERR_LOG(pAC, SK_ERRCL_HW , SKERR_SIRQ_E017,
				SKERR_SIRQ_E017MSG);
		}
		Para.Para64 = Port;
		SkEventQueue(pAC, SKGE_DRV, SK_DRV_PORT_FAIL, Para);
		Para.Para32[0] = Port;
		SkEventQueue(pAC, SKGE_RLMT, SK_RLMT_LINK_DOWN, Para);

		return;
	}

	/* Check whether frames with a size of 1k were sent */
	Len = sizeof(SK_U64);
	SkPnmiGetVar(pAC, IoC, OID_SKGE_STAT_TX_MAX, (char *)&TxMax,
		&Len, (SK_U32)SK_PNMI_PORT_PHYS2INST(pAC, Port),
		pAC->Rlmt.Port[Port].Net->NetNumber);

	if (TxMax > 0) {
		/* From now on check the parity */
		pPrt->PCheckPar = SK_TRUE;
	}
}	/* SkMacParity */


/******************************************************************************
 *
 *	Hardware Error service routine
 *
 * Description:
 *
 * Notes:
 */
static	void	SkGeHwErr(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
SK_U32	HwStatus)	/* Interrupt status word */
{
	SK_EVPARA	Para;
	SK_U16		Word;

	if ((HwStatus & IS_IRQ_MST_ERR) || (HwStatus & IS_IRQ_STAT)) {
		if (HwStatus & IS_IRQ_STAT) {
			SK_ERR_LOG(pAC, SK_ERRCL_HW , SKERR_SIRQ_E013, SKERR_SIRQ_E013MSG);
		}
		else {
			SK_ERR_LOG(pAC, SK_ERRCL_HW , SKERR_SIRQ_E012, SKERR_SIRQ_E012MSG);
		}

		/* Reset all bits in the PCI STATUS register */
		SK_OUT8(IoC, B2_TST_CTRL1, TST_CFG_WRITE_ON);
		SK_IN16(IoC, PCI_C(PCI_STATUS), &Word);
		SK_OUT16(IoC, PCI_C(PCI_STATUS), Word | PCI_ERRBITS);
		SK_OUT8(IoC, B2_TST_CTRL1, TST_CFG_WRITE_OFF);

		Para.Para64 = 0;
		SkEventQueue(pAC, SKGE_DRV, SK_DRV_ADAP_FAIL, Para);
	}

	if (HwStatus & IS_NO_STAT_M1) {
		/* Ignore it */
		/* This situation is also indicated in the descriptor */
		SK_OUT16(IoC, MR_ADDR(MAC_1,RX_MFF_CTRL1), MFF_CLR_INSTAT);
	}

	if (HwStatus & IS_NO_STAT_M2) {
		/* Ignore it */
		/* This situation is also indicated in the descriptor */
		SK_OUT16(IoC, MR_ADDR(MAC_2,RX_MFF_CTRL1), MFF_CLR_INSTAT);
	}

	if (HwStatus & IS_NO_TIST_M1) {
		/* Ignore it */
		/* This situation is also indicated in the descriptor */
		SK_OUT16(IoC, MR_ADDR(MAC_1,RX_MFF_CTRL1), MFF_CLR_INTIST);
	}

	if (HwStatus & IS_NO_TIST_M2) {
		/* Ignore it */
		/* This situation is also indicated in the descriptor */
		SK_OUT16(IoC, MR_ADDR(MAC_2,RX_MFF_CTRL1), MFF_CLR_INTIST);
	}

	if (HwStatus & IS_RAM_RD_PAR) {
		SK_OUT16(IoC, B3_RI_CTRL, RI_CLR_RD_PERR);
		SK_ERR_LOG(pAC, SK_ERRCL_HW , SKERR_SIRQ_E014, SKERR_SIRQ_E014MSG);
		Para.Para64 = 0;
		SkEventQueue(pAC, SKGE_DRV, SK_DRV_ADAP_FAIL, Para);
	}

	if (HwStatus & IS_RAM_WR_PAR) {
		SK_OUT16(IoC, B3_RI_CTRL, RI_CLR_WR_PERR);
		SK_ERR_LOG(pAC, SK_ERRCL_HW , SKERR_SIRQ_E015, SKERR_SIRQ_E015MSG);
		Para.Para64 = 0;
		SkEventQueue(pAC, SKGE_DRV, SK_DRV_ADAP_FAIL, Para);
	}

	if (HwStatus & IS_M1_PAR_ERR) {
		SkMacParity(pAC, IoC, MAC_1);
	}

	if (HwStatus & IS_M2_PAR_ERR) {
		SkMacParity(pAC, IoC, MAC_2);
	}

	if (HwStatus & IS_R1_PAR_ERR) {
		/* Clear IRQ */
		SK_OUT32(IoC, B0_R1_CSR, CSR_IRQ_CL_P);

		SK_ERR_LOG(pAC, SK_ERRCL_HW , SKERR_SIRQ_E018, SKERR_SIRQ_E018MSG);
		Para.Para64 = MAC_1;
		SkEventQueue(pAC, SKGE_DRV, SK_DRV_PORT_FAIL, Para);
		Para.Para32[0] = MAC_1;
		SkEventQueue(pAC, SKGE_RLMT, SK_RLMT_LINK_DOWN, Para);
	}

	if (HwStatus & IS_R2_PAR_ERR) {
		/* Clear IRQ */
		SK_OUT32(IoC, B0_R2_CSR, CSR_IRQ_CL_P);

		SK_ERR_LOG(pAC, SK_ERRCL_HW , SKERR_SIRQ_E019, SKERR_SIRQ_E019MSG);
		Para.Para64 = MAC_2;
		SkEventQueue(pAC, SKGE_DRV, SK_DRV_PORT_FAIL, Para);
		Para.Para32[0] = MAC_2;
		SkEventQueue(pAC, SKGE_RLMT, SK_RLMT_LINK_DOWN, Para);
	}
}	/* SkGeHwErr */


/******************************************************************************
 *
 *	Interrupt service routine
 *
 * Description:
 *
 * Notes:
 */
void	SkGeSirqIsr(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
SK_U32	Istatus)	/* Interrupt status word */
{
	SK_EVPARA	Para;
	SK_U32		RegVal32;	/* Read register Value */
	SK_U16		XmIsr;

	if (Istatus & IS_HW_ERR) {
		SK_IN32(IoC, B0_HWE_ISRC, &RegVal32);
		SkGeHwErr(pAC, IoC, RegVal32);
	}

	/*
	 * Packet Timeout interrupts
	 */
	/* Check whether XMACs are correctly initialized */
	if ((Istatus & (IS_PA_TO_RX1 | IS_PA_TO_TX1)) &&
		!pAC->GIni.GP[MAC_1].PState) {
		/* XMAC was not initialized but Packet timeout occured */
		SK_ERR_LOG(pAC, SK_ERRCL_SW | SK_ERRCL_INIT, SKERR_SIRQ_E004,
			SKERR_SIRQ_E004MSG);
	}

	if ((Istatus & (IS_PA_TO_RX2 | IS_PA_TO_TX2)) &&
	    !pAC->GIni.GP[MAC_2].PState) {
		/* XMAC was not initialized but Packet timeout occured */
		SK_ERR_LOG(pAC, SK_ERRCL_SW | SK_ERRCL_INIT, SKERR_SIRQ_E005,
			SKERR_SIRQ_E005MSG);
	}

	if (Istatus & IS_PA_TO_RX1) {
		/* Means network is filling us up */
		SK_ERR_LOG(pAC, SK_ERRCL_HW | SK_ERRCL_INIT, SKERR_SIRQ_E002,
			SKERR_SIRQ_E002MSG);
		SK_OUT16(IoC, B3_PA_CTRL, PA_CLR_TO_RX1);
	}

	if (Istatus & IS_PA_TO_RX2) {
		/* Means network is filling us up */
		SK_ERR_LOG(pAC, SK_ERRCL_HW | SK_ERRCL_INIT, SKERR_SIRQ_E003,
			SKERR_SIRQ_E003MSG);
		SK_OUT16(IoC, B3_PA_CTRL, PA_CLR_TO_RX2);
	}

	if (Istatus & IS_PA_TO_TX1) {
		unsigned int	Len;
		SK_U64		Octets;
		SK_GEPORT	*pPrt = &pAC->GIni.GP[0];

		/* May be a normal situation in a server with a slow network */
		SK_OUT16(IoC, B3_PA_CTRL, PA_CLR_TO_TX1);

		if ((pPrt->PLinkModeStatus == SK_LMODE_STAT_HALF || 
		    pPrt->PLinkModeStatus == SK_LMODE_STAT_AUTOHALF) &&
		    !pPrt->HalfDupTimerActive) {
			/* 
			 * many more pack. arb. timeouts may come in between,
			 * we ignore those
			 */
			pPrt->HalfDupTimerActive = SK_TRUE;

			Len = sizeof(SK_U64);
			SkPnmiGetVar(pAC, IoC, OID_SKGE_STAT_TX_OCTETS, (char *) &Octets,
				&Len, (SK_U32) SK_PNMI_PORT_PHYS2INST(pAC, 0),
				pAC->Rlmt.Port[0].Net->NetNumber);
			pPrt->LastOctets = Octets;
			Para.Para32[0] = 0;
			SkTimerStart(pAC, IoC,
				&pPrt->HalfDupChkTimer,
				SK_HALFDUP_CHK_TIME,
				SKGE_HWAC,
				SK_HWEV_HALFDUP_CHK,
				Para);
		}
	}

	if (Istatus & IS_PA_TO_TX2) {
		unsigned int	Len;
		SK_U64		Octets;
		SK_GEPORT	*pPrt = &pAC->GIni.GP[1];

		/* May be a normal situation in a server with a slow network */
		SK_OUT16(IoC, B3_PA_CTRL, PA_CLR_TO_TX2);

		if ((pPrt->PLinkModeStatus == SK_LMODE_STAT_HALF || 
		    pPrt->PLinkModeStatus == SK_LMODE_STAT_AUTOHALF) &&
		    !pPrt->HalfDupTimerActive) {
			pPrt->HalfDupTimerActive = SK_TRUE;

			Len = sizeof(SK_U64);
			SkPnmiGetVar(pAC, IoC, OID_SKGE_STAT_TX_OCTETS, (char *) &Octets,
				&Len, (SK_U32) SK_PNMI_PORT_PHYS2INST(pAC, 1),
				pAC->Rlmt.Port[1].Net->NetNumber);
			pPrt->LastOctets = Octets;
			Para.Para32[0] = 1;
			SkTimerStart(pAC, IoC,
				&pPrt->HalfDupChkTimer,
				SK_HALFDUP_CHK_TIME,
				SKGE_HWAC,
				SK_HWEV_HALFDUP_CHK,
				Para);
		}
	}

	/*
	 * Check interrupts of the particular queues.
	 */
	if (Istatus & IS_R1_C) {
		/* Clear IRQ */
		SK_OUT32(IoC, B0_R1_CSR, CSR_IRQ_CL_C);
		SK_ERR_LOG(pAC, SK_ERRCL_SW | SK_ERRCL_INIT, SKERR_SIRQ_E006,
			SKERR_SIRQ_E006MSG);
		Para.Para64 = MAC_1;
		SkEventQueue(pAC, SKGE_DRV, SK_DRV_PORT_FAIL, Para);
		Para.Para32[0] = MAC_1;
		SkEventQueue(pAC, SKGE_RLMT, SK_RLMT_LINK_DOWN, Para);
	}

	if (Istatus & IS_R2_C) {
		/* Clear IRQ */
		SK_OUT32(IoC, B0_R2_CSR, CSR_IRQ_CL_C);
		SK_ERR_LOG(pAC, SK_ERRCL_SW | SK_ERRCL_INIT, SKERR_SIRQ_E007,
			SKERR_SIRQ_E007MSG);
		Para.Para64 = MAC_2;
		SkEventQueue(pAC, SKGE_DRV, SK_DRV_PORT_FAIL, Para);
		Para.Para32[0] = MAC_2;
		SkEventQueue(pAC, SKGE_RLMT, SK_RLMT_LINK_DOWN, Para);
	}

	if (Istatus & IS_XS1_C) {
		/* Clear IRQ */
		SK_OUT32(IoC, B0_XS1_CSR, CSR_IRQ_CL_C);
		SK_ERR_LOG(pAC, SK_ERRCL_SW | SK_ERRCL_INIT, SKERR_SIRQ_E008,
			SKERR_SIRQ_E008MSG);
		Para.Para64 = MAC_1;
		SkEventQueue(pAC, SKGE_DRV, SK_DRV_PORT_FAIL, Para);
		Para.Para32[0] = MAC_1;
		SkEventQueue(pAC, SKGE_RLMT, SK_RLMT_LINK_DOWN, Para);
	}

	if (Istatus & IS_XA1_C) {
		/* Clear IRQ */
		SK_OUT32(IoC, B0_XA1_CSR, CSR_IRQ_CL_C);
		SK_ERR_LOG(pAC, SK_ERRCL_SW | SK_ERRCL_INIT, SKERR_SIRQ_E009,
			SKERR_SIRQ_E009MSG);
		Para.Para64 = MAC_1;
		SkEventQueue(pAC, SKGE_DRV, SK_DRV_PORT_FAIL, Para);
		Para.Para32[0] = MAC_1;
		SkEventQueue(pAC, SKGE_RLMT, SK_RLMT_LINK_DOWN, Para);
	}

	if (Istatus & IS_XS2_C) {
		/* Clear IRQ */
		SK_OUT32(IoC, B0_XS2_CSR, CSR_IRQ_CL_C);
		SK_ERR_LOG(pAC, SK_ERRCL_SW | SK_ERRCL_INIT, SKERR_SIRQ_E010,
			SKERR_SIRQ_E010MSG);
		Para.Para64 = MAC_2;
		SkEventQueue(pAC, SKGE_DRV, SK_DRV_PORT_FAIL, Para);
		Para.Para32[0] = MAC_2;
		SkEventQueue(pAC, SKGE_RLMT, SK_RLMT_LINK_DOWN, Para);
	}

	if (Istatus & IS_XA2_C) {
		/* Clear IRQ */
		SK_OUT32(IoC, B0_XA2_CSR, CSR_IRQ_CL_C);
		SK_ERR_LOG(pAC, SK_ERRCL_SW | SK_ERRCL_INIT, SKERR_SIRQ_E011,
			SKERR_SIRQ_E011MSG);
		Para.Para64 = MAC_2;
		SkEventQueue(pAC, SKGE_DRV, SK_DRV_PORT_FAIL, Para);
		Para.Para32[0] = MAC_2;
		SkEventQueue(pAC, SKGE_RLMT, SK_RLMT_LINK_DOWN, Para);
	}

	/*
	 * External reg interrupt.
	 */
	if (Istatus & IS_EXT_REG) {
		SK_U16 	PhyInt;
		SK_U16 	PhyIMsk;
		int		i;
		SK_GEPORT	*pPrt;		/* GIni Port struct pointer */

		/* Test IRQs from PHY. */
		for (i = 0; i < pAC->GIni.GIMacsFound; i++) {
			pPrt = &pAC->GIni.GP[i];
			switch (pPrt->PhyType) {
			case SK_PHY_XMAC:
				break;
			case SK_PHY_BCOM:
				if (pPrt->PState) {
					PHY_READ(IoC, pPrt, i, PHY_BCOM_INT_STAT, &PhyInt);
					PHY_READ(IoC, pPrt, i, PHY_BCOM_INT_MASK, &PhyIMsk);

#ifdef xDEBUG
					if (PhyInt & PhyIMsk) {
						CMSMPrintString(
							pAC->pConfigTable,
							MSG_TYPE_RUNTIME_INFO,
							"SirqIsr - Stat: %x",
							(void *)PhyInt,
							(void *)NULL);
					}
#endif	/* DEBUG */
					
					if (PhyInt & ~PhyIMsk) {
						SK_DBG_MSG(
							pAC,
							SK_DBGMOD_HWM,
							SK_DBGCAT_IRQ,
							("Port %d Bcom Int: %x Mask: %x\n",
								i, PhyInt, PhyIMsk));
						SkPhyIsrBcom(pAC, IoC, i, PhyInt);
					}
				}
				break;
			case SK_PHY_LONE:
				PHY_READ(IoC, pPrt, i, PHY_LONE_INT_STAT, &PhyInt);
				PHY_READ(IoC, pPrt, i, PHY_LONE_INT_ENAB, &PhyIMsk);
				
				if (PhyInt & PhyIMsk) {
					SK_DBG_MSG(
						pAC,
						SK_DBGMOD_HWM,
						SK_DBGCAT_IRQ,
						("Port %d  Lone Int: %x Mask: %x\n",
						i, PhyInt, PhyIMsk));
					SkPhyIsrLone(pAC, IoC, i, PhyInt);
				}
				break;
			case SK_PHY_NAT:
				/* todo: National */
				break;
			}
		}
	}

	/*
	 * I2C Ready interrupt
	 */
	if (Istatus & IS_I2C_READY) {
		SkI2cIsr(pAC, IoC);
	}

	if (Istatus & IS_LNK_SYNC_M1) {
		/*
		 * We do NOT need the Link Sync interrupt, because it shows
		 * us only a link going down.
		 */
		/* clear interrupt */
		SK_OUT8(IoC, MR_ADDR(MAC_1, LNK_SYNC_CTRL), LED_CLR_IRQ);
	}

	/* Check MAC after link sync counter */
	if (Istatus & IS_MAC1) {
		XM_IN16(IoC, MAC_1, XM_ISRC, &XmIsr);
		SkXmIrq(pAC, IoC, MAC_1, XmIsr);
	}

	if (Istatus & IS_LNK_SYNC_M2) {
		/*
		 * We do NOT need the Link Sync interrupt, because it shows
		 * us only a link going down.
		 */
		/* clear interrupt */
		SK_OUT8(IoC, MR_ADDR(MAC_2,LNK_SYNC_CTRL), LED_CLR_IRQ);
	}

	/* Check MAC after link sync counter */
	if (Istatus & IS_MAC2) {
		XM_IN16(IoC, MAC_2, XM_ISRC, &XmIsr);
		SkXmIrq(pAC, IoC, MAC_2, XmIsr);
	}

	/*
	 * Timer interrupt
	 *  To be served last
	 */
	if (Istatus & IS_TIMINT) {
		SkHwtIsr(pAC, IoC);
	}
}	/* SkGeSirqIsr */


int	SkGePortCheckShorts(
SK_AC	*pAC,		/* Adapter Context */
SK_IOC	IoC,		/* IO Context */
int		Port)		/* Which port should be checked */
{
	SK_U64		Shorts;			/* Short Event Counter */
	SK_U64		CheckShorts;	/* Check value for Short Event Counter */
	SK_U64		RxCts;			/* RX Counter (packets on network) */
	SK_U64		RxTmp;			/* RX temp. Counter */
	SK_U64		FcsErrCts;		/* FCS Error Counter */
	SK_GEPORT	*pPrt;			/* GIni Port struct pointer */
	unsigned 	Len;
	int			Rtv;			/* Return value */
	int			i;

	pPrt = &pAC->GIni.GP[Port];

	/* Default: no action */
	Rtv = SK_HW_PS_NONE;

	/*
	 * Extra precaution: check for short Event counter
	 */
	Len = sizeof(SK_U64);
	SkPnmiGetVar(pAC, IoC, OID_SKGE_STAT_RX_SHORTS, (char *)&Shorts,
		&Len, (SK_U32)SK_PNMI_PORT_PHYS2INST(pAC, Port),
		pAC->Rlmt.Port[Port].Net->NetNumber);

	/*
	 * Read RX counter (packets seen on the network and not neccesarily
	 * really received.
	 */
	Len = sizeof(SK_U64);
	RxCts = 0;

	for (i = 0; i < sizeof(SkGeRxOids)/sizeof(SK_U32); i++) {
		SkPnmiGetVar(pAC, IoC, SkGeRxOids[i], (char *)&RxTmp,
			&Len, (SK_U32)SK_PNMI_PORT_PHYS2INST(pAC, Port),
			pAC->Rlmt.Port[Port].Net->NetNumber);
		RxCts += RxTmp;
	}

	/* On default: check shorts against zero */
	CheckShorts = 0;

	/*
	 * Extra extra precaution on active links:
	 */
	if (pPrt->PHWLinkUp) {
		/*
		 * Reset Link Restart counter
		 */
		pPrt->PLinkResCt = 0;
		pPrt->PAutoNegTOCt = 0;

		/* If link is up check for 2 */
		CheckShorts = 2;

		Len = sizeof(SK_U64);
		SkPnmiGetVar(pAC, IoC, OID_SKGE_STAT_RX_FCS,
			(char *)&FcsErrCts, &Len, (SK_U32)SK_PNMI_PORT_PHYS2INST(pAC, Port),
			pAC->Rlmt.Port[Port].Net->NetNumber);
		
		if (pPrt->PLinkModeConf == SK_LMODE_AUTOSENSE &&
		    pPrt->PLipaAutoNeg == SK_LIPA_UNKNOWN &&
		    (pPrt->PLinkMode == SK_LMODE_HALF ||
			 pPrt->PLinkMode == SK_LMODE_FULL)) {
			/*
			 * This is autosensing and we are in the fallback
			 * manual full/half duplex mode.
			 */
			if (RxCts == pPrt->PPrevRx) {
				/*
				 * Nothing received
				 * restart link
				 */
				pPrt->PPrevFcs = FcsErrCts;
				pPrt->PPrevShorts = Shorts;
				return (SK_HW_PS_RESTART);
			}
			else {
				pPrt->PLipaAutoNeg = SK_LIPA_MANUAL;
			}
		}

		if (((RxCts - pPrt->PPrevRx) > pPrt->PRxLim) ||
		    (!(FcsErrCts - pPrt->PPrevFcs))) {
			/*
			 * Note: The compare with zero above has to be done the way shown,
			 * otherwise the Linux driver will have a problem.
			 */
			/*
			 * We received a bunch of frames or no CRC error occured on the
			 * network -> ok.
			 */
			pPrt->PPrevRx = RxCts;
			pPrt->PPrevFcs = FcsErrCts;
			pPrt->PPrevShorts = Shorts;

			return (SK_HW_PS_NONE);
		}

		pPrt->PPrevFcs = FcsErrCts;
	}


	if ((Shorts - pPrt->PPrevShorts) > CheckShorts) {
		SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_IRQ,
			("Short Event Count Restart Port %d \n", Port));
		Rtv = SK_HW_PS_RESTART;
	}

	pPrt->PPrevShorts = Shorts;
	pPrt->PPrevRx = RxCts;

	return (Rtv);
}	/* SkGePortCheckShorts*/


int	SkGePortCheckUp(
SK_AC	*pAC,		/* Adapter Context */
SK_IOC	IoC,		/* IO Context */
int		Port)		/* Which port should be checked */
{
	switch (pAC->GIni.GP[Port].PhyType) {
	case SK_PHY_XMAC:
		return (SkGePortCheckUpXmac(pAC, IoC, Port));
	case SK_PHY_BCOM:
		return (SkGePortCheckUpBcom(pAC, IoC, Port));
	case SK_PHY_LONE:
		return (SkGePortCheckUpLone(pAC, IoC, Port));
	case SK_PHY_NAT:
		return (SkGePortCheckUpNat(pAC, IoC, Port));
	}
	return (SK_HW_PS_NONE);
}	/* SkGePortCheckUp */


static int	SkGePortCheckUpXmac(
SK_AC	*pAC,		/* Adapter Context */
SK_IOC	IoC,		/* IO Context */
int		Port)		/* Which port should be checked */
{
	SK_U64		Shorts;		/* Short Event Counter */
	SK_GEPORT	*pPrt;		/* GIni Port struct pointer */
	unsigned	Len;
	int			Done;
	SK_U32		GpReg;		/* General Purpose register value */
	SK_U16		Isrc;		/* Interrupt source register */
	SK_U16		IsrcSum;	/* Interrupt source register sum */
	SK_U16		LpAb;		/* Link Partner Ability */
	SK_U16		ResAb;		/* Resolved Ability */
	SK_U16		ExtStat;	/* Extended Status Register */
	SK_BOOL		AutoNeg;	/* Is Autonegotiation used ? */
	SK_U8		NextMode;	/* Next AutoSensing Mode */

	pPrt = &pAC->GIni.GP[Port];

	if (pPrt->PHWLinkUp) {
		if (pPrt->PhyType != SK_PHY_XMAC) {
			return (SK_HW_PS_NONE);
		}
		else {
			return (SkGePortCheckShorts(pAC, IoC, Port));
		}
	}

	IsrcSum = pPrt->PIsave;
	pPrt->PIsave = 0;

	/* Now wait for each port's link. */
	if (pPrt->PLinkMode == SK_LMODE_HALF ||
	    pPrt->PLinkMode == SK_LMODE_FULL) {
		AutoNeg = SK_FALSE;
	}
	else {
		AutoNeg = SK_TRUE;
	}

	if (pPrt->PLinkBroken) {
		/* Link was broken */
		XM_IN32(IoC,Port,XM_GP_PORT, &GpReg);

		if ((GpReg & XM_GP_INP_ASS) == 0) {
			/* The Link is in sync */
			XM_IN16(IoC,Port,XM_ISRC, &Isrc);
			IsrcSum |= Isrc;
			SkXmAutoNegLipaXmac(pAC, IoC, Port, IsrcSum);
			if ((Isrc & XM_IS_INP_ASS) == 0) {
				/* It has been in sync since last Time */
				/* Restart the PORT */
				SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_IRQ,
					("Link in sync Restart Port %d\n", Port));

				/* We now need to reinitialize the PrevShorts counter. */
				Len = sizeof(SK_U64);
				SkPnmiGetVar(pAC, IoC, OID_SKGE_STAT_RX_SHORTS, (char *)&Shorts,
					&Len, (SK_U32)SK_PNMI_PORT_PHYS2INST(pAC, Port),
					pAC->Rlmt.Port[Port].Net->NetNumber);
				pPrt->PPrevShorts = Shorts;

				pAC->GIni.GP[Port].PLinkBroken = SK_FALSE;

				pAC->GIni.GP[Port].PLinkResCt ++;
				pPrt->PAutoNegTimeOut = 0;

				if (pAC->GIni.GP[Port].PLinkResCt < SK_MAX_LRESTART) {
					return (SK_HW_PS_RESTART);
				}

				SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
					("Do NOT restart on Port %d %x %x\n", Port, Isrc, IsrcSum));
				pAC->GIni.GP[Port].PLinkResCt = 0;
			}
			else {
				pPrt->PIsave = (SK_U16)(IsrcSum & (XM_IS_AND));
				SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
					("Save Sync/nosync Port %d %x %x\n", Port, Isrc, IsrcSum));

				/* Do nothing more if link is broken */
				return (SK_HW_PS_NONE);
			}
		}
		else {
			/* Do nothing more if link is broken */
			return (SK_HW_PS_NONE);
		}

	}
	else {
		/* Link was not broken, check if it is */
		XM_IN16(IoC, Port, XM_ISRC, &Isrc);
		IsrcSum |= Isrc;
		if ((Isrc & XM_IS_INP_ASS) == XM_IS_INP_ASS) {
			XM_IN16(IoC, Port, XM_ISRC, &Isrc);
			IsrcSum |= Isrc;
			if ((Isrc & XM_IS_INP_ASS) == XM_IS_INP_ASS) {
				XM_IN16(IoC, Port, XM_ISRC, &Isrc);
				IsrcSum |= Isrc;
				if ((Isrc & XM_IS_INP_ASS) == XM_IS_INP_ASS) {
					pPrt->PLinkBroken = SK_TRUE;
					/*
					 * Re-Init Link partner Autoneg flag
					 */
					pPrt->PLipaAutoNeg = SK_LIPA_UNKNOWN;
					SK_DBG_MSG(pAC,SK_DBGMOD_HWM, SK_DBGCAT_IRQ,
						("Link broken Port %d\n", Port));

					/* Cable removed-> reinit sense mode. */
					/* Init default sense mode. */
					SkHWInitDefSense(pAC, IoC, Port);

					return (SK_HW_PS_RESTART);
				}
			}
		}
		else {
			SkXmAutoNegLipaXmac(pAC, IoC, Port, Isrc);
			if (SkGePortCheckShorts(pAC, IoC, Port) == SK_HW_PS_RESTART) {
				return (SK_HW_PS_RESTART);
			}
		}
	}

	/*
	 * here we usually can check whether the link is in sync and
	 * autonegotiation is done.
	 */
	XM_IN32(IoC, Port, XM_GP_PORT, &GpReg);
	XM_IN16(IoC, Port, XM_ISRC, &Isrc);
	IsrcSum |= Isrc;

	SkXmAutoNegLipaXmac(pAC, IoC, Port, IsrcSum);
	if ((GpReg & XM_GP_INP_ASS) != 0 || (IsrcSum & XM_IS_INP_ASS) != 0) {
		if ((GpReg & XM_GP_INP_ASS) == 0) {
			/* Save Autonegotiation Done interrupt only if link is in sync. */
			pPrt->PIsave = (SK_U16)(IsrcSum & (XM_IS_AND));
		}
#ifdef	DEBUG
		if (pPrt->PIsave & (XM_IS_AND)) {
			SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
				("AutoNeg done rescheduled Port %d\n", Port));
		}
#endif
		return (SK_HW_PS_NONE);
	}

	if (AutoNeg) {
		if (IsrcSum & XM_IS_AND) {
			SkHWLinkUp(pAC, IoC, Port);
			Done = SkXmAutoNegDone(pAC,IoC,Port);
			if (Done != SK_AND_OK) {
				/* Get PHY parameters, for debuging only */
				PHY_READ(IoC, pPrt, Port, PHY_XMAC_AUNE_LP, &LpAb);
				PHY_READ(IoC, pPrt, Port, PHY_XMAC_RES_ABI, &ResAb);
				SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
					("AutoNeg FAIL Port %d (LpAb %x, ResAb %x)\n",
					 Port, LpAb, ResAb));
					
				/* Try next possible mode */
				NextMode = SkHWSenseGetNext(pAC, IoC, Port);
				SkHWLinkDown(pAC, IoC, Port);
				if (Done == SK_AND_DUP_CAP) {
					/* GoTo next mode */
					SkHWSenseSetNext(pAC, IoC, Port, NextMode);
				}

				return (SK_HW_PS_RESTART);
			}
			else {
				/*
				 * Dummy Read extended status to prevent extra link down/ups
				 * (clear Page Received bit if set)
				 */
				PHY_READ(IoC, pPrt, Port, PHY_XMAC_AUNE_EXP, &ExtStat);
				SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
					("AutoNeg done Port %d\n", Port));
				return (SK_HW_PS_LINK);
			}
		} 
		
		/*
		 * AutoNeg not done, but HW link is up. Check for timeouts
		 */
		pPrt->PAutoNegTimeOut ++;
		if (pPrt->PAutoNegTimeOut >= SK_AND_MAX_TO) {
			/*
			 * Increase the Timeout counter.
			 */
			pPrt->PAutoNegTOCt ++;

			/*
			 * Timeout occured.
			 * What do we need now?
			 */
			SK_DBG_MSG(pAC,SK_DBGMOD_HWM,
				SK_DBGCAT_IRQ,
				("AutoNeg timeout Port %d\n",
				 Port));
			if (pPrt->PLinkModeConf == SK_LMODE_AUTOSENSE &&
				pPrt->PLipaAutoNeg != SK_LIPA_AUTO) {
				/*
				 * Timeout occured
				 * Set Link manually up.
				 */
				SkHWSenseSetNext(pAC, IoC, Port, SK_LMODE_FULL);
				SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_IRQ,
					("Set manual full duplex Port %d\n", Port));
			}

			if (pPrt->PLinkModeConf == SK_LMODE_AUTOSENSE &&
				pPrt->PLipaAutoNeg == SK_LIPA_AUTO &&
				pPrt->PAutoNegTOCt >= SK_MAX_ANEG_TO) {
				/*
				 * This is rather complicated.
				 * we need to check here whether the LIPA_AUTO
				 * we saw before is false alert. We saw at one 
				 * switch ( SR8800) that on boot time it sends
				 * just one autoneg packet and does no further
				 * autonegotiation.
				 * Solution: we restart the autosensing after
				 * a few timeouts.
				 */
				pPrt->PAutoNegTOCt = 0;
				pPrt->PLipaAutoNeg = SK_LIPA_UNKNOWN;
				SkHWInitDefSense(pAC, IoC, Port);
			}

			/*
			 * Do the restart
			 */
			return (SK_HW_PS_RESTART);
		}
	}
	else {
		/*
		 * Link is up and we don't need more.
		 */
#ifdef	DEBUG
		if (pPrt->PLipaAutoNeg == SK_LIPA_AUTO) {
			SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_CTRL,
				("ERROR: Lipa auto detected on port %d\n", Port));
		}
#endif

		SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_IRQ,
			("Link sync(GP), Port %d\n", Port));
		SkHWLinkUp(pAC, IoC, Port);
		return (SK_HW_PS_LINK);
	}

	return (SK_HW_PS_NONE);
}	/* SkGePortCheckUpXmac */


/******************************************************************************
 *
 * SkGePortCheckUpBcom - Check, if the link is up
 *
 * return:
 *	0	o.k. nothing needed
 *	1	Restart needed on this port
 *	2	Link came up
 */
static int	SkGePortCheckUpBcom(
SK_AC	*pAC,	/* Adapter Context */
SK_IOC	IoC,	/* IO Context */
int		Port)	/* Which port should be checked */
{
	SK_GEPORT	*pPrt;		/* GIni Port struct pointer */
	int			Done;
	SK_U16		Isrc;		/* Interrupt source register */
	SK_U16		PhyStat;	/* Phy Status Register */
	SK_U16		ResAb;		/* Master/Slave resolution */
	SK_U16		Ctrl;		/* Broadcom control flags */
#ifdef DEBUG
	SK_U16		LpAb;
	SK_U16		ExtStat;
#endif	/* DEBUG */
	SK_BOOL		AutoNeg;	/* Is Autonegotiation used ? */

	pPrt = &pAC->GIni.GP[Port];

	/* Check for No HCD Link events (#10523) */
	PHY_READ(IoC, pPrt, Port, PHY_BCOM_INT_STAT, &Isrc);

#ifdef xDEBUG
	if ((Isrc & ~0x1800) == 0x70) {
		SK_U32	Stat1, Stat2, Stat3;

		Stat1 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_INT_MASK, &Stat1);
		CMSMPrintString(
			pAC->pConfigTable,
			MSG_TYPE_RUNTIME_INFO,
			"CheckUp1 - Stat: %x, Mask: %x",
			(void *)Isrc,
			(void *)Stat1);

		Stat1 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_CTRL, &Stat1);
		Stat2 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_STAT, &Stat2);
		Stat1 = Stat1 << 16 | Stat2;
		Stat2 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_AUNE_ADV, &Stat2);
		Stat3 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_AUNE_LP, &Stat3);
		Stat2 = Stat2 << 16 | Stat3;
		CMSMPrintString(
			pAC->pConfigTable,
			MSG_TYPE_RUNTIME_INFO,
			"Ctrl/Stat: %x, AN Adv/LP: %x",
			(void *)Stat1,
			(void *)Stat2);

		Stat1 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_AUNE_EXP, &Stat1);
		Stat2 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_EXT_STAT, &Stat2);
		Stat1 = Stat1 << 16 | Stat2;
		Stat2 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_1000T_CTRL, &Stat2);
		Stat3 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_1000T_STAT, &Stat3);
		Stat2 = Stat2 << 16 | Stat3;
		CMSMPrintString(
			pAC->pConfigTable,
			MSG_TYPE_RUNTIME_INFO,
			"AN Exp/IEEE Ext: %x, 1000T Ctrl/Stat: %x",
			(void *)Stat1,
			(void *)Stat2);

		Stat1 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_P_EXT_CTRL, &Stat1);
		Stat2 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_P_EXT_STAT, &Stat2);
		Stat1 = Stat1 << 16 | Stat2;
		Stat2 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_AUX_CTRL, &Stat2);
		Stat3 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_AUX_STAT, &Stat3);
		Stat2 = Stat2 << 16 | Stat3;
		CMSMPrintString(
			pAC->pConfigTable,
			MSG_TYPE_RUNTIME_INFO,
			"PHY Ext Ctrl/Stat: %x, Aux Ctrl/Stat: %x",
			(void *)Stat1,
			(void *)Stat2);
	}
#endif	/* DEBUG */

	if ((Isrc & (PHY_B_IS_NO_HDCL /* | PHY_B_IS_NO_HDC */)) != 0) {
		PHY_READ(IoC, pPrt, Port, PHY_BCOM_CTRL, &Ctrl);
		PHY_WRITE(IoC, pPrt, Port, PHY_BCOM_CTRL, Ctrl | PHY_CT_LOOP);
		PHY_WRITE(IoC, pPrt, Port, PHY_BCOM_CTRL, Ctrl & ~PHY_CT_LOOP);
		SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_CTRL,
			("No HCD Link event, Port %d\n", Port));
#ifdef xDEBUG
		CMSMPrintString(
			pAC->pConfigTable,
			MSG_TYPE_RUNTIME_INFO,
			"No HCD link event, port %d.",
			(void *)Port,
			(void *)NULL);
#endif	/* DEBUG */
	}

	/* Not obsolete: link status bit is latched to 0 and autoclearing! */
	PHY_READ(IoC, pPrt, Port, PHY_BCOM_STAT, &PhyStat);

	if (pPrt->PHWLinkUp) {
		return (SK_HW_PS_NONE);
	}

#ifdef xDEBUG
	{
		SK_U32	Stat1, Stat2, Stat3;

		Stat1 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_INT_MASK, &Stat1);
		CMSMPrintString(
			pAC->pConfigTable,
			MSG_TYPE_RUNTIME_INFO,
			"CheckUp1a - Stat: %x, Mask: %x",
			(void *)Isrc,
			(void *)Stat1);

		Stat1 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_CTRL, &Stat1);
		Stat2 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_STAT, &PhyStat);
		Stat1 = Stat1 << 16 | PhyStat;
		Stat2 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_AUNE_ADV, &Stat2);
		Stat3 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_AUNE_LP, &Stat3);
		Stat2 = Stat2 << 16 | Stat3;
		CMSMPrintString(
			pAC->pConfigTable,
			MSG_TYPE_RUNTIME_INFO,
			"Ctrl/Stat: %x, AN Adv/LP: %x",
			(void *)Stat1,
			(void *)Stat2);

		Stat1 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_AUNE_EXP, &Stat1);
		Stat2 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_EXT_STAT, &Stat2);
		Stat1 = Stat1 << 16 | Stat2;
		Stat2 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_1000T_CTRL, &Stat2);
		Stat3 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_1000T_STAT, &ResAb);
		Stat2 = Stat2 << 16 | ResAb;
		CMSMPrintString(
			pAC->pConfigTable,
			MSG_TYPE_RUNTIME_INFO,
			"AN Exp/IEEE Ext: %x, 1000T Ctrl/Stat: %x",
			(void *)Stat1,
			(void *)Stat2);

		Stat1 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_P_EXT_CTRL, &Stat1);
		Stat2 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_P_EXT_STAT, &Stat2);
		Stat1 = Stat1 << 16 | Stat2;
		Stat2 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_AUX_CTRL, &Stat2);
		Stat3 = 0;
		PHY_READ(pAC, pPrt, Port, PHY_BCOM_AUX_STAT, &Stat3);
		Stat2 = Stat2 << 16 | Stat3;
		CMSMPrintString(
			pAC->pConfigTable,
			MSG_TYPE_RUNTIME_INFO,
			"PHY Ext Ctrl/Stat: %x, Aux Ctrl/Stat: %x",
			(void *)Stat1,
			(void *)Stat2);
	}
#endif	/* DEBUG */

	/* Now wait for each port's link. */
	if (pPrt->PLinkMode == SK_LMODE_HALF || pPrt->PLinkMode == SK_LMODE_FULL) {
		AutoNeg = SK_FALSE;
	}
	else {
		AutoNeg = SK_TRUE;
	}

	/*
	 * Here we usually can check whether the link is in sync and
	 * autonegotiation is done.
	 */

	PHY_READ(IoC, pPrt, Port, PHY_BCOM_STAT, &PhyStat);

#ifdef xDEBUG
	if ((PhyStat & PHY_ST_LSYNC) >> 2 != (ExtStat & PHY_B_PES_LS) >> 8) {
		CMSMPrintString(
			pAC->pConfigTable,
			MSG_TYPE_RUNTIME_INFO,
			"PhyStat != ExtStat: %x %x",
			(void *)PhyStat,
			(void *)ExtStat);
	}
#endif	/* DEBUG */

	SkXmAutoNegLipaBcom(pAC, IoC, Port, PhyStat);
	
	SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_CTRL,
		("AutoNeg:%d, PhyStat: %Xh.\n", AutoNeg, PhyStat));

	PHY_READ(IoC, pPrt, Port, PHY_BCOM_1000T_STAT, &ResAb);

	if (ResAb & PHY_B_1000S_MSF) {
		/* Error */
		SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_CTRL,
			("Master/Slave Fault port %d\n", Port));
		pPrt->PAutoNegFail = SK_TRUE;
		pPrt->PMSStatus = SK_MS_STAT_FAULT;
		return (SK_HW_PS_RESTART);
	}

	if ((PhyStat & PHY_ST_LSYNC) == 0) {
		return (SK_HW_PS_NONE);
	}
	else if (ResAb & PHY_B_1000S_MSR) {
		pPrt->PMSStatus = SK_MS_STAT_MASTER;
	}
	else {
		pPrt->PMSStatus = SK_MS_STAT_SLAVE;
	}
	
	SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_CTRL,
		("AutoNeg:%d, PhyStat: %Xh.\n", AutoNeg, PhyStat));

	if (AutoNeg) {
		if (PhyStat & PHY_ST_AN_OVER) {
			SkHWLinkUp(pAC, IoC, Port);
			Done = SkXmAutoNegDone(pAC, IoC, Port);
			if (Done != SK_AND_OK) {
#ifdef DEBUG
				/* Get PHY parameters, for debugging only. */
				PHY_READ(IoC, pPrt, Port, PHY_BCOM_AUNE_LP, &LpAb);
				PHY_READ(IoC, pPrt, Port, PHY_BCOM_1000T_STAT, &ExtStat);
				SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_CTRL,
					("AutoNeg FAIL Port %d (LpAb %x, 1000TStat %x)\n",
					Port, LpAb, ExtStat));
#endif	/* DEBUG */
				return (SK_HW_PS_RESTART);
			}
			else {
#ifdef xDEBUG
				/* Dummy read ISR to prevent extra link downs/ups. */
				PHY_READ(IoC, pPrt, Port, PHY_BCOM_INT_STAT, &ExtStat);

				if ((ExtStat & ~0x1800) != 0) {
					CMSMPrintString(
						pAC->pConfigTable,
						MSG_TYPE_RUNTIME_INFO,
						"CheckUp2 - Stat: %x",
						(void *)ExtStat,
						(void *)NULL);
				}
#endif	/* DEBUG */
				
				SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_CTRL,
					("AutoNeg done Port %d\n", Port));
				return (SK_HW_PS_LINK);
			}
		} 
	}
	else {	/* !AutoNeg */
		/*
		 * Link is up and we don't need more.
		 */
#ifdef	DEBUG
		if (pPrt->PLipaAutoNeg == SK_LIPA_AUTO) {
			SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_CTRL,
				("ERROR: Lipa auto detected on port %d\n", Port));
		}
#endif

#ifdef xDEBUG
		/* Dummy read ISR to prevent extra link downs/ups. */
		PHY_READ(IoC, pPrt, Port, PHY_BCOM_INT_STAT, &ExtStat);

		if ((ExtStat & ~0x1800) != 0) {
			CMSMPrintString(
				pAC->pConfigTable,
				MSG_TYPE_RUNTIME_INFO,
				"CheckUp3 - Stat: %x",
				(void *)ExtStat,
				(void *)NULL);
		}
#endif	/* DEBUG */
		
		SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_IRQ,
			("Link sync(GP), Port %d\n", Port));
		SkHWLinkUp(pAC, IoC, Port);
		return (SK_HW_PS_LINK);
	}

	return (SK_HW_PS_NONE);
}	/* SkGePortCheckUpBcom */


/******************************************************************************
 *
 * SkGePortCheckUpLone - Check if the link is up
 *
 * return:
 *	0	o.k. nothing needed
 *	1	Restart needed on this port
 *	2	Link came up
 */
static int	SkGePortCheckUpLone(
SK_AC	*pAC,		/* Adapter Context */
SK_IOC	IoC,		/* IO Context */
int		Port)		/* Which port should be checked */
{
	SK_GEPORT	*pPrt;		/* GIni Port struct pointer */
	int		Done;
	SK_U16		Isrc;		/* Interrupt source register */
	SK_U16		LpAb;		/* Link Partner Ability */
	SK_U16		ExtStat;	/* Extended Status Register */
	SK_U16		PhyStat;	/* Phy Status Register */
	SK_U16		StatSum;
	SK_BOOL		AutoNeg;	/* Is Autonegotiation used ? */
	SK_U8		NextMode;	/* Next AutoSensing Mode */

	pPrt = &pAC->GIni.GP[Port];

	if (pPrt->PHWLinkUp) {
		return (SK_HW_PS_NONE);
	}

	StatSum = pPrt->PIsave;
	pPrt->PIsave = 0;

	/* Now wait for each ports link */
	if (pPrt->PLinkMode == SK_LMODE_HALF ||
	    pPrt->PLinkMode == SK_LMODE_FULL) {
		AutoNeg = SK_FALSE;
	}
	else {
		AutoNeg = SK_TRUE;
	}

	/*
	 * here we usually can check whether the link is in sync and
	 * autonegotiation is done.
	 */
	XM_IN16(IoC, Port, XM_ISRC, &Isrc);
	PHY_READ(IoC, pPrt, Port, PHY_LONE_STAT, &PhyStat);
	StatSum |= PhyStat;

	SkXmAutoNegLipaLone(pAC, IoC, Port, PhyStat);
	if ((PhyStat & PHY_ST_LSYNC) == 0){
		/*
		 * Save Autonegotiation Done bit
		 */
		pPrt->PIsave = (SK_U16)(StatSum & PHY_ST_AN_OVER);
#ifdef DEBUG
		if (pPrt->PIsave & PHY_ST_AN_OVER) {
			SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
				("AutoNeg done rescheduled Port %d\n", Port));
		}
#endif
		return (SK_HW_PS_NONE);
	}

	if (AutoNeg) {
		if (StatSum & PHY_ST_AN_OVER) {
			SkHWLinkUp(pAC, IoC, Port);
			Done = SkXmAutoNegDone(pAC,IoC,Port);
			if (Done != SK_AND_OK) {
				/* Get PHY parameters, for debuging only */
				PHY_READ(IoC, pPrt, Port,
					PHY_LONE_AUNE_LP,
					&LpAb);
				PHY_READ(IoC, pPrt, Port,
					PHY_LONE_1000T_STAT,
					&ExtStat);
				SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_CTRL,
					("AutoNeg FAIL Port %d (LpAb %x, 1000TStat %x)\n",
					 Port, LpAb, ExtStat));
					
				/* Try next possible mode */
				NextMode = SkHWSenseGetNext(pAC, IoC, Port);
				SkHWLinkDown(pAC, IoC, Port);
				if (Done == SK_AND_DUP_CAP) {
					/* GoTo next mode */
					SkHWSenseSetNext(pAC, IoC, Port, NextMode);
				}

				return (SK_HW_PS_RESTART);

			}
			else {
				/*
				 * Dummy Read interrupt status to prevent
				 * extra link down/ups
				 */
				PHY_READ(IoC, pPrt, Port, PHY_LONE_INT_STAT, &ExtStat);
				SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_CTRL,
					("AutoNeg done Port %d\n", Port));
				return (SK_HW_PS_LINK);
			}
		} 
		
		/*
		 * AutoNeg not done, but HW link is up. Check for timeouts
		 */
		pPrt->PAutoNegTimeOut ++;
		if (pPrt->PAutoNegTimeOut >= SK_AND_MAX_TO) {
			/*
			 * Timeout occured.
			 * What do we need now?
			 */
			SK_DBG_MSG(pAC,SK_DBGMOD_HWM,
				SK_DBGCAT_IRQ,
				("AutoNeg timeout Port %d\n",
				 Port));
			if (pPrt->PLinkModeConf == SK_LMODE_AUTOSENSE &&
				pPrt->PLipaAutoNeg != SK_LIPA_AUTO) {
				/*
				 * Timeout occured
				 * Set Link manually up.
				 */
				SkHWSenseSetNext(pAC, IoC, Port,
					SK_LMODE_FULL);
				SK_DBG_MSG(pAC,SK_DBGMOD_HWM,
					SK_DBGCAT_IRQ,
					("Set manual full duplex Port %d\n",
					 Port));
			}

			/*
			 * Do the restart
			 */
			return (SK_HW_PS_RESTART);
		}
	}
	else {
		/*
		 * Link is up and we don't need more.
		 */
#ifdef	DEBUG
		if (pPrt->PLipaAutoNeg == SK_LIPA_AUTO) {
			SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_CTRL,
				("ERROR: Lipa auto detected on port %d\n", Port));
		}
#endif

		/*
		 * Dummy Read interrupt status to prevent
		 * extra link down/ups
		 */
		PHY_READ(IoC, pPrt, Port, PHY_LONE_INT_STAT, &ExtStat);
		
		SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_IRQ,
			("Link sync(GP), Port %d\n", Port));
		SkHWLinkUp(pAC, IoC, Port);
		return (SK_HW_PS_LINK);
	}

	return (SK_HW_PS_NONE);
}	/* SkGePortCheckUpLone*/


/******************************************************************************
 *
 * SkGePortCheckUpNat - Check if the link is up
 *
 * return:
 *	0	o.k. nothing needed
 *	1	Restart needed on this port
 *	2	Link came up
 */
static int	SkGePortCheckUpNat(
SK_AC	*pAC,		/* Adapter Context */
SK_IOC	IoC,		/* IO Context */
int		Port)		/* Which port should be checked */
{
	/* todo: National */
	return (SK_HW_PS_NONE);
}	/* SkGePortCheckUpNat */


/******************************************************************************
 *
 *	Event service routine
 *
 * Description:
 *
 * Notes:
 */
int	SkGeSirqEvent(
SK_AC		*pAC,		/* Adapter Context */
SK_IOC		IoC,		/* Io Context */
SK_U32		Event,		/* Module specific Event */
SK_EVPARA	Para)		/* Event specific Parameter */
{
	SK_U64		Octets;
	SK_GEPORT	*pPrt;		/* GIni Port struct pointer */
	SK_U32		Port;
	SK_U32		Time;
	unsigned	Len;
	int			PortStat;
	SK_U8		Val8;

	Port = Para.Para32[0];
	pPrt = & pAC->GIni.GP[Port];

	switch (Event) {
	case SK_HWEV_WATIM:
		/* Check whether port came up */
		PortStat = SkGePortCheckUp(pAC, IoC, Port);

		switch (PortStat) {
		case SK_HW_PS_RESTART:
			if (pPrt->PHWLinkUp) {
				/*
				 * Set Link to down.
				 */
				SkHWLinkDown(pAC, IoC, Port);

				/*
				 * Signal directly to RLMT to ensure correct
				 * sequence of SWITCH and RESET event.
				 */
				SkRlmtEvent(pAC, IoC, SK_RLMT_LINK_DOWN, Para);
			}

			/* Restart needed */
			SkEventQueue(pAC, SKGE_DRV, SK_DRV_PORT_RESET, Para);
			break;

		case SK_HW_PS_LINK:
			/* Signal to RLMT */
			SkEventQueue(pAC, SKGE_RLMT, SK_RLMT_LINK_UP, Para);
			break;

		}
		
		/* Start again the check Timer */
		if (pPrt->PHWLinkUp) {
			Time = SK_WA_ACT_TIME;
		}
		else {
			Time = SK_WA_INA_TIME;
		}

		/* Todo: still needed for non-XMAC PHYs??? */
		SkTimerStart(pAC, IoC, &pAC->GIni.GP[Port].PWaTimer,
			Time, SKGE_HWAC, SK_HWEV_WATIM, Para);
		break;

	case SK_HWEV_PORT_START:
		if (pPrt->PHWLinkUp) {
			/*
			 * Signal directly to RLMT to ensure correct
			 * sequence of SWITCH and RESET event.
			 */
			SkRlmtEvent(pAC, IoC, SK_RLMT_LINK_DOWN, Para);
		}

		SkHWLinkDown(pAC, IoC, Port);

		/* Schedule Port RESET */
		SkEventQueue(pAC, SKGE_DRV, SK_DRV_PORT_RESET, Para);

		SkTimerStart(pAC, IoC, &pPrt->PWaTimer, SK_WA_INA_TIME,
			SKGE_HWAC, SK_HWEV_WATIM, Para);
		break;

	case SK_HWEV_PORT_STOP:
		if (pAC->GIni.GP[Port].PHWLinkUp) {
			/*
			 * Signal directly to RLMT to ensure correct
			 * sequence of SWITCH and RESET event.
			 */
			SkRlmtEvent(pAC, IoC, SK_RLMT_LINK_DOWN, Para);
		}

		SkTimerStop(pAC, IoC, &pPrt->PWaTimer);

		SkHWLinkDown(pAC, IoC, Port);
		break;

	case SK_HWEV_UPDATE_STAT:
		/* We do NOT need to update any statistics */
		break;

	case SK_HWEV_CLEAR_STAT:
		/* We do NOT need to clear any statistics */
		for (Port = 0; Port < (SK_U32)pAC->GIni.GIMacsFound; Port++) {
			pPrt->PPrevRx = 0;
			pPrt->PPrevFcs = 0;
			pPrt->PPrevShorts = 0;
		}
		break;

	case SK_HWEV_SET_LMODE:
		Val8 = (SK_U8)Para.Para32[1];
		if (pPrt->PLinkModeConf != Val8) {
			/* Set New link mode */
			pPrt->PLinkModeConf = Val8;

			/* Restart Port */
			SkEventQueue(pAC, SKGE_HWAC, SK_HWEV_PORT_STOP, Para);
			SkEventQueue(pAC, SKGE_HWAC, SK_HWEV_PORT_START, Para);
		}
		break;

	case SK_HWEV_SET_FLOWMODE:
		Val8 = (SK_U8)Para.Para32[1];
		if (pPrt->PFlowCtrlMode != Val8) {
			/* Set New Flow Control mode */
			pPrt->PFlowCtrlMode = Val8;

			/* Restart Port */
			SkEventQueue(pAC, SKGE_HWAC, SK_HWEV_PORT_STOP, Para);
			SkEventQueue(pAC, SKGE_HWAC, SK_HWEV_PORT_START, Para);
		}
		break;

	case SK_HWEV_SET_ROLE:
		Val8 = (SK_U8)Para.Para32[1];
		if (pPrt->PMSMode != Val8) {
			/* Set New link mode */
			pPrt->PMSMode = Val8;

			/* Restart Port */
			SkEventQueue(pAC, SKGE_HWAC, SK_HWEV_PORT_STOP, Para);
			SkEventQueue(pAC, SKGE_HWAC, SK_HWEV_PORT_START, Para);
		}
		break;

	case SK_HWEV_HALFDUP_CHK:
		pPrt->HalfDupTimerActive = SK_FALSE;
		if (pPrt->PLinkModeStatus == SK_LMODE_STAT_HALF || 
		    pPrt->PLinkModeStatus == SK_LMODE_STAT_AUTOHALF) {
			Len = sizeof(SK_U64);
			SkPnmiGetVar(pAC, IoC, OID_SKGE_STAT_TX_OCTETS, (char *)&Octets,
				&Len, (SK_U32)SK_PNMI_PORT_PHYS2INST(pAC, Port),
				pAC->Rlmt.Port[Port].Net->NetNumber);
			if (pPrt->LastOctets == Octets) {
				/* TX hanging, do a FIFO flush restarts it. */
				SkXmFlushTxFifo(pAC, IoC, Port);
			}
		}
		break;
	default:
		SK_ERR_LOG(pAC, SK_ERRCL_SW, SKERR_SIRQ_E001, SKERR_SIRQ_E001MSG);
		break;
	}

	return (0);
}	/* SkGeSirqEvent */


/******************************************************************************
 *
 *	SkPhyIsrBcom - PHY interrupt service routine
 *
 * Description: handle all interrupts from BCOM PHY
 *
 * Returns: N/A
 */
static void SkPhyIsrBcom(
SK_AC		*pAC,		/* Adapter Context */
SK_IOC		IoC,		/* Io Context */
int			Port,		/* Port Num = PHY Num */
SK_U16		IStatus)	/* Interrupt Status */
{
	SK_GEPORT	*pPrt;		/* GIni Port struct pointer */
	SK_EVPARA	Para;

	pPrt = &pAC->GIni.GP[Port];

	if (IStatus & PHY_B_IS_PSE) {
		/* Incorrectable pair swap error. */
		SK_ERR_LOG(pAC, SK_ERRCL_SW | SK_ERRCL_INIT, SKERR_SIRQ_E022,
			SKERR_SIRQ_E022MSG);
	}
	
	if (IStatus & PHY_B_IS_MDXI_SC) {
		/* not used */
	}
	
	if (IStatus & PHY_B_IS_HCT) {
		/* not used */
	}
	
	if (IStatus & PHY_B_IS_LCT) {
		/* not used */
	}
	
	if (IStatus & (PHY_B_IS_AN_PR | PHY_B_IS_LST_CHANGE)) {
		Para.Para32[0] = (SK_U32)Port;

		SkHWLinkDown(pAC, IoC, Port);

		/* Signal to RLMT */
		SkEventQueue(pAC, SKGE_RLMT, SK_RLMT_LINK_DOWN, Para);

		SkTimerStart(pAC, IoC, &pPrt->PWaTimer,
			SK_WA_INA_TIME, SKGE_HWAC, SK_HWEV_WATIM, Para);
	}

	if (IStatus & PHY_B_IS_NO_HDCL) {
	}

	if (IStatus & PHY_B_IS_NO_HDC) {
		/* not used */
	}

	if (IStatus & PHY_B_IS_NEG_USHDC) {
		/* not used */
	}

	if (IStatus & PHY_B_IS_SCR_S_ER) {
		/* not used */
	}

	if (IStatus & PHY_B_IS_RRS_CHANGE) {
		/* not used */
	}

	if (IStatus & PHY_B_IS_LRS_CHANGE) {
		/* not used */
	}

	if (IStatus & PHY_B_IS_DUP_CHANGE) {
		/* not used */
	}

	if (IStatus & PHY_B_IS_LSP_CHANGE) {
		/* not used */
	}

	if (IStatus & PHY_B_IS_CRC_ER) {
		/* not used */
	}
}	/* SkPhyIsrBcom */


/******************************************************************************
 *
 *	SkPhyIsrLone - PHY interrupt service routine
 *
 * Description: handle all interrupts from LONE PHY
 *
 * Returns: N/A
 */
static void SkPhyIsrLone(
SK_AC	*pAC,		/* Adapter Context */
SK_IOC	IoC,		/* Io Context */
int		Port,		/* Port Num = PHY Num */
SK_U16	IStatus)	/* Interrupt Status */
{
	SK_EVPARA	Para;

	if (IStatus & PHY_L_IS_CROSS) {
		/* not used */
	}
	
	if (IStatus & PHY_L_IS_POL) {
		/* not used */
	}
	
	if (IStatus & PHY_L_IS_SS) {
		/* not used */
	}
	
	if (IStatus & PHY_L_IS_CFULL) {
		/* not used */
	}
	
	if (IStatus & PHY_L_IS_AN_C) {
		/* not used */
	}
	
	if (IStatus & PHY_L_IS_SPEED) {
		/* not used */
	}
	
	if (IStatus & PHY_L_IS_CFULL) {
		/* not used */
	}
	
	if (IStatus & (PHY_L_IS_DUP | PHY_L_IS_ISOL)) {
		SkHWLinkDown(pAC, IoC, Port);

		/* Signal to RLMT */
		Para.Para32[0] = (SK_U32)Port;
		SkEventQueue(pAC, SKGE_RLMT, SK_RLMT_LINK_DOWN, Para);

		SkTimerStart(pAC, IoC, &pAC->GIni.GP[Port].PWaTimer,
			SK_WA_INA_TIME, SKGE_HWAC, SK_HWEV_WATIM, Para);
	}

	if (IStatus & PHY_L_IS_MDINT) {
		/* not used */
	}
}	/* SkPhyIsrLone */

/* End of File */
