/******************************************************************************
 *
 * Name:	skxmac2.c
 * Project:	GEnesis, PCI Gigabit Ethernet Adapter
 * Version:	$Revision: 1.1.1.1 $
 * Date:	$Date: 2010/03/05 07:31:24 $
 * Purpose:	Contains functions to initialize the XMAC II
 *
 ******************************************************************************/

/******************************************************************************
 *
 *	(C)Copyright 1998-2001 SysKonnect GmbH.
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	The information in this file is provided "AS IS" without warranty.
 *
 ******************************************************************************/


#include "h/skdrv1st.h"
#include "h/xmac_ii.h"
#include "h/skdrv2nd.h"

/* defines ********************************************************************/
/* typedefs *******************************************************************/
/* global variables ***********************************************************/

/* local variables ************************************************************/

static const char SysKonnectFileId[] =
	"@(#)$Id: skxmac2.c,v 1.1.1.1 2010/03/05 07:31:24 reynolds Exp $ (C) SK ";

/* BCOM PHY magic pattern list */
typedef struct s_PhyHack {
	int		PhyReg;		/* Phy register */
	SK_U16	PhyVal;		/* Value to write */
} BCOM_HACK;

BCOM_HACK BcomRegA1Hack[] = {
 { 0x18, 0x0c20 }, { 0x17, 0x0012 }, { 0x15, 0x1104 }, { 0x17, 0x0013 },
 { 0x15, 0x0404 }, { 0x17, 0x8006 }, { 0x15, 0x0132 }, { 0x17, 0x8006 },
 { 0x15, 0x0232 }, { 0x17, 0x800D }, { 0x15, 0x000F }, { 0x18, 0x0420 },
 { 0, 0 }
};
BCOM_HACK BcomRegC0Hack[] = {
 { 0x18, 0x0c20 }, { 0x17, 0x0012 }, { 0x15, 0x1204 }, { 0x17, 0x0013 },
 { 0x15, 0x0A04 }, { 0x18, 0x0420 },
 { 0, 0 }
};

/* function prototypes ********************************************************/
static void	SkXmInitPhyXmac(SK_AC*, SK_IOC, int, SK_BOOL);
static void	SkXmInitPhyBcom(SK_AC*, SK_IOC, int, SK_BOOL);
static void	SkXmInitPhyLone(SK_AC*, SK_IOC, int, SK_BOOL);
static void	SkXmInitPhyNat (SK_AC*, SK_IOC, int, SK_BOOL);
static int	SkXmAutoNegDoneXmac(SK_AC*, SK_IOC, int);
static int	SkXmAutoNegDoneBcom(SK_AC*, SK_IOC, int);
static int	SkXmAutoNegDoneLone(SK_AC*, SK_IOC, int);
static int	SkXmAutoNegDoneNat (SK_AC*, SK_IOC, int);

/******************************************************************************
 *
 *	SkXmSetRxCmd() - Modify the value of the XMACs Rx Command Register
 *
 * Description:
 *	The features
 *	 o FCS stripping,			SK_STRIP_FCS_ON/OFF
 *	 o pad byte stripping,			SK_STRIP_PAD_ON/OFF
 *	 o don't set XMR_FS_ERR in frame	SK_LENERR_OK_ON/OFF
 *	   status for inrange length error
 *	   frames, and
 *	 o don't set XMR_FS_ERR in frame	SK_BIG_PK_OK_ON/OFF
 *	   status for frames > 1514 bytes
 *
 *	for incomming packets may be enabled/disabled by this function.
 *	Additional modes may be added later.
 *	Multiple modes can be enabled/disabled at the same time.
 *	The new configuration is stored into the HWAC port configuration
 *	and is written to the Receive Command register immediatlely.
 *	The new configuration is saved over any SkGePortStop() and
 *	SkGeInitPort() calls. The configured value will be overwritten
 *	when SkGeInit(Level 0) is executed.
 *
 * Returns:
 *	nothing
 */
void SkXmSetRxCmd(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int		Port,		/* The XMAC to handle with belongs to this Port */
int		Mode)		/* Mode is SK_STRIP_FCS_ON/OFF, SK_STRIP_PAD_ON/OFF,
					   SK_LENERR_OK_ON/OFF, or SK_BIG_PK_OK_ON/OFF */
{
	SK_GEPORT	*pPrt;
	SK_U16		OldRxMode;

	pPrt = &pAC->GIni.GP[Port];
	OldRxMode = pPrt->PRxCmd;

	switch(Mode & (SK_STRIP_FCS_ON | SK_STRIP_FCS_OFF)) {
	case SK_STRIP_FCS_ON:
		pPrt->PRxCmd |= XM_RX_STRIP_FCS;
		break;
	case SK_STRIP_FCS_OFF:
		pPrt->PRxCmd &= ~XM_RX_STRIP_FCS;
		break;
	}

	switch(Mode & (SK_STRIP_PAD_ON | SK_STRIP_PAD_OFF)) {
	case SK_STRIP_PAD_ON:
		pPrt->PRxCmd |= XM_RX_STRIP_PAD;
		break;
	case SK_STRIP_PAD_OFF:
		pPrt->PRxCmd &= ~XM_RX_STRIP_PAD;
		break;
	}

	switch(Mode & (SK_LENERR_OK_ON | SK_LENERR_OK_OFF)) {
	case SK_LENERR_OK_ON:
		pPrt->PRxCmd |= XM_RX_LENERR_OK;
		break;
	case SK_LENERR_OK_OFF:
		pPrt->PRxCmd &= ~XM_RX_LENERR_OK;
		break;
	}

	switch(Mode & (SK_BIG_PK_OK_ON | SK_BIG_PK_OK_OFF)) {
	case SK_BIG_PK_OK_ON:
		pPrt->PRxCmd |= XM_RX_BIG_PK_OK;
		break;
	case SK_BIG_PK_OK_OFF:
		pPrt->PRxCmd &= ~XM_RX_BIG_PK_OK;
		break;
	}

	/* Write the new mode to the receive command register if required */
	if (OldRxMode != pPrt->PRxCmd) {
		XM_OUT16(IoC, Port, XM_RX_CMD, pPrt->PRxCmd);
	}
}	/* SkXmSetRxCmd*/


/******************************************************************************
 *
 *	SkXmClrExactAddr() - Clear Exact Match Address Registers
 *
 * Description:
 *	All Exact Match Address registers of the XMAC 'Port' will be
 *	cleared starting with 'StartNum' up to (and including) the
 *	Exact Match address number of 'StopNum'.
 *
 * Returns:
 *	nothing
 */
void SkXmClrExactAddr(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int		Port,		/* The XMAC to handle with belongs to this Port */
int		StartNum,	/* Begin with this Address Register Index (0..15) */
int		StopNum)	/* Stop after finished with this Register Idx (0..15) */
{
	int		i;
	SK_U16	ZeroAddr[3] = {0x0000, 0x0000, 0x0000};

	if ((unsigned)StartNum > 15 || (unsigned)StopNum > 15 ||
		StartNum > StopNum) {

		SK_ERR_LOG(pAC, SK_ERRCL_SW, SKERR_HWI_E001, SKERR_HWI_E001MSG);
		return;
	}

	for (i = StartNum; i <= StopNum; i++) {
		XM_OUTADDR(IoC, Port, XM_EXM(i), &ZeroAddr[0]);
	}
}	/* SkXmClrExactAddr */


/******************************************************************************
 *
 *	SkXmClrSrcCheck() - Clear Source Check Address Register
 *
 * Description:
 *	The Source Check Address Register of the XMAC 'Port' number
 *	will be cleared.
 *
 * Returns:
 *	nothing
 */
static void SkXmClrSrcCheck(
SK_AC	*pAC,	/* adapter context */
SK_IOC	IoC,	/* IO context */
int		Port)	/* The XMAC to handle with belongs to this Port (MAC_1 + n) */
{
	SK_U16	ZeroAddr[3] = {0x0000, 0x0000, 0x0000};

	XM_OUTHASH(IoC, Port, XM_SRC_CHK, &ZeroAddr);
}	/* SkXmClrSrcCheck */


/******************************************************************************
 *
 *	SkXmClrHashAddr() - Clear Hash Address Registers
 *
 * Description:
 *	The Hash Address Register of the XMAC 'Port' will be cleared.
 *
 * Returns:
 *	nothing
 */
static void SkXmClrHashAddr(
SK_AC	*pAC,	/* adapter context */
SK_IOC	IoC,	/* IO context */
int		Port)	/* The XMAC to handle with belongs to this Port (MAC_1 + n) */
{
	SK_U16	ZeroAddr[4] = {0x0000, 0x0000, 0x0000, 0x0000};

	XM_OUTHASH(IoC, Port, XM_HSM, &ZeroAddr);
}	/* SkXmClrHashAddr*/


/******************************************************************************
 *
 *	SkXmFlushTxFifo() - Flush the XMACs transmit FIFO
 *
 * Description:
 *	Flush the transmit FIFO of the XMAC specified by the index 'Port'
 *
 * Returns:
 *	nothing
 */
