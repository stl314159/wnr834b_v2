/* $Id: nj_s.c,v 1.1.1.1 2010/03/05 07:31:30 reynolds Exp $
 *
 * This software may be used and distributed according to the terms
 * of the GNU General Public License, incorporated herein by reference.
 *
 */

#define __NO_VERSION__
#include <linux/config.h>
#include <linux/init.h>
#include "hisax.h"
#include "isac.h"
#include "isdnl1.h"
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/ppp_defs.h>
#include "netjet.h"

const char *NETjet_S_revision = "$Revision: 1.1.1.1 $";

static u_char dummyrr(struct IsdnCardState *cs, int chan, u_char off)
{
	return(5);
}

static void dummywr(struct IsdnCardState *cs, int chan, u_char off, u_char value)
{
}

static void
netjet_s_interrupt(int intno, void *dev_id, struct pt_regs *regs)
{
	struct IsdnCardState *cs = dev_id;
	u_char val, sval;
	long flags;

	if (!cs) {
		printk(KERN_WARNING "NETjet-S: Spurious interrupt!\n");
		return;
	}
	if (!((sval = bytein(cs->hw.njet.base + NETJET_IRQSTAT1)) &
		NETJET_ISACIRQ)) {
		val = NETjet_ReadIC(cs, ISAC_ISTA);
		if (cs->debug & L1_DEB_ISAC)
			debugl1(cs, "tiger: i1 %x %x", sval, val);
		if (val) {
			isac_interrupt(cs, val);
			NETjet_WriteIC(cs, ISAC_MASK, 0xFF);
			NETjet_WriteIC(cs, ISAC_MASK, 0x0);
		}
	}
	save_flags(flags);
	cli();
	/* start new code 13/07/00 GE */
	/* set bits in sval to indicate which page is free */
	if (inl(cs->hw.njet.base + NETJET_DMA_WRITE_ADR) <
		inl(cs->hw.njet.base + NETJET_DMA_WRITE_IRQ))
		/* the 2nd write page is free */
		sval = 0x08;
	else	/* the 1st write page is free */
		sval = 0x04;	
	if (inl(cs->hw.njet.base + NETJET_DMA_READ_ADR) <
		inl(cs->hw.njet.base + NETJET_DMA_READ_IRQ))
		/* the 2nd read page is free */
		sval = sval | 0x02;
	else	/* the 1st read page is free */
		sval = sval | 0x01;	
	if (sval != cs->hw.njet.last_is0) /* we have a DMA interrupt */
	{
		if (test_and_set_bit(FLG_LOCK_ATOMIC, &cs->HW_Flags)) {
			restore_flags(flags);
			return;
		}
		cs->hw.njet.irqstat0 = sval;
		restore_flags(flags);
		if ((cs->hw.njet.irqstat0 & NETJET_IRQM0_READ) != 
			(cs->hw.njet.last_is0 & NETJET_IRQM0_READ))
			/* we have a read dma int */
			read_tiger(cs);
		if ((cs->hw.njet.irqstat0 & NETJET_IRQM0_WRITE) !=
			(cs->hw.njet.last_is0 & NETJET_IRQM0_WRITE))
			/* we have a write dma int */
			write_tiger(cs);
		/* end new code 13/07/00 GE */
		test_and_clear_bit(FLG_LOCK_ATOMIC, &cs->HW_Flags);
	} else
		restore_flags(flags);

/*	if (!testcnt--) {
		cs->hw.njet.dmactrl = 0;
		byteout(cs->hw.njet.base + NETJET_DMACTRL,
			cs->hw.njet.dmactrl);
		byteout(cs->hw.njet.base + NETJET_IRQMASK0, 0);
	}
*/
}

static void
reset_netjet_s(struct IsdnCardState *cs)
{
	long flags;

	save_flags(flags);
	sti();
	cs->hw.njet.ctrl_reg = 0xff;  /* Reset On */
	byteout(cs->hw.njet.base + NETJET_CTRL, cs->hw.njet.ctrl_reg);
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout((10*HZ)/1000);	/* Timeout 10ms */
	cs->hw.njet.ctrl_reg = 0x40;  /* Reset Off and status read clear */
	/* now edge triggered for TJ320 GE 13/07/00 */
	byteout(cs->hw.njet.base + NETJET_CTRL, cs->hw.njet.ctrl_reg);
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout((10*HZ)/1000);	/* Timeout 10ms */
	restore_flags(flags);
	cs->hw.njet.auxd = 0;
	cs->hw.njet.dmactrl = 0;
	byteout(cs->hw.njet.base + NETJET_AUXCTRL, ~NETJET_ISACIRQ);
	byteout(cs->hw.njet.base + NETJET_IRQMASK1, NETJET_ISACIRQ);
	byteout(cs->hw.njet.auxa, cs->hw.njet.auxd);
}

static int
NETjet_S_card_msg(struct IsdnCardState *cs, int mt, void *arg)
{
	switch (mt) {
		case CARD_RESET:
			reset_netjet_s(cs);
			return(0);
		case CARD_RELEASE:
			release_io_netjet(cs);
			return(0);
		case CARD_INIT:
			inittiger(cs);
			clear_pending_isac_ints(cs);
			initisac(cs);
			/* Reenable all IRQ */
			cs->writeisac(cs, ISAC_MASK, 0);
			return(0);
		case CARD_TEST:
			return(0);
	}
	return(0);
}

