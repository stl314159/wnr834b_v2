/* $Id: bkm_a4t.c,v 1.1.1.1 2010/03/05 07:31:30 reynolds Exp $
 *
 * low level stuff for T-Berkom A4T
 *
 * Author       Roland Klabunde
 * Copyright    by Roland Klabunde   <R.Klabunde@Berkom.de>
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
#include "hscx.h"
#include "jade.h"
#include "isdnl1.h"
#include <linux/pci.h>
#include "bkm_ax.h"

extern const char *CardType[];

const char *bkm_a4t_revision = "$Revision: 1.1.1.1 $";


static inline u_char
readreg(unsigned int ale, unsigned long adr, u_char off)
{
	register u_int ret;
	long flags;
	unsigned int *po = (unsigned int *) adr;	/* Postoffice */
	save_flags(flags);
	cli();
	*po = (GCS_2 | PO_WRITE | off);
	__WAITI20__(po);
	*po = (ale | PO_READ);
	__WAITI20__(po);
	ret = *po;
	restore_flags(flags);
	return ((unsigned char) ret);
}


static inline void
readfifo(unsigned int ale, unsigned long adr, u_char off, u_char * data, int size)
{
	/* fifo read without cli because it's allready done  */
	int i;
	for (i = 0; i < size; i++)
		*data++ = readreg(ale, adr, off);
}


static inline void
writereg(unsigned int ale, unsigned long adr, u_char off, u_char data)
{
	long flags;
	unsigned int *po = (unsigned int *) adr;	/* Postoffice */
	save_flags(flags);
	cli();
	*po = (GCS_2 | PO_WRITE | off);
	__WAITI20__(po);
	*po = (ale | PO_WRITE | data);
	__WAITI20__(po);
	restore_flags(flags);
}


static inline void
writefifo(unsigned int ale, unsigned long adr, u_char off, u_char * data, int size)
{
	/* fifo write without cli because it's allready done  */
	int i;

	for (i = 0; i < size; i++)
		writereg(ale, adr, off, *data++);
}


/* Interface functions */

static u_char
ReadISAC(struct IsdnCardState *cs, u_char offset)
{
	return (readreg(cs->hw.ax.isac_ale, cs->hw.ax.isac_adr, offset));
}

static void
WriteISAC(struct IsdnCardState *cs, u_char offset, u_char value)
{
	writereg(cs->hw.ax.isac_ale, cs->hw.ax.isac_adr, offset, value);
}

static void
ReadISACfifo(struct IsdnCardState *cs, u_char * data, int size)
{
	readfifo(cs->hw.ax.isac_ale, cs->hw.ax.isac_adr, 0, data, size);
}

static void
WriteISACfifo(struct IsdnCardState *cs, u_char * data, int size)
{
	writefifo(cs->hw.ax.isac_ale, cs->hw.ax.isac_adr, 0, data, size);
}

static u_char
ReadJADE(struct IsdnCardState *cs, int jade, u_char offset)
{
	return (readreg(cs->hw.ax.jade_ale, cs->hw.ax.jade_adr, offset + (jade == -1 ? 0 : (jade ? 0xC0 : 0x80))));
}

static void
WriteJADE(struct IsdnCardState *cs, int jade, u_char offset, u_char value)
{
	writereg(cs->hw.ax.jade_ale, cs->hw.ax.jade_adr, offset + (jade == -1 ? 0 : (jade ? 0xC0 : 0x80)), value);
}

/*
 * fast interrupt JADE stuff goes here
 */

#define READJADE(cs, nr, reg) readreg(cs->hw.ax.jade_ale,\
 		cs->hw.ax.jade_adr, reg + (nr == -1 ? 0 : (nr ? 0xC0 : 0x80)))
#define WRITEJADE(cs, nr, reg, data) writereg(cs->hw.ax.jade_ale,\
 		cs->hw.ax.jade_adr, reg + (nr == -1 ? 0 : (nr ? 0xC0 : 0x80)), data)

#define READJADEFIFO(cs, nr, ptr, cnt) readfifo(cs->hw.ax.jade_ale,\
		cs->hw.ax.jade_adr, (nr == -1 ? 0 : (nr ? 0xC0 : 0x80)), ptr, cnt)
#define WRITEJADEFIFO(cs, nr, ptr, cnt) writefifo( cs->hw.ax.jade_ale,\
		cs->hw.ax.jade_adr, (nr == -1 ? 0 : (nr ? 0xC0 : 0x80)), ptr, cnt)