void SkXmFlushTxFifo(
SK_AC	*pAC,	/* adapter context */
SK_IOC	IoC,	/* IO context */
int		Port)	/* The XMAC to handle with belongs to this Port (MAC_1 + n) */
{
	SK_U32	MdReg;

	XM_IN32(IoC, Port, XM_MODE, &MdReg);
	MdReg |= XM_MD_FTF;
	XM_OUT32(IoC, Port, XM_MODE, MdReg);
}	/* SkXmFlushTxFifo */


/******************************************************************************
 *
 *	SkXmFlushRxFifo() - Flush the XMACs receive FIFO
 *
 * Description:
 *	Flush the receive FIFO of the XMAC specified by the index 'Port'
 *
 * Returns:
 *	nothing
 */
void SkXmFlushRxFifo(
SK_AC	*pAC,	/* adapter context */
SK_IOC	IoC,	/* IO context */
int		Port)	/* The XMAC to handle with belongs to this Port (MAC_1 + n) */
{
	SK_U32	MdReg;

	XM_IN32(IoC, Port, XM_MODE, &MdReg);
	MdReg |= XM_MD_FRF;
	XM_OUT32(IoC, Port, XM_MODE, MdReg);
}	/* SkXmFlushRxFifo*/


/******************************************************************************
 *
 *	SkXmSoftRst() - Do a XMAC software reset
 *
 * Description:
 *	The PHY registers should not be destroyed during this
 *	kind of software reset. Therefore the XMAC Software Reset
 *	(XM_GP_RES_MAC bit in XM_GP_PORT) must not be used!
 *
 *	The software reset is done by
 *		- disabling the Rx and Tx state maschine,
 *		- reseting the statistics module,
 *		- clear all other significant XMAC Mode,
 *		  Command, and Control Registers
 *		- clearing the Hash Register and the
 *		  Exact Match Address registers, and
 *		- flushing the XMAC's Rx and Tx FIFOs.
 *
 * Note:
 *	Another requirement when stopping the XMAC is to
 *	avoid sending corrupted frames on the network.
 *	Disabling the Tx state maschine will NOT interrupt
 *	the currently transmitted frame. But we must take care
 *	that the tx FIFO is cleared AFTER the current frame
 *	is complete sent to the network.
 *
 *	It takes about 12ns to send a frame with 1538 bytes.
 *	One PCI clock goes at least 15ns (66MHz). Therefore
 *	after reading XM_GP_PORT back, we are sure that the
 *	transmitter is disabled AND idle. And this means
 *	we may flush the transmit FIFO now.
 *
 * Returns:
 *	nothing
 */
void SkXmSoftRst(
SK_AC	*pAC,	/* adapter context */
SK_IOC	IoC,	/* IO context */
int		Port)	/* port to stop (MAC_1 + n) */
{
	SK_GEPORT	*pPrt;
	SK_U16		Word;

	pPrt = &pAC->GIni.GP[Port];

	/* disable the receiver and transmitter */
	XM_IN16(IoC, Port, XM_MMU_CMD, &Word);
	XM_OUT16(IoC, Port, XM_MMU_CMD, Word & ~(XM_MMU_ENA_RX|XM_MMU_ENA_TX));

	/* reset the statistics module */
	XM_OUT32(IoC, Port, XM_GP_PORT, XM_GP_RES_STAT);

	/*
	 * clear all other significant XMAC Mode,
	 * Command, and Control Registers
	 */
	XM_OUT16(IoC, Port, XM_IMSK, 0xffff);		/* disable all IRQs */
	XM_OUT32(IoC, Port, XM_MODE, 0x00000000);	/* clear Mode Reg */
	XM_OUT16(IoC, Port, XM_TX_CMD, 0x0000);		/* reset TX CMD Reg */
	XM_OUT16(IoC, Port, XM_RX_CMD, 0x0000);		/* reset RX CMD Reg */
	
	/* disable all PHY IRQs */
	switch (pAC->GIni.GP[Port].PhyType) {
	case SK_PHY_BCOM:
			PHY_WRITE(IoC, pPrt, Port, PHY_BCOM_INT_MASK, 0xffff);
			break;
		case SK_PHY_LONE:
			PHY_WRITE(IoC, pPrt, Port, PHY_LONE_INT_ENAB, 0x0);
			break;
		case SK_PHY_NAT:
			/* todo: National
			 PHY_WRITE(IoC, pPrt, Port, PHY_NAT_INT_MASK, 
				0xffff); */
			break;
	}

	/* clear the Hash Register */
	SkXmClrHashAddr(pAC, IoC, Port);

	/* clear the Exact Match Address registers */
	SkXmClrExactAddr(pAC, IoC, Port, 0, 15);
	SkXmClrSrcCheck(pAC, IoC, Port);

	/* flush the XMAC's Rx and Tx FIFOs */
	SkXmFlushTxFifo(pAC, IoC, Port);
	SkXmFlushRxFifo(pAC, IoC, Port);

	pAC->GIni.GP[Port].PState = SK_PRT_STOP;
}	/* SkXmSoftRst*/


/******************************************************************************
 *
 *	SkXmHardRst() - Do a XMAC hardware reset
 *
 * Description:
 *	The XMAC of the specified 'Port' and all connected devices
 *	(PHY and SERDES) will receive a reset signal on its *Reset
 *	pins.
 *	External PHYs must be reset be clearing a bit in the GPIO
 *	register (Timing requirements: Broadcom: 400ns, Level One:
 *	none, National: 80ns).
 *
 * ATTENTION:
 * 	It is absolutely neccessary to reset the SW_RST Bit first
 *	before calling this function.
 *
 * Returns:
 *	nothing
 */
void SkXmHardRst(
SK_AC	*pAC,	/* adapter context */
SK_IOC	IoC,	/* IO context */
int		Port)	/* port to stop (MAC_1 + n) */
{
	SK_U32	Reg;
	int		i;
	int		TOut;
	SK_U16	Word;

	for (i = 0; i < 4; i++) {
		/* TX_MFF_CTRL1 is a 32 bit register but only the lowest 16 */
		/* bit contains buttoms to press */
		SK_OUT16(IoC, MR_ADDR(Port, TX_MFF_CTRL1), (SK_U16)MFF_CLR_MAC_RST);

		TOut = 0;
		do {
			TOut ++;
			if (TOut > 10000) {
				/*
				 * Adapter seems to be in RESET state.
				 * Registers cannot be written.
				 */
				return;
			}

			SK_OUT16(IoC, MR_ADDR(Port, TX_MFF_CTRL1),
				(SK_U16) MFF_SET_MAC_RST);
			SK_IN16(IoC,MR_ADDR(Port,TX_MFF_CTRL1), &Word);
		} while ((Word & MFF_SET_MAC_RST) == 0);
	}

	/* For external PHYs there must be special handling */
	if (pAC->GIni.GP[Port].PhyType != SK_PHY_XMAC) {
		/* reset external PHY */
		SK_IN32(IoC, B2_GP_IO, &Reg);
		if (Port == 0) {
			Reg |= GP_DIR_0; /* set to output */
			Reg &= ~GP_IO_0;
		}
		else {
			Reg |= GP_DIR_2; /* set to output */
			Reg &= ~GP_IO_2;
		}
		SK_OUT32(IoC, B2_GP_IO, Reg);

		/* short delay */
		SK_IN32(IoC, B2_GP_IO, &Reg);
	}

	pAC->GIni.GP[Port].PState = SK_PRT_RESET;
}	/* SkXmHardRst */


/******************************************************************************
 *
 *	SkXmInitMac() - Initialize the XMAC II
 *
 * Description:
 *	Initialize all the XMAC of the specified port.
 *	The XMAC must be reset or stopped before calling this function.
 *
 * Note:
 *	The XMAC's Rx and Tx state machine is still disabled when returning.
 *
 * Returns:
 *	nothing
 */