static struct pci_dev *dev_netjet __initdata = NULL;

int __init
setup_netjet_s(struct IsdnCard *card)
{
	int bytecnt;
	struct IsdnCardState *cs = card->cs;
	char tmp[64];
	long flags;

#ifdef __BIG_ENDIAN
#error "not running on big endian machines now"
#endif
	strcpy(tmp, NETjet_S_revision);
	printk(KERN_INFO "HiSax: Traverse Tech. NETjet-S driver Rev. %s\n", HiSax_getrev(tmp));
	if (cs->typ != ISDN_CTYPE_NETJET_S)
		return(0);
	test_and_clear_bit(FLG_LOCK_ATOMIC, &cs->HW_Flags);

#if CONFIG_PCI

	for ( ;; )
	{
		if (!pci_present()) {
			printk(KERN_ERR "Netjet: no PCI bus present\n");
			return(0);
		}
		if ((dev_netjet = pci_find_device(PCI_VENDOR_ID_TIGERJET,
			PCI_DEVICE_ID_TIGERJET_300,  dev_netjet))) {
			if (pci_enable_device(dev_netjet))
				return(0);
			pci_set_master(dev_netjet);
			cs->irq = dev_netjet->irq;
			if (!cs->irq) {
				printk(KERN_WARNING "NETjet-S: No IRQ for PCI card found\n");
				return(0);
			}
			cs->hw.njet.base = pci_resource_start(dev_netjet, 0);
			if (!cs->hw.njet.base) {
				printk(KERN_WARNING "NETjet-S: No IO-Adr for PCI card found\n");
				return(0);
			}
			/* 2001/10/04 Christoph Ersfeld, Formula-n Europe AG www.formula-n.com */
			if ((dev_netjet->subsystem_vendor == 0x55) &&
				(dev_netjet->subsystem_device == 0x02)) {
				printk(KERN_WARNING "Netjet: You tried to load this driver with an incompatible TigerJet-card\n");
				printk(KERN_WARNING "Use type=41 for Formula-n enter:now ISDN PCI and compatible\n");
				return(0);
			}
			/* end new code */
		} else {
			printk(KERN_WARNING "NETjet-S: No PCI card found\n");
			return(0);
		}

		cs->hw.njet.auxa = cs->hw.njet.base + NETJET_AUXDATA;
		cs->hw.njet.isac = cs->hw.njet.base | NETJET_ISAC_OFF;

		save_flags(flags);
		sti();

		cs->hw.njet.ctrl_reg = 0xff;  /* Reset On */
		byteout(cs->hw.njet.base + NETJET_CTRL, cs->hw.njet.ctrl_reg);

		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout((10*HZ)/1000);	/* Timeout 10ms */

		cs->hw.njet.ctrl_reg = 0x00;  /* Reset Off and status read clear */
		byteout(cs->hw.njet.base + NETJET_CTRL, cs->hw.njet.ctrl_reg);

		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout((10*HZ)/1000);	/* Timeout 10ms */

		restore_flags(flags);

		cs->hw.njet.auxd = 0xC0;
		cs->hw.njet.dmactrl = 0;

		byteout(cs->hw.njet.base + NETJET_AUXCTRL, ~NETJET_ISACIRQ);
		byteout(cs->hw.njet.base + NETJET_IRQMASK1, NETJET_ISACIRQ);
		byteout(cs->hw.njet.auxa, cs->hw.njet.auxd);

		switch ( ( ( NETjet_ReadIC( cs, ISAC_RBCH ) >> 5 ) & 3 ) )
		{
			case 0 :
				break;

			case 3 :
				printk( KERN_WARNING "NETjet-S: NETspider-U PCI card found\n" );
				continue;

			default :
				printk( KERN_WARNING "NETjet-S: No PCI card found\n" );
				return 0;
                }
                break;
	}
#else

	printk(KERN_WARNING "NETjet-S: NO_PCI_BIOS\n");
	printk(KERN_WARNING "NETjet-S: unable to config NETJET-S PCI\n");
	return (0);

#endif /* CONFIG_PCI */

	bytecnt = 256;

	printk(KERN_INFO
		"NETjet-S: PCI card configured at %#lx IRQ %d\n",
		cs->hw.njet.base, cs->irq);
	if (check_region(cs->hw.njet.base, bytecnt)) {
		printk(KERN_WARNING
		       "HiSax: %s config port %#lx-%#lx already in use\n",
		       CardType[card->typ],
		       cs->hw.njet.base,
		       cs->hw.njet.base + bytecnt);
		return (0);
	} else {
		request_region(cs->hw.njet.base, bytecnt, "netjet-s isdn");
	}
	reset_netjet_s(cs);
	cs->readisac  = &NETjet_ReadIC;
	cs->writeisac = &NETjet_WriteIC;
	cs->readisacfifo  = &NETjet_ReadICfifo;
	cs->writeisacfifo = &NETjet_WriteICfifo;
	cs->BC_Read_Reg  = &dummyrr;
	cs->BC_Write_Reg = &dummywr;
	cs->BC_Send_Data = &netjet_fill_dma;
	cs->cardmsg = &NETjet_S_card_msg;
	cs->irq_func = &netjet_s_interrupt;
	cs->irq_flags |= SA_SHIRQ;
	ISACVersion(cs, "NETjet-S:");
	return (1);
}