#include "jade_irq.c"

static void
bkm_interrupt(int intno, void *dev_id, struct pt_regs *regs)
{
	struct IsdnCardState *cs = dev_id;
	u_char val = 0;
	I20_REGISTER_FILE *pI20_Regs;

	if (!cs) {
		printk(KERN_WARNING "HiSax: Telekom A4T: Spurious interrupt!\n");
		return;
	}
	pI20_Regs = (I20_REGISTER_FILE *) (cs->hw.ax.base);

	/* ISDN interrupt pending? */
	if (pI20_Regs->i20IntStatus & intISDN) {
		/* Reset the ISDN interrupt     */
		pI20_Regs->i20IntStatus = intISDN;
		/* Disable ISDN interrupt */
		pI20_Regs->i20IntCtrl &= ~intISDN;
		/* Channel A first */
		val = readreg(cs->hw.ax.jade_ale, cs->hw.ax.jade_adr, jade_HDLC_ISR + 0x80);
		if (val) {
			jade_int_main(cs, val, 0);
		}
		/* Channel B  */
		val = readreg(cs->hw.ax.jade_ale, cs->hw.ax.jade_adr, jade_HDLC_ISR + 0xC0);
		if (val) {
			jade_int_main(cs, val, 1);
		}
		/* D-Channel */
		val = readreg(cs->hw.ax.isac_ale, cs->hw.ax.isac_adr, ISAC_ISTA);
		if (val) {
			isac_interrupt(cs, val);
		}
		/* Reenable ISDN interrupt */
		pI20_Regs->i20IntCtrl |= intISDN;
	}
}

void
release_io_bkm(struct IsdnCardState *cs)
{
	if (cs->hw.ax.base) {
		iounmap((void *) cs->hw.ax.base);
		cs->hw.ax.base = 0;
	}
}

static void
enable_bkm_int(struct IsdnCardState *cs, unsigned bEnable)
{
	if (cs->typ == ISDN_CTYPE_BKM_A4T) {
		I20_REGISTER_FILE *pI20_Regs = (I20_REGISTER_FILE *) (cs->hw.ax.base);
		if (bEnable)
			pI20_Regs->i20IntCtrl |= (intISDN | intPCI);
		else
			/* CAUTION: This disables the video capture driver too */
			pI20_Regs->i20IntCtrl &= ~(intISDN | intPCI);
	}
}

static void
reset_bkm(struct IsdnCardState *cs)
{
	long flags;

	if (cs->typ == ISDN_CTYPE_BKM_A4T) {
		I20_REGISTER_FILE *pI20_Regs = (I20_REGISTER_FILE *) (cs->hw.ax.base);
		save_flags(flags);
		sti();
		/* Issue the I20 soft reset     */
		pI20_Regs->i20SysControl = 0xFF;	/* all in */
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout((10 * HZ) / 1000);
		/* Remove the soft reset */
		pI20_Regs->i20SysControl = sysRESET | 0xFF;
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout((10 * HZ) / 1000);
		/* Set our configuration */
		pI20_Regs->i20SysControl = sysRESET | sysCFG;
		/* Issue ISDN reset     */
		pI20_Regs->i20GuestControl = guestWAIT_CFG |
		    g_A4T_JADE_RES |
		    g_A4T_ISAR_RES |
		    g_A4T_ISAC_RES |
		    g_A4T_JADE_BOOTR |
		    g_A4T_ISAR_BOOTR;
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout((10 * HZ) / 1000);

		/* Remove RESET state from ISDN */
		pI20_Regs->i20GuestControl &= ~(g_A4T_ISAC_RES |
						g_A4T_JADE_RES |
						g_A4T_ISAR_RES);
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout((10 * HZ) / 1000);
		restore_flags(flags);
	}
}

static int
BKM_card_msg(struct IsdnCardState *cs, int mt, void *arg)
{
	switch (mt) {
		case CARD_RESET:
			/* Disable ints */
			enable_bkm_int(cs, 0);
			reset_bkm(cs);
			return (0);
		case CARD_RELEASE:
			/* Sanity */
			enable_bkm_int(cs, 0);
			reset_bkm(cs);
			release_io_bkm(cs);
			return (0);
		case CARD_INIT:
			clear_pending_isac_ints(cs);
			clear_pending_jade_ints(cs);
			initisac(cs);
			initjade(cs);
			/* Enable ints */
			enable_bkm_int(cs, 1);
			return (0);
		case CARD_TEST:
			return (0);
	}
	return (0);
}