void SkXmInitMac(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int		Port)		/* Port Index (MAC_1 + n) */
{
	SK_GEPORT	*pPrt;
	SK_U32		Reg;
	int			i;
	SK_U16		SWord;
	SK_U16		PhyId;

	pPrt = &pAC->GIni.GP[Port];

	if (pPrt->PState == SK_PRT_STOP) {
		/* Port State: SK_PRT_STOP */
		/* Verify that the reset bit is cleared */
		SK_IN16(IoC, MR_ADDR(Port, TX_MFF_CTRL1), &SWord);
		if (SWord & (SK_U16)MFF_SET_MAC_RST) {
			/* PState does not match HW state */
			SK_ERR_LOG(pAC, SK_ERRCL_SW, SKERR_HWI_E006, SKERR_HWI_E006MSG);
			/* Correct it. */
			pPrt->PState = SK_PRT_RESET;
		}
	}

	if (pPrt->PState == SK_PRT_RESET) {
		/*
		 * clear HW reset
		 * Note: The SW reset is self clearing, therefore there is
		 *	 nothing to do here.
		 */
		SK_OUT16(IoC, MR_ADDR(Port, TX_MFF_CTRL1), (SK_U16)MFF_CLR_MAC_RST);

		/* Ensure that XMAC reset release is done (errata from LReinbold?). */
		SK_IN16(IoC, MR_ADDR(Port, TX_MFF_CTRL1), &SWord);

		/* Clear PHY reset. */
		if (pAC->GIni.GP[Port].PhyType != SK_PHY_XMAC) {
			SK_IN32(IoC, B2_GP_IO, &Reg);
			if (Port == 0) {
				Reg |= GP_DIR_0; /* Set to output. */
				Reg |= GP_IO_0;
			}
			else {
				Reg |= GP_DIR_2; /* Set to output. */
				Reg |= GP_IO_2;
			}
			SK_OUT32(IoC, B2_GP_IO, Reg);

			/* Enable GMII interface. */
			XM_OUT16(IoC, Port, XM_HW_CFG, XM_HW_GMII_MD);

			PHY_READ(IoC, pPrt, Port, PHY_XMAC_ID1, &PhyId);
#ifdef xDEBUG
			if (SWord == 0xFFFF) {
				i = 1;
				do {
					PHY_READ(IoC, pPrt, Port, PHY_XMAC_ID1, &SWord);
					i++;
					/* Limit retries; else machine may hang. */
				} while (SWord == 0xFFFF && i < 500000);

				CMSMPrintString(
					pAC->pConfigTable,
					MSG_TYPE_RUNTIME_INFO,
					"ID1 is %x after %d reads.",
					(void *)SWord,
					(void *)i);

				/* Trigger PCI analyzer */
				/* SK_IN32(IoC, 0x012c, &Reg); */
			}
#endif	/* DEBUG */

			/*
			 * Optimize MDIO transfer by suppressing preamble.
			 * Must be done AFTER first access to BCOM chip.
			 */
			XM_IN16(IoC, Port, XM_MMU_CMD, &SWord);
			XM_OUT16(IoC, Port, XM_MMU_CMD, SWord | XM_MMU_NO_PRE);

			if (PhyId == 0x6044) {
				/* Write magic patterns to reserved registers. */
				i = 0;
				while (BcomRegC0Hack[i].PhyReg != 0) {
					PHY_WRITE(IoC, pPrt, Port, BcomRegC0Hack[i].PhyReg,
						BcomRegC0Hack[i].PhyVal);
					i++;
				}
			}
			else if (PhyId == 0x6041) {
				/* Write magic patterns to reserved registers. */
				i = 0;
				while (BcomRegA1Hack[i].PhyReg != 0) {
					PHY_WRITE(IoC, pPrt, Port, BcomRegA1Hack[i].PhyReg,
						BcomRegA1Hack[i].PhyVal);
					i++;
				}
			}

			/* Disable Power Management after reset. */
			PHY_READ(IoC, pPrt, Port, PHY_BCOM_AUX_CTRL, &SWord);
#ifdef xDEBUG
			if (SWord == 0xFFFF) {
				i = 1;
				do {
					PHY_READ(IoC, pPrt, Port, PHY_BCOM_AUX_CTRL, &SWord);
					i++;
					/* Limit retries; else machine may hang. */
				} while (SWord == 0xFFFF && i < 500000);

				CMSMPrintString(
					pAC->pConfigTable,
					MSG_TYPE_RUNTIME_INFO,
					"AUX_CTRL is %x after %d reads.",
					(void *)SWord,
					(void *)i);

				/* Trigger PCI analyzer */
				/* SK_IN32(IoC, 0x012c, &Reg); */
			}
#endif	/* DEBUG */
			PHY_WRITE(IoC, pPrt, Port, PHY_BCOM_AUX_CTRL,
				SWord | PHY_B_AC_DIS_PM);

			/* PHY LED initialization is done in SkGeXmitLED(), not here. */
		}

		/* Dummy read the Interrupt source register */
		XM_IN16(IoC, Port, XM_ISRC, &SWord);
		
		/*
		 * The autonegotiation process starts immediately after
		 * clearing the reset. The autonegotiation process should be
		 * started by the SIRQ, therefore stop it here immediately.
		 */
		SkXmInitPhy(pAC, IoC, Port, SK_FALSE);

	}

	/*
	 * configure the XMACs Station Address
	 * B2_MAC_2 = xx xx xx xx xx x1 is programed to XMAC A
	 * B2_MAC_3 = xx xx xx xx xx x2 is programed to XMAC B
	 */
	for (i = 0; i < 3; i++) {
		/*
		 * The following 2 statements are together endianess
		 * independant. Remember this when changing.
		 */
		SK_IN16(IoC, (B2_MAC_2 + Port * 8 + i * 2), &SWord);
		XM_OUT16(IoC, Port, (XM_SA + i * 2), SWord);
	}

	/* Tx Inter Packet Gap (XM_TX_IPG):	use default */
	/* Tx High Water Mark (XM_TX_HI_WM):	use default */
	/* Tx Low Water Mark (XM_TX_LO_WM):	use default */
	/* Host Request Threshold (XM_HT_THR):	use default */
	/* Rx Request Threshold (XM_RX_THR):	use default */
	/* Rx Low Water Mark (XM_RX_LO_WM):	use default */

	/* configure Rx High Water Mark (XM_RX_HI_WM) */
	XM_OUT16(IoC, Port, XM_RX_HI_WM, 0x05aa);

	if (pAC->GIni.GIMacsFound > 1) {
		switch (pAC->GIni.GIPortUsage) {
		case SK_RED_LINK:
			/* Configure Tx Request Threshold for red. link */
			XM_OUT16(IoC, Port, XM_TX_THR, SK_XM_THR_REDL);
			break;
		case SK_MUL_LINK:
			/* Configure Tx Request Threshold for load bal. */
			XM_OUT16(IoC, Port, XM_TX_THR, SK_XM_THR_MULL);
			break;
		case SK_JUMBO_LINK:
			/* Configure Tx Request Threshold for jumbo frames */
			XM_OUT16(IoC, Port, XM_TX_THR, SK_XM_THR_JUMBO);
			break;
		default:
			SK_ERR_LOG(pAC, SK_ERRCL_SW, SKERR_HWI_E014,
				SKERR_HWI_E014MSG);
			break;
		}
	}
	else {
		/* Configure Tx Request Threshold for single port */
		XM_OUT16(IoC, Port, XM_TX_THR, SK_XM_THR_SL);
	}

	/*
	 * setup register defaults for the Rx Command Register
	 *	- Enable Automatic Frame Padding on Tx side
	 */
	XM_OUT16(IoC, Port, XM_TX_CMD, XM_TX_AUTO_PAD);

	/*
	 * setup register defaults for the Rx Command Register,
	 * program value of PRxCmd
	 */
	XM_OUT16(IoC, Port, XM_RX_CMD, pPrt->PRxCmd);

	/*
	 * setup register defaults for the Mode Register
	 *	- Don't strip error frames to avoid Store & Forward
	 *	  on the rx side.
	 *	- Enable 'Check Station Address' bit
	 *	- Enable 'Check Address Array' bit
	 */
	XM_OUT32(IoC, Port, XM_MODE, XM_DEF_MODE);

	/*
	 * Initialize the Receive Counter Event Mask (XM_RX_EV_MSK)
	 *	- Enable all bits excepting 'Octets Rx OK Low CntOv'
	 *	  and 'Octets Rx OK Hi Cnt Ov'.
	 */
	XM_OUT32(IoC, Port, XM_RX_EV_MSK, XMR_DEF_MSK);

	/*
	 * Initialize the Transmit Counter Event Mask (XM_TX_EV_MSK)
	 *	- Enable all bits excepting 'Octets Tx OK Low CntOv'
	 *	  and 'Octets Tx OK Hi Cnt Ov'.
	 */
	XM_OUT32(IoC, Port, XM_TX_EV_MSK, XMT_DEF_MSK);

	/*
	 * Do NOT init XMAC interrupt mask here.
	 * All interrupts remain disable until link comes up!
	 */
	pPrt->PState = SK_PRT_INIT;

	/*
	 * Any additional configuration changes may be done now.
	 * The last action is to enable the rx and tx state machine.
	 * This should be done after the autonegotiation process
	 * has been completed successfully.
	 */
}	/* SkXmInitMac*/


/******************************************************************************
 *
 *	SkXmInitDupMd() - Initialize the XMACs Duplex Mode
 *
 * Description:
 *	This function initilaizes the XMACs Duplex Mode.
 *	It should be called after successfully finishing
 *	the Autonegotiation Process
 *
 * Returns:
 *	nothing
 */
void SkXmInitDupMd(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int		Port)		/* Port Index (MAC_1 + n) */
{
	switch (pAC->GIni.GP[Port].PLinkModeStatus) {
	case SK_LMODE_STAT_AUTOHALF:
	case SK_LMODE_STAT_HALF:
		/* Configuration Actions for Half Duplex Mode */
		/*
		 * XM_BURST = default value. We are propable not quick
		 * 	enough at the 'XMAC' bus to burst 8kB.
		 *	The XMAC stopps bursting if no transmit frames
		 *	are available or the burst limit is exceeded.
		 */
		/* XM_TX_RT_LIM = default value (15) */
		/* XM_TX_STIME = default value (0xff = 4096 bit times) */
		break;
	case SK_LMODE_STAT_AUTOFULL:
	case SK_LMODE_STAT_FULL:
		/* Configuration Actions for Full Duplex Mode */
		/*
		 * The duplex mode is configured by the PHY,
		 * therefore it seems to be that there is nothing
		 * to do here.
		 */
		break;
	case SK_LMODE_STAT_UNKNOWN:
	default:
		SK_ERR_LOG(pAC, SK_ERRCL_SW, SKERR_HWI_E007, SKERR_HWI_E007MSG);
		break;
	}
}	/* SkXmInitDupMd */


/******************************************************************************
 *
 *	SkXmInitPauseMd() - initialize the Pause Mode to be used for this port
 *
 * Description:
 *	This function initilaizes the Pause Mode which should
 *	be used for this port.
 *	It should be called after successfully finishing
 *	the Autonegotiation Process
 *
 * Returns:
 *	nothing
 */
void SkXmInitPauseMd(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int		Port)		/* Port Index (MAC_1 + n) */
{
	SK_GEPORT	*pPrt;
	SK_U32		DWord;
	SK_U16		Word;

	pPrt = &pAC->GIni.GP[Port];

	if (pPrt->PFlowCtrlStatus == SK_FLOW_STAT_NONE ||
		pPrt->PFlowCtrlStatus == SK_FLOW_STAT_LOC_SEND) {

		/* Disable Pause Frame Reception */
		XM_IN16(IoC, Port, XM_MMU_CMD, &Word);
		XM_OUT16(IoC, Port, XM_MMU_CMD, Word | XM_MMU_IGN_PF);
	}
	else {
		/*
		 * enabling pause frame reception is required for 1000BT 
		 * because the XMAC is not reset if the link is going down
		 */
		/* Enable Pause Frame Reception */
		XM_IN16(IoC, Port, XM_MMU_CMD, &Word);
		XM_OUT16(IoC, Port, XM_MMU_CMD, Word & ~XM_MMU_IGN_PF);
	}	

	if (pPrt->PFlowCtrlStatus == SK_FLOW_STAT_SYMMETRIC ||
		pPrt->PFlowCtrlStatus == SK_FLOW_STAT_LOC_SEND) {

		/*
		 * Configure Pause Frame Generation
		 * Use internal and external Pause Frame Generation.
		 * Sending pause frames is edge triggert. Send a
		 * Pause frame with the maximum pause time if
		 * internal oder external FIFO full condition
		 * occurs. Send a zero pause time frame to
		 * start transmission again.
		 */

		/* XM_PAUSE_DA = '010000C28001' (default) */

		/* XM_MAC_PTIME = 0xffff (maximum) */
		/* remember this value is defined in big endian (!) */
		XM_OUT16(IoC, Port, XM_MAC_PTIME, 0xffff);

		/* Set Pause Mode in Mode Register */
		XM_IN32(IoC, Port, XM_MODE, &DWord);
		XM_OUT32(IoC, Port, XM_MODE, DWord | XM_PAUSE_MODE);

		/* Set Pause Mode in MAC Rx FIFO */
		SK_OUT16(IoC, MR_ADDR(Port,RX_MFF_CTRL1), MFF_ENA_PAUSE);
	}
	else {
		/*
		 * disable pause frame generation is required for 1000BT 
		 * because the XMAC is not reset if the link is going down
		 */
		/* Disable Pause Mode in Mode Register */
		XM_IN32(IoC, Port, XM_MODE, &DWord);
		XM_OUT32(IoC, Port, XM_MODE, DWord & ~XM_PAUSE_MODE);

		/* Disable Pause Mode in MAC Rx FIFO */
		SK_OUT16(IoC, MR_ADDR(Port,RX_MFF_CTRL1), MFF_DIS_PAUSE);
	}
}	/* SkXmInitPauseMd*/


/******************************************************************************
 *
 *	SkXmInitPhy() - Initialize the XMAC II Phy registers
 *
 * Description:
 *	Initialize all the XMACs Phy registers
 *
 * Note:
 *
 * Returns:
 *	nothing
 */
void SkXmInitPhy(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int		Port,		/* Port Index (MAC_1 + n) */
SK_BOOL	DoLoop)		/* Should a Phy LOOback be set-up? */
{
	SK_GEPORT	*pPrt;

	pPrt = &pAC->GIni.GP[Port];
	switch (pPrt->PhyType) {
	case SK_PHY_XMAC:
		SkXmInitPhyXmac(pAC, IoC, Port, DoLoop);
		break;
	case SK_PHY_BCOM:
		SkXmInitPhyBcom(pAC, IoC, Port, DoLoop);
		break;
	case SK_PHY_LONE:
		SkXmInitPhyLone(pAC, IoC, Port, DoLoop);
		break;
	case SK_PHY_NAT:
		SkXmInitPhyNat(pAC, IoC, Port, DoLoop);
		break;
	}
}	/* SkXmInitPhy*/


/******************************************************************************
 *
 *	SkXmInitPhyXmac() - Initialize the XMAC II Phy registers
 *
 * Description:
 *	Initialize all the XMACs Phy registers
 *
 * Note:
 *
 * Returns:
 *	nothing
 */
static void SkXmInitPhyXmac(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int		Port,		/* Port Index (MAC_1 + n) */
SK_BOOL	DoLoop)		/* Should a Phy LOOback be set-up? */
{
	SK_GEPORT	*pPrt;
	SK_U16		Ctrl;

	pPrt = &pAC->GIni.GP[Port];

	/* Autonegotiation ? */
	if (pPrt->PLinkMode == SK_LMODE_HALF ||
	    pPrt->PLinkMode == SK_LMODE_FULL) {
		SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
			("InitPhyXmac: no autonegotiation Port %d\n", Port));
		/* No Autonegiotiation */
		/* Set DuplexMode in Config register */
		Ctrl = (pPrt->PLinkMode == SK_LMODE_FULL ? PHY_CT_DUP_MD : 0);

		/*
		 * Do NOT enable Autonegotiation here. This would hold
		 * the link down because no IDLES are transmitted
		 */
	}
	else {
		SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
			("InitPhyXmac: with autonegotiation Port %d\n", Port));
		/* Set Autonegotiation advertisement */
		Ctrl = 0;

		/* Set Full/half duplex capabilities */
		switch (pPrt->PLinkMode) {
		case SK_LMODE_AUTOHALF:
			Ctrl |= PHY_X_AN_HD;
			break;
		case SK_LMODE_AUTOFULL:
			Ctrl |= PHY_X_AN_FD;
			break;
		case SK_LMODE_AUTOBOTH:
			Ctrl |= PHY_X_AN_FD | PHY_X_AN_HD;
			break;
		default:
			SK_ERR_LOG(pAC, SK_ERRCL_SW | SK_ERRCL_INIT,
				SKERR_HWI_E015, SKERR_HWI_E015MSG);
		}

		switch (pPrt->PFlowCtrlMode) {
		case SK_FLOW_MODE_NONE:
			Ctrl |= PHY_X_P_NO_PAUSE;
			break;
		case SK_FLOW_MODE_LOC_SEND:
			Ctrl |= PHY_X_P_ASYM_MD;
			break;
		case SK_FLOW_MODE_SYMMETRIC:
			Ctrl |= PHY_X_P_SYM_MD;
			break;
		case SK_FLOW_MODE_SYM_OR_REM:
			Ctrl |= PHY_X_P_BOTH_MD;
			break;
		default:
			SK_ERR_LOG(pAC, SK_ERRCL_SW | SK_ERRCL_INIT,
				SKERR_HWI_E016, SKERR_HWI_E016MSG);
		}

		/* Write AutoNeg Advertisement Register */
		PHY_WRITE(IoC, pPrt, Port, PHY_XMAC_AUNE_ADV, Ctrl);

		/* Restart Autonegotiation */
		Ctrl = PHY_CT_ANE | PHY_CT_RE_CFG;
	}

	if (DoLoop) {
		/* Set the Phy Loopback bit, too */
		Ctrl |= PHY_CT_LOOP;
	}

	/* Write to the Phy control register */
	PHY_WRITE(IoC, pPrt, Port, PHY_XMAC_CTRL, Ctrl);
}	/* SkXmInitPhyXmac*/


/******************************************************************************
 *
 *	SkXmInitPhyBcom() - Initialize the Broadcom Phy registers
 *
 * Description:
 *	Initialize all the Broadcom Phy registers
 *
 * Note:
 *
 * Returns:
 *	nothing
 */