static struct pci_dev *dev_a4t __initdata = NULL;

int __init
setup_bkm_a4t(struct IsdnCard *card)
{
	struct IsdnCardState *cs = card->cs;
	char tmp[64];
	u_int pci_memaddr = 0, found = 0;
	I20_REGISTER_FILE *pI20_Regs;
#if CONFIG_PCI
#endif

	strcpy(tmp, bkm_a4t_revision);
	printk(KERN_INFO "HiSax: T-Berkom driver Rev. %s\n", HiSax_getrev(tmp));
	if (cs->typ == ISDN_CTYPE_BKM_A4T) {
		cs->subtyp = BKM_A4T;
	} else
		return (0);

#if CONFIG_PCI
	if (!pci_present()) {
		printk(KERN_ERR "bkm_a4t: no PCI bus present\n");
		return (0);
	}
	while ((dev_a4t = pci_find_device(PCI_VENDOR_ID_ZORAN,
		PCI_DEVICE_ID_ZORAN_36120, dev_a4t))) {
		u16 sub_sys;
		u16 sub_vendor;

		sub_vendor = dev_a4t->subsystem_vendor;
		sub_sys = dev_a4t->subsystem_device;
		if ((sub_sys == PCI_DEVICE_ID_BERKOM_A4T) && (sub_vendor == PCI_VENDOR_ID_BERKOM)) {
			if (pci_enable_device(dev_a4t))
				return(0);
			found = 1;
			pci_memaddr = pci_resource_start(dev_a4t, 0);
			cs->irq = dev_a4t->irq;
			break;
		}
	}
	if (!found) {
		printk(KERN_WARNING "HiSax: %s: Card not found\n", CardType[card->typ]);
		return (0);
	}
	if (!cs->irq) {		/* IRQ range check ?? */
		printk(KERN_WARNING "HiSax: %s: No IRQ\n", CardType[card->typ]);
		return (0);
	}
	if (!pci_memaddr) {
		printk(KERN_WARNING "HiSax: %s: No Memory base address\n", CardType[card->typ]);
		return (0);
	}
	cs->hw.ax.base = (long) ioremap(pci_memaddr, 4096);
	/* Check suspecious address */
	pI20_Regs = (I20_REGISTER_FILE *) (cs->hw.ax.base);
	if ((pI20_Regs->i20IntStatus & 0x8EFFFFFF) != 0) {
		printk(KERN_WARNING "HiSax: %s address %lx-%lx suspecious\n",
		       CardType[card->typ], cs->hw.ax.base, cs->hw.ax.base + 4096);
		iounmap((void *) cs->hw.ax.base);
		cs->hw.ax.base = 0;
		return (0);
	}
	cs->hw.ax.isac_adr = cs->hw.ax.base + PO_OFFSET;
	cs->hw.ax.jade_adr = cs->hw.ax.base + PO_OFFSET;
	cs->hw.ax.isac_ale = GCS_1;
	cs->hw.ax.jade_ale = GCS_3;
#else
	printk(KERN_WARNING "HiSax: %s: NO_PCI_BIOS\n", CardType[card->typ]);
	printk(KERN_WARNING "HiSax: %s: unable to configure\n", CardType[card->typ]);
	return (0);
#endif				/* CONFIG_PCI */
	printk(KERN_INFO "HiSax: %s: Card configured at 0x%lX IRQ %d\n",
	       CardType[card->typ], cs->hw.ax.base, cs->irq);

	reset_bkm(cs);
	cs->readisac = &ReadISAC;
	cs->writeisac = &WriteISAC;
	cs->readisacfifo = &ReadISACfifo;
	cs->writeisacfifo = &WriteISACfifo;
	cs->BC_Read_Reg = &ReadJADE;
	cs->BC_Write_Reg = &WriteJADE;
	cs->BC_Send_Data = &jade_fill_fifo;
	cs->cardmsg = &BKM_card_msg;
	cs->irq_func = &bkm_interrupt;
	cs->irq_flags |= SA_SHIRQ;
	ISACVersion(cs, "Telekom A4T:");
	/* Jade version */
	JadeVersion(cs, "Telekom A4T:");
	return (1);
}