static void SkXmInitPhyBcom(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int		Port,		/* Port Index (MAC_1 + n) */
SK_BOOL	DoLoop)		/* Should a Phy LOOback be set-up? */
{
	SK_GEPORT	*pPrt;
	SK_U16		Ctrl1;
	SK_U16		Ctrl2;
	SK_U16		Ctrl3;
	SK_U16		Ctrl4;
	SK_U16		Ctrl5;

	Ctrl1 = PHY_B_CT_SP1000;
	Ctrl2 = 0;
	Ctrl3 = PHY_SEL_TYPE;
	Ctrl4 = PHY_B_PEC_EN_LTR;
	Ctrl5 = PHY_B_AC_TX_TST;

	pPrt = &pAC->GIni.GP[Port];

	/* manually Master/Slave ? */
	if (pPrt->PMSMode != SK_MS_MODE_AUTO) {
		Ctrl2 |= PHY_B_1000C_MSE;
		if (pPrt->PMSMode == SK_MS_MODE_MASTER) {
			Ctrl2 |= PHY_B_1000C_MSC;
		}
	}
	/* Autonegotiation ? */
	if (pPrt->PLinkMode == SK_LMODE_HALF ||
	    pPrt->PLinkMode == SK_LMODE_FULL) {
		SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
			("InitPhyBcom: no autonegotiation Port %d\n", Port));
		/* No Autonegiotiation */
		/* Set DuplexMode in Config register */
		Ctrl1 |= (pPrt->PLinkMode == SK_LMODE_FULL ? PHY_CT_DUP_MD : 0);

		/* Determine Master/Slave manually if not already done. */
		if (pPrt->PMSMode == SK_MS_MODE_AUTO) {
			Ctrl2 |= PHY_B_1000C_MSE;	/* set it to Slave */
		}

		/*
		 * Do NOT enable Autonegotiation here. This would hold
		 * the link down because no IDLES are transmitted
		 */
	}
	else {
		SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
			("InitPhyBcom: with autonegotiation Port %d\n", Port));
		/* Set Autonegotiation advertisement */

		/* Set Full/half duplex capabilities */
		switch (pPrt->PLinkMode) {
		case SK_LMODE_AUTOHALF:
			Ctrl2 |= PHY_B_1000C_AHD;
			break;
		case SK_LMODE_AUTOFULL:
			Ctrl2 |= PHY_B_1000C_AFD;
			break;
		case SK_LMODE_AUTOBOTH:
			Ctrl2 |= PHY_B_1000C_AFD | PHY_B_1000C_AHD;
			break;
		default:
			SK_ERR_LOG(pAC, SK_ERRCL_SW | SK_ERRCL_INIT,
				SKERR_HWI_E015, SKERR_HWI_E015MSG);
		}

		switch (pPrt->PFlowCtrlMode) {
		case SK_FLOW_MODE_NONE:
			Ctrl3 |= PHY_B_P_NO_PAUSE;
			break;
		case SK_FLOW_MODE_LOC_SEND:
			Ctrl3 |= PHY_B_P_ASYM_MD;
			break;
		case SK_FLOW_MODE_SYMMETRIC:
			Ctrl3 |= PHY_B_P_SYM_MD;
			break;
		case SK_FLOW_MODE_SYM_OR_REM:
			Ctrl3 |= PHY_B_P_BOTH_MD;
			break;
		default:
			SK_ERR_LOG(pAC, SK_ERRCL_SW | SK_ERRCL_INIT,
				SKERR_HWI_E016, SKERR_HWI_E016MSG);
		}

		/* Restart Autonegotiation */
		Ctrl1 |= PHY_CT_ANE | PHY_CT_RE_CFG;

	}
	
	/* Initialize LED register here? */
	/* No. Please do it in SkDgXmitLed() (if required) and swap
	   init order of LEDs and XMAC. (MAl) */
	
	/* Write 1000Base-T Control Register */
	PHY_WRITE(IoC, pPrt, Port, PHY_BCOM_1000T_CTRL, Ctrl2);
	SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
		("1000Base-T Control Reg = %x\n", Ctrl2));
	
	/* Write AutoNeg Advertisement Register */
	PHY_WRITE(IoC, pPrt, Port, PHY_BCOM_AUNE_ADV, Ctrl3);
	SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
		("AutoNeg Advertisment Reg = %x\n", Ctrl3));
	

	if (DoLoop) {
		/* Set the Phy Loopback bit, too */
		Ctrl1 |= PHY_CT_LOOP;
	}

	if (pAC->GIni.GIPortUsage == SK_JUMBO_LINK) {
		/* configure fifo to high latency for xmission of ext. packets*/
		Ctrl4 |= PHY_B_PEC_HIGH_LA;

		/* configure reception of extended packets */
		Ctrl5 |= PHY_B_AC_LONG_PACK;

		PHY_WRITE(IoC, pPrt, Port, PHY_BCOM_AUX_CTRL, Ctrl5);
	}

	/* Configure LED Traffic Mode and Jumbo Frame usage if specified */
	PHY_WRITE(IoC, pPrt, Port, PHY_BCOM_P_EXT_CTRL, Ctrl4);
	
	/* Write to the Phy control register */
	PHY_WRITE(IoC, pPrt, Port, PHY_BCOM_CTRL, Ctrl1);
	SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
		("PHY Control Reg = %x\n", Ctrl1));
}	/* SkXmInitPhyBcom */


/******************************************************************************
 *
 *	SkXmInitPhyLone() - Initialize the Level One Phy registers
 *
 * Description:
 *	Initialize all the Level One Phy registers
 *
 * Note:
 *
 * Returns:
 *	nothing
 */
static void SkXmInitPhyLone(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int		Port,		/* Port Index (MAC_1 + n) */
SK_BOOL	DoLoop)		/* Should a Phy LOOback be set-up? */
{
	SK_GEPORT	*pPrt;
	SK_U16		Ctrl1;
	SK_U16		Ctrl2;
	SK_U16		Ctrl3;

	Ctrl1 = PHY_L_CT_SP1000;
	Ctrl2 = 0;
	Ctrl3 = PHY_SEL_TYPE;

	pPrt = &pAC->GIni.GP[Port];

	/* manually Master/Slave ? */
	if (pPrt->PMSMode != SK_MS_MODE_AUTO) {
		Ctrl2 |= PHY_L_1000C_MSE;
		if (pPrt->PMSMode == SK_MS_MODE_MASTER) {
			Ctrl2 |= PHY_L_1000C_MSC;
		}
	}
	/* Autonegotiation ? */
	if (pPrt->PLinkMode == SK_LMODE_HALF ||
	    pPrt->PLinkMode == SK_LMODE_FULL) {
		/*
		 * level one spec say: "1000Mbps: manual mode not allowed"
		 * but lets see what happens...
		 */
		SK_ERR_LOG(pAC, SK_ERRCL_SW | SK_ERRCL_INIT,
			0, "Level One PHY only works with Autoneg");
		SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
			("InitPhyLone: no autonegotiation Port %d\n", Port));
		/* No Autonegiotiation */
		/* Set DuplexMode in Config register */
		Ctrl1 = (pPrt->PLinkMode == SK_LMODE_FULL ? PHY_CT_DUP_MD : 0);

		/* Determine Master/Slave manually if not already done. */
		if (pPrt->PMSMode == SK_MS_MODE_AUTO) {
			Ctrl2 |= PHY_L_1000C_MSE;	/* set it to Slave */
		}

		/*
		 * Do NOT enable Autonegotiation here. This would hold
		 * the link down because no IDLES are transmitted
		 */
	}
	else {
		SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
			("InitPhyLone: with autonegotiation Port %d\n", Port));
		/* Set Autonegotiation advertisement */

		/* Set Full/half duplex capabilities */
		switch (pPrt->PLinkMode) {
		case SK_LMODE_AUTOHALF:
			Ctrl2 |= PHY_L_1000C_AHD;
			break;
		case SK_LMODE_AUTOFULL:
			Ctrl2 |= PHY_L_1000C_AFD;
			break;
		case SK_LMODE_AUTOBOTH:
			Ctrl2 |= PHY_L_1000C_AFD | PHY_L_1000C_AHD;
			break;
		default:
			SK_ERR_LOG(pAC, SK_ERRCL_SW | SK_ERRCL_INIT,
				SKERR_HWI_E015, SKERR_HWI_E015MSG);
		}

		switch (pPrt->PFlowCtrlMode) {
		case SK_FLOW_MODE_NONE:
			Ctrl3 |= PHY_L_P_NO_PAUSE;
			break;
		case SK_FLOW_MODE_LOC_SEND:
			Ctrl3 |= PHY_L_P_ASYM_MD;
			break;
		case SK_FLOW_MODE_SYMMETRIC:
			Ctrl3 |= PHY_L_P_SYM_MD;
			break;
		case SK_FLOW_MODE_SYM_OR_REM:
			Ctrl3 |= PHY_L_P_BOTH_MD;
			break;
		default:
			SK_ERR_LOG(pAC, SK_ERRCL_SW | SK_ERRCL_INIT,
				SKERR_HWI_E016, SKERR_HWI_E016MSG);
		}

		/* Restart Autonegotiation */
		Ctrl1 = PHY_CT_ANE | PHY_CT_RE_CFG;

	}
	
	/* Initialize LED register here ? */
	/* No. Please do it in SkDgXmitLed() (if required) and swap
	   init order of LEDs and XMAC. (MAl) */
	
	/* Write 1000Base-T Control Register */
	PHY_WRITE(IoC, pPrt, Port, PHY_LONE_1000T_CTRL, Ctrl2);
	SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
		("1000Base-T Control Reg = %x\n", Ctrl2));
	
	/* Write AutoNeg Advertisement Register */
	PHY_WRITE(IoC, pPrt, Port, PHY_LONE_AUNE_ADV, Ctrl3);
	SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
		("AutoNeg Advertisment Reg = %x\n", Ctrl3));
	

	if (DoLoop) {
		/* Set the Phy Loopback bit, too */
		Ctrl1 |= PHY_CT_LOOP;
	}

	if (pAC->GIni.GIPortUsage == SK_JUMBO_LINK) {
		/*
		 * nothing to do for Level one.
		 * PHY supports frames up to 10k.
		 */
	}
	
	/* Write to the Phy control register */
	PHY_WRITE(IoC, pPrt, Port, PHY_LONE_CTRL, Ctrl1);
	SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
		("PHY Control Reg = %x\n", Ctrl1));
}	/* SkXmInitPhyLone*/


/******************************************************************************
 *
 *	SkXmInitPhyNat() - Initialize the National Phy registers
 *
 * Description:
 *	Initialize all the National Phy registers
 *
 * Note:
 *
 * Returns:
 *	nothing
 */
static void SkXmInitPhyNat(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int		Port,		/* Port Index (MAC_1 + n) */
SK_BOOL	DoLoop)		/* Should a Phy LOOback be set-up? */
{
/* todo: National */
}	/* SkXmInitPhyNat*/


/******************************************************************************
 *
 *	SkXmAutoNegLipaXmac() - Decides whether Link Partner could do autoneg
 *
 *	This function analyses the Interrupt status word. If any of the
 *	Autonegotiating interrupt bits are set, the PLipaAutoNeg variable
 *	is set true.
 */
void	SkXmAutoNegLipaXmac(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int		Port,		/* Port Index (MAC_1 + n) */
SK_U16	IStatus)	/* Interrupt Status word to analyse */
{
	SK_GEPORT	*pPrt;

	pPrt = &pAC->GIni.GP[Port];

	if (pPrt->PLipaAutoNeg != SK_LIPA_AUTO &&
		(IStatus & (XM_IS_LIPA_RC|XM_IS_RX_PAGE|XM_IS_AND))) {

		SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
			("AutoNegLipa: AutoNeg detected on port %d %x\n", Port, IStatus));
		pPrt->PLipaAutoNeg = SK_LIPA_AUTO;
	}
}	/* SkXmAutoNegLipaXmac*/


/******************************************************************************
 *
 *	SkXmAutoNegLipaBcom() - Decides whether Link Partner could do autoneg
 *
 *	This function analyses the PHY status word. If any of the
 *	Autonegotiating bits are set, The PLipaAutoNeg variable
 *	is set true.
 */
void	SkXmAutoNegLipaBcom(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int		Port,		/* Port Index (MAC_1 + n) */
SK_U16	PhyStat)	/* PHY Status word to analyse */
{
	SK_GEPORT	*pPrt;

	pPrt = &pAC->GIni.GP[Port];

	if (pPrt->PLipaAutoNeg != SK_LIPA_AUTO && (PhyStat & PHY_ST_AN_OVER)) {
		SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
			("AutoNegLipa: AutoNeg detected on port %d %x\n", Port, PhyStat));
		pPrt->PLipaAutoNeg = SK_LIPA_AUTO;
	}
}	/* SkXmAutoNegLipaBcom*/


/******************************************************************************
 *
 *	SkXmAutoNegLipaLone() - Decides whether Link Partner could do autoneg
 *
 *	This function analyses the PHY status word. If any of the
 *	Autonegotiating bits are set, The PLipaAutoNeg variable
 *	is set true.
 */
void	SkXmAutoNegLipaLone(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int	Port,		/* Port Index (MAC_1 + n) */
SK_U16	PhyStat)	/* PHY Status word to analyse */
{
	SK_GEPORT	*pPrt;

	pPrt = &pAC->GIni.GP[Port];

	if (pPrt->PLipaAutoNeg != SK_LIPA_AUTO &&
		(PhyStat & (PHY_ST_AN_OVER))) {

		SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
			("AutoNegLipa: AutoNeg detected on port %d %x\n", Port, PhyStat));
		pPrt->PLipaAutoNeg = SK_LIPA_AUTO;
	}
}	/* SkXmAutoNegLipaLone*/


/******************************************************************************
 *
 *	SkXmAutoNegLipaNat() - Decides whether Link Partner could do autoneg
 *
 *	This function analyses the PHY status word. If any of the
 *	Autonegotiating bits are set, The PLipaAutoNeg variable
 *	is set true.
 */
void	SkXmAutoNegLipaNat(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int		Port,		/* Port Index (MAC_1 + n) */
SK_U16	PhyStat)	/* PHY Status word to analyse */
{
	SK_GEPORT	*pPrt;

	pPrt = &pAC->GIni.GP[Port];

	if (pPrt->PLipaAutoNeg != SK_LIPA_AUTO &&
		(PhyStat & (PHY_ST_AN_OVER))) {

		SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
			("AutoNegLipa: AutoNeg detected on port %d %x\n", Port, PhyStat));
		pPrt->PLipaAutoNeg = SK_LIPA_AUTO;
	}
}	/* SkXmAutoNegLipaNat*/


/******************************************************************************
 *
 *	SkXmAutoNegDone() - Auto negotiation handling
 *
 * Description:
 *	This function handles the autonegotiation if the Done bit is set.
 *
 * Note:
 *	o The XMACs interrupt source register is NOT read here.
 *	o This function is public because it is used in the diagnostics
 *	  tool, too.
 *
 * Returns:
 *	SK_AND_OK	o.k.
 *	SK_AND_DUP_CAP 	Duplex capability error happened
 *	SK_AND_OTHER 	Other error happened
 */
int	SkXmAutoNegDone(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int		Port)		/* Port Index (MAC_1 + n) */
{
	switch (pAC->GIni.GP[Port].PhyType) {
	case SK_PHY_XMAC:
		return (SkXmAutoNegDoneXmac(pAC, IoC, Port));
	case SK_PHY_BCOM:
		return (SkXmAutoNegDoneBcom(pAC, IoC, Port));
	case SK_PHY_LONE:
		return (SkXmAutoNegDoneLone(pAC, IoC, Port));
	case SK_PHY_NAT:
		return (SkXmAutoNegDoneNat(pAC, IoC, Port));
	}
	return (SK_AND_OTHER);
}	/* SkXmAutoNegDone*/


/******************************************************************************
 *
 *	SkXmAutoNegDoneXmac() - Auto negotiation handling
 *
 * Description:
 *	This function handles the autonegotiation if the Done bit is set.
 *
 * Note:
 *	o The XMACs interrupt source register is NOT read here.
 *
 * Returns:
 *	SK_AND_OK	o.k.
 *	SK_AND_DUP_CAP 	Duplex capability error happened
 *	SK_AND_OTHER 	Other error happened
 */
static int	SkXmAutoNegDoneXmac(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int		Port)		/* Port Index (MAC_1 + n) */
{
	SK_GEPORT	*pPrt;
	SK_U16		ResAb;		/* Resolved Ability */
	SK_U16		LPAb;		/* Link Partner Ability */

	SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL, ("AutoNegDoneXmac"
		"Port %d\n",Port));

	pPrt = &pAC->GIni.GP[Port];

	/* Get PHY parameters */
	PHY_READ(IoC, pPrt, Port, PHY_XMAC_AUNE_LP, &LPAb);
	PHY_READ(IoC, pPrt, Port, PHY_XMAC_RES_ABI, &ResAb);

	if (LPAb & PHY_X_AN_RFB) {
		/* At least one of the remote fault bit is set */
		/* Error */
		SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
			("AutoNegFail: Remote fault bit set Port %d\n", Port));
		pPrt->PAutoNegFail = SK_TRUE;
		return (SK_AND_OTHER);
	}

	/* Check Duplex mismatch */
	if ((ResAb & (PHY_X_RS_HD | PHY_X_RS_FD)) == PHY_X_RS_FD) {
		pPrt->PLinkModeStatus = SK_LMODE_STAT_AUTOFULL;
	}
	else if ((ResAb & (PHY_X_RS_HD | PHY_X_RS_FD)) == PHY_X_RS_HD) {
		pPrt->PLinkModeStatus = SK_LMODE_STAT_AUTOHALF;
	}
	else {
		/* Error */
		SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
			("AutoNegFail: Duplex mode mismatch port %d\n", Port));
		pPrt->PAutoNegFail = SK_TRUE;
		return (SK_AND_DUP_CAP);
	}

	/* Check PAUSE mismatch */
	/* We are NOT using chapter 4.23 of the Xaqti manual */
	/* We are using IEEE 802.3z/D5.0 Table 37-4 */
	if ((pPrt->PFlowCtrlMode == SK_FLOW_MODE_SYMMETRIC ||
	     pPrt->PFlowCtrlMode == SK_FLOW_MODE_SYM_OR_REM) &&
	    (LPAb & PHY_X_P_SYM_MD)) {
		/* Symmetric PAUSE */
		pPrt->PFlowCtrlStatus = SK_FLOW_STAT_SYMMETRIC;
	}
	else if (pPrt->PFlowCtrlMode == SK_FLOW_MODE_SYM_OR_REM &&
		   (LPAb & PHY_X_RS_PAUSE) == PHY_X_P_ASYM_MD) {
		/* Enable PAUSE receive, disable PAUSE transmit */
		pPrt->PFlowCtrlStatus = SK_FLOW_STAT_REM_SEND;
	}
	else if (pPrt->PFlowCtrlMode == SK_FLOW_MODE_LOC_SEND &&
		   (LPAb & PHY_X_RS_PAUSE) == PHY_X_P_BOTH_MD) {
		/* Disable PAUSE receive, enable PAUSE transmit */
		pPrt->PFlowCtrlStatus = SK_FLOW_STAT_LOC_SEND;
	}
	else {
		/* PAUSE mismatch -> no PAUSE */
		pPrt->PFlowCtrlStatus = SK_FLOW_STAT_NONE;
	}

	/* We checked everything and may now enable the link */
	pPrt->PAutoNegFail = SK_FALSE;

	SkXmRxTxEnable(pAC, IoC, Port);
	return (SK_AND_OK);
}	/* SkXmAutoNegDoneXmac*/


/******************************************************************************
 *
 *	SkXmAutoNegDoneBcom() - Auto negotiation handling
 *
 * Description:
 *	This function handles the autonegotiation if the Done bit is set.
 *
 * Note:
 *	o The XMACs interrupt source register is NOT read here.
 *
 * Returns:
 *	SK_AND_OK	o.k.
 *	SK_AND_DUP_CAP 	Duplex capability error happened
 *	SK_AND_OTHER 	Other error happened
 */
static int	SkXmAutoNegDoneBcom(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int		Port)		/* Port Index (MAC_1 + n) */
{
	SK_GEPORT	*pPrt;
	SK_U16		LPAb;		/* Link Partner Ability */
	SK_U16		AuxStat;	/* Auxiliary Status */


	SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
		("AutoNegDoneBcom, Port %d\n", Port));
	pPrt = &pAC->GIni.GP[Port];

	/* Get PHY parameters. */
	PHY_READ(IoC, pPrt, Port, PHY_BCOM_AUNE_LP, &LPAb);
	PHY_READ(IoC, pPrt, Port, PHY_BCOM_AUX_STAT, &AuxStat);

	if (LPAb & PHY_B_AN_RF) {
		/* Remote fault bit is set: Error. */
		SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_CTRL,
			("AutoNegFail: Remote fault bit set Port %d\n", Port));
		pPrt->PAutoNegFail = SK_TRUE;
		return (SK_AND_OTHER);
	}

	/* Check Duplex mismatch. */
	if ((AuxStat & PHY_B_AS_AN_RES) == PHY_B_RES_1000FD) {
		pPrt->PLinkModeStatus = SK_LMODE_STAT_AUTOFULL;
	}
	else if ((AuxStat & PHY_B_AS_AN_RES) == PHY_B_RES_1000HD) {
		pPrt->PLinkModeStatus = SK_LMODE_STAT_AUTOHALF;
	}
	else {
		/* Error. */
		SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
			("AutoNegFail: Duplex mode mismatch port %d\n", Port));
		pPrt->PAutoNegFail = SK_TRUE;
		return (SK_AND_DUP_CAP);
	}
	

	/* Check PAUSE mismatch. */
	/* We are NOT using chapter 4.23 of the Xaqti manual. */
	/* We are using IEEE 802.3z/D5.0 Table 37-4. */
	if ((AuxStat & (PHY_B_AS_PRR | PHY_B_AS_PRT)) == 
		(PHY_B_AS_PRR | PHY_B_AS_PRT)) {
		/* Symmetric PAUSE. */
		pPrt->PFlowCtrlStatus = SK_FLOW_STAT_SYMMETRIC;
	}
	else if ((AuxStat & (PHY_B_AS_PRR | PHY_B_AS_PRT)) == PHY_B_AS_PRR) {
		/* Enable PAUSE receive, disable PAUSE transmit. */
		pPrt->PFlowCtrlStatus = SK_FLOW_STAT_REM_SEND;
	}
	else if ((AuxStat & (PHY_B_AS_PRR | PHY_B_AS_PRT)) == PHY_B_AS_PRT) {
		/* Disable PAUSE receive, enable PAUSE transmit. */
		pPrt->PFlowCtrlStatus = SK_FLOW_STAT_LOC_SEND;
	}
	else {
		/* PAUSE mismatch -> no PAUSE. */
		pPrt->PFlowCtrlStatus = SK_FLOW_STAT_NONE;
	}

	/* We checked everything and may now enable the link. */
	pPrt->PAutoNegFail = SK_FALSE;

	SkXmRxTxEnable(pAC, IoC, Port);
	return (SK_AND_OK);
}	/* SkXmAutoNegDoneBcom*/


/******************************************************************************
 *
 *	SkXmAutoNegDoneLone() - Auto negotiation handling
 *
 * Description:
 *	This function handles the autonegotiation if the Done bit is set.
 *
 * Note:
 *	o The XMACs interrupt source register is NOT read here.
 *
 * Returns:
 *	SK_AND_OK	o.k.
 *	SK_AND_DUP_CAP 	Duplex capability error happened
 *	SK_AND_OTHER 	Other error happened
 */
static int	SkXmAutoNegDoneLone(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int		Port)		/* Port Index (MAC_1 + n) */
{
	SK_GEPORT	*pPrt;
	SK_U16		ResAb;		/* Resolved Ability */
	SK_U16		LPAb;		/* Link Partner Ability */
	SK_U16		QuickStat;	/* Auxiliary Status */

	SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL, ("AutoNegDoneLone"
		"Port %d\n",Port));
	pPrt = &pAC->GIni.GP[Port];

	/* Get PHY parameters */
	PHY_READ(IoC, pPrt, Port, PHY_LONE_AUNE_LP, &LPAb);
	PHY_READ(IoC, pPrt, Port, PHY_LONE_1000T_STAT, &ResAb);
	PHY_READ(IoC, pPrt, Port, PHY_LONE_Q_STAT, &QuickStat);

	if (LPAb & PHY_L_AN_RF) {
		/* Remote fault bit is set */
		/* Error */
		SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
			("AutoNegFail: Remote fault bit set Port %d\n", Port));
		pPrt->PAutoNegFail = SK_TRUE;
		return (SK_AND_OTHER);
	}

	/* Check Duplex mismatch */
	if (QuickStat & PHY_L_QS_DUP_MOD) {
		pPrt->PLinkModeStatus = SK_LMODE_STAT_AUTOFULL;
	}
	else {
		pPrt->PLinkModeStatus = SK_LMODE_STAT_AUTOHALF;
	}
	
	/* Check Master/Slave resolution */
	if (ResAb & (PHY_L_1000S_MSF)) {
		/* Error */
		SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_CTRL,
			("Master/Slave Fault port %d\n", Port));
		pPrt->PAutoNegFail = SK_TRUE;
		pPrt->PMSStatus = SK_MS_STAT_FAULT;
		return (SK_AND_OTHER);
	}
	else if (ResAb & PHY_L_1000S_MSR) {
		pPrt->PMSStatus = SK_MS_STAT_MASTER;
	}
	else {
		pPrt->PMSStatus = SK_MS_STAT_SLAVE;
	}

	/* Check PAUSE mismatch */
	/* We are NOT using chapter 4.23 of the Xaqti manual */
	/* We are using IEEE 802.3z/D5.0 Table 37-4 */
	/* we must manually resolve the abilities here */
	pPrt->PFlowCtrlStatus = SK_FLOW_STAT_NONE;
	switch (pPrt->PFlowCtrlMode) {
	case SK_FLOW_MODE_NONE:
		/* default */
		break;
	case SK_FLOW_MODE_LOC_SEND:
		if ((QuickStat & (PHY_L_QS_PAUSE | PHY_L_QS_AS_PAUSE)) ==
			(PHY_L_QS_PAUSE | PHY_L_QS_AS_PAUSE)) {
			/* Disable PAUSE receive, enable PAUSE transmit */
			pPrt->PFlowCtrlStatus = SK_FLOW_STAT_LOC_SEND;
		}
		break;
	case SK_FLOW_MODE_SYMMETRIC:
		if ((QuickStat & PHY_L_QS_PAUSE) == PHY_L_QS_PAUSE) {
			/* Symmetric PAUSE */
			pPrt->PFlowCtrlStatus = SK_FLOW_STAT_SYMMETRIC;
		}
		break;
	case SK_FLOW_MODE_SYM_OR_REM:
		if ((QuickStat & (PHY_L_QS_PAUSE | PHY_L_QS_AS_PAUSE)) ==
			PHY_L_QS_AS_PAUSE) {
			/* Enable PAUSE receive, disable PAUSE transmit */
			pPrt->PFlowCtrlStatus = SK_FLOW_STAT_REM_SEND;
		}
		else if ((QuickStat & PHY_L_QS_PAUSE) == PHY_L_QS_PAUSE) {
			/* Symmetric PAUSE */
			pPrt->PFlowCtrlStatus = SK_FLOW_STAT_SYMMETRIC;
		}
		break;
	default:
		SK_ERR_LOG(pAC, SK_ERRCL_SW | SK_ERRCL_INIT,
			SKERR_HWI_E016, SKERR_HWI_E016MSG);
	}

	/* We checked everything and may now enable the link */
	pPrt->PAutoNegFail = SK_FALSE;

	SkXmRxTxEnable(pAC, IoC, Port);
	return (SK_AND_OK);
}	/* SkXmAutoNegDoneLone */


/******************************************************************************
 *
 *	SkXmAutoNegDoneNat() - Auto negotiation handling
 *
 * Description:
 *	This function handles the autonegotiation if the Done bit is set.
 *
 * Note:
 *	o The XMACs interrupt source register is NOT read here.
 *	o This function is public because it is used in the diagnostics
 *	  tool, too.
 *
 * Returns:
 *	SK_AND_OK	o.k.
 *	SK_AND_DUP_CAP 	Duplex capability error happened
 *	SK_AND_OTHER 	Other error happened
 */
static int	SkXmAutoNegDoneNat(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int		Port)		/* Port Index (MAC_1 + n) */
{
/* todo: National */
	return (SK_AND_OK);
}	/* SkXmAutoNegDoneNat*/


/******************************************************************************
 *
 *	SkXmRxTxEnable() - Enable RxTx activity if port is up
 *
 * Description:
 *
 * Note:
 *	o The XMACs interrupt source register is NOT read here.
 *
 * Returns:
 *	0	o.k.
 *	!= 0	Error happened
 */
int SkXmRxTxEnable(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int		Port)		/* Port Index (MAC_1 + n) */
{
	SK_GEPORT	*pPrt;
	SK_U16		Reg;		/* 16bit register value */
	SK_U16		IntMask;	/* XMac interrupt mask */
	SK_U16		SWord;

	pPrt = &pAC->GIni.GP[Port];

	if (!pPrt->PHWLinkUp) {
		/* The Hardware link is NOT up */
		return (0);
	}

	if ((pPrt->PLinkMode == SK_LMODE_AUTOHALF ||
	     pPrt->PLinkMode == SK_LMODE_AUTOFULL ||
	     pPrt->PLinkMode == SK_LMODE_AUTOBOTH) &&
	     pPrt->PAutoNegFail) {
		/* Autonegotiation is not done or failed */
		return (0);
	}

	/* Set Dup Mode and Pause Mode */
	SkXmInitDupMd (pAC, IoC, Port);
	SkXmInitPauseMd (pAC, IoC, Port);

	/*
	 * Initialize the Interrupt Mask Register. Default IRQs are...
	 *	- Link Asynchronous Event
	 *	- Link Partner requests config
	 *	- Auto Negotiation Done
	 *	- Rx Counter Event Overflow
	 *	- Tx Counter Event Overflow
	 *	- Transmit FIFO Underrun
	 */
	if (pPrt->PhyType == SK_PHY_XMAC) {
		IntMask = XM_DEF_MSK;
	}
	else {
		/* disable GP0 interrupt bit */
		IntMask = XM_DEF_MSK | XM_IS_INP_ASS;
	}
	XM_OUT16(IoC, Port, XM_IMSK, IntMask);

	/* RX/TX enable */
	XM_IN16(IoC, Port, XM_MMU_CMD, &Reg);
	if (pPrt->PhyType != SK_PHY_XMAC &&
		(pPrt->PLinkModeStatus == SK_LMODE_STAT_FULL || 
		 pPrt->PLinkModeStatus == SK_LMODE_STAT_AUTOFULL)) {
		Reg |= XM_MMU_GMII_FD;
	}
	switch (pPrt->PhyType) {
	case SK_PHY_BCOM:
		/* Enable Power Management after link up */
		PHY_READ(IoC, pPrt, Port, PHY_BCOM_AUX_CTRL, &SWord);
		PHY_WRITE(IoC, pPrt, Port, PHY_BCOM_AUX_CTRL, SWord & ~PHY_B_AC_DIS_PM);
		PHY_WRITE(IoC, pPrt, Port, PHY_BCOM_INT_MASK, PHY_B_DEF_MSK);
		break;
	case SK_PHY_LONE:
		PHY_WRITE(IoC, pPrt, Port, PHY_LONE_INT_ENAB, PHY_L_DEF_MSK);
		break;
	case SK_PHY_NAT:
		/* todo National:
		PHY_WRITE(IoC, pPrt, Port, PHY_NAT_INT_MASK, 
			PHY_N_DEF_MSK); */
		/* no interrupts possible from National ??? */
		break;
	}
	XM_OUT16(IoC, Port, XM_MMU_CMD, Reg | XM_MMU_ENA_RX | XM_MMU_ENA_TX);
				      
	return (0);
}	/* SkXmRxTxEnable*/

#ifndef SK_DIAG

/******************************************************************************
 *
 *	SkXmIrq() - Interrupt service routine
 *
 * Description:
 *	Services an Interrupt of the XMAC II
 *
 * Note:
 *	The XMACs interrupt source register is NOT read here.
 *	With an external PHY, some interrupt bits are not meaningfull
 *	any more:
 *	- LinkAsyncEvent (bit #14)              XM_IS_LNK_AE
 *	- LinkPartnerReqConfig (bit #10)	XM_IS_LIPA_RC
 *	- Page Received (bit #9)		XM_IS_RX_PAGE
 *	- NextPageLoadedForXmt (bit #8)		XM_IS_TX_PAGE
 *	- AutoNegDone (bit #7)			XM_IS_AND
 *	Also probably not valid any more is the GP0 input bit:
 *	- GPRegisterBit0set			XM_IS_INP_ASS
 *
 * Returns:
 *	nothing
 */
void SkXmIrq(
SK_AC	*pAC,		/* adapter context */
SK_IOC	IoC,		/* IO context */
int		Port,		/* Port Index (MAC_1 + n) */
SK_U16	IStatus)	/* Interrupt status read from the XMAC */
{
	SK_GEPORT	*pPrt;
	SK_EVPARA	Para;
	SK_U16		IStatus2;

	pPrt = &pAC->GIni.GP[Port];
	
	if (pPrt->PhyType != SK_PHY_XMAC) {
		/* mask bits that are not used with ext. PHY */
		IStatus &= ~(XM_IS_LNK_AE | XM_IS_LIPA_RC |
			XM_IS_RX_PAGE | XM_IS_TX_PAGE |
			XM_IS_AND | XM_IS_INP_ASS);
	}
	
	/*
	 * LinkPartner Autonegable?
	 */
	if (pPrt->PhyType == SK_PHY_XMAC) {
		SkXmAutoNegLipaXmac(pAC, IoC, Port, IStatus);
	}

	SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_IRQ,
		("XmacIrq Port %d Isr %x\n", Port, IStatus));

	if (!pPrt->PHWLinkUp) {
		/* Spurious XMAC interrupt */
		SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_IRQ,
			("SkXmIrq: spurious interrupt on port %d\n", Port));
		return;
	}

	if (IStatus & XM_IS_INP_ASS) {
		/* Reread ISR Register if link is not in sync */
		XM_IN16(IoC, Port, XM_ISRC, &IStatus2);

		SK_DBG_MSG(pAC, SK_DBGMOD_HWM, SK_DBGCAT_IRQ,
			("SkXmIrq: Link async. Double check port %d %x %x\n",
			 Port, IStatus, IStatus2));
		IStatus &= ~XM_IS_INP_ASS;
		IStatus |= IStatus2;

	}

	if (IStatus & XM_IS_LNK_AE) {
		/* not used GP0 is used instead */
	}

	if (IStatus & XM_IS_TX_ABORT) {
		/* not used */
	}

	if (IStatus & XM_IS_FRC_INT) {
		/* not used. use ASIC IRQ instead if needed */
	}

	if (IStatus & (XM_IS_INP_ASS | XM_IS_LIPA_RC | XM_IS_RX_PAGE)) {
		SkHWLinkDown(pAC, IoC, Port);

		/* Signal to RLMT */
		Para.Para32[0] = (SK_U32) Port;
		SkEventQueue(pAC, SKGE_RLMT, SK_RLMT_LINK_DOWN, Para);

		SkTimerStart(pAC, IoC, &pAC->GIni.GP[Port].PWaTimer,
			SK_WA_INA_TIME, SKGE_HWAC, SK_HWEV_WATIM, Para);
	}

	if (IStatus & XM_IS_RX_PAGE) {
		/* not used */
	}

	if (IStatus & XM_IS_TX_PAGE) {
		/* not used */
	}

	if (IStatus & XM_IS_AND) {
		SK_DBG_MSG(pAC,SK_DBGMOD_HWM,SK_DBGCAT_IRQ,
			("SkXmIrq: AND on link that is up port %d\n", Port));
	}

	if (IStatus & XM_IS_TSC_OV) {
		/* not used */
	}

	if (IStatus & XM_IS_RXC_OV) {
		Para.Para32[0] = (SK_U32) Port;
		Para.Para32[1] = (SK_U32) IStatus;
		SkPnmiEvent(pAC, IoC, SK_PNMI_EVT_SIRQ_OVERFLOW, Para);
	}

	if (IStatus & XM_IS_TXC_OV) {
		Para.Para32[0] = (SK_U32) Port;
		Para.Para32[1] = (SK_U32) IStatus;
		SkPnmiEvent(pAC, IoC, SK_PNMI_EVT_SIRQ_OVERFLOW, Para);
	}

	if (IStatus & XM_IS_RXF_OV) {
		/* normal situation -> no effect */
	}

	if (IStatus & XM_IS_TXF_UR) {
		/* may NOT happen -> error log */
		SK_ERR_LOG(pAC, SK_ERRCL_HW , SKERR_SIRQ_E020,
			SKERR_SIRQ_E020MSG);
	}

	if (IStatus & XM_IS_TX_COMP) {
		/* not served here */
	}

	if (IStatus & XM_IS_RX_COMP) {
		/* not served here */
	}

}	/* SkXmIrq*/

#endif /* !SK_DIAG */

/* End of file */
