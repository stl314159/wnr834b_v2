/* $Id: cf-enabler.c,v 1.1.1.1 2010/03/05 07:31:15 reynolds Exp $
 *
 *  linux/drivers/block/cf-enabler.c
 *
 *  Copyright (C) 1999  Niibe Yutaka
 *  Copyright (C) 2000  Toshiharu Nozawa
 *  Copyright (C) 2001  A&D Co., Ltd.
 *
 *  Enable the CF configuration.
 */

#include <linux/config.h>
#include <linux/init.h>

#include <asm/io.h>
#include <asm/irq.h>

/*
 * You can connect Compact Flash directly to the bus of SuperH.
 * This is the enabler for that.
 *
 * SIM: How generic is this really? It looks pretty board, or at
 * least SH sub-type, specific to me.
 * I know it doesn't work on the Overdrive!
 */

/*
 * 0xB8000000 : Attribute
 * 0xB8001000 : Common Memory
 * 0xBA000000 : I/O
 */
#if defined(CONFIG_IDE) && defined(__SH4__)
/* SH4 can't access PCMCIA interface through P2 area.
 * we must remap it with appropreate attribute bit of the page set.
 * this part is based on Greg Banks' hd64465_ss.c implementation - Masahiro Abe */
#include <linux/mm.h>
#include <linux/vmalloc.h>

#if defined(CONFIG_CF_AREA6)
#define slot_no 0
#else
#define slot_no 1
#endif

/* defined in mm/ioremap.c */
extern void * p3_ioremap(unsigned long phys_addr, unsigned long size, unsigned long flags);

/* use this pointer to access to directly connected compact flash io area*/
void *cf_io_base;

static int __init allocate_cf_area(void)
{
	pgprot_t prot;
	unsigned long paddrbase, psize;

	/* open I/O area window */
	paddrbase = virt_to_phys((void*)CONFIG_CF_BASE_ADDR);
	psize = PAGE_SIZE;
	prot = PAGE_KERNEL_PCC(slot_no, _PAGE_PCC_IO16);
	cf_io_base = p3_ioremap(paddrbase, psize, prot.pgprot);
	if (!cf_io_base) {
		printk("allocate_cf_area : can't open CF I/O window!\n");
		return -ENOMEM;
	}
/*	printk("p3_ioremap(paddr=0x%08lx, psize=0x%08lx, prot=0x%08lx)=0x%08lx\n",
	    	paddrbase, psize, prot.pgprot, cf_io_base);*/


	return 0;
}
#endif

static int __init cf_init_default(void)
{
/* You must have enabled the card, and set the level interrupt
 * before reaching this point. Possibly in boot ROM or boot loader.
 */
#if defined(CONFIG_IDE) && defined(__SH4__)
	allocate_cf_area();
#endif
#if defined(CONFIG_SH_UNKNOWN)
	/* This should be done in each board's init_xxx_irq. */
	make_imask_irq(14);
	disable_irq(14);
#endif
	return 0;
}

#if defined(CONFIG_SH_GENERIC) || defined(CONFIG_SH_SOLUTION_ENGINE)
#include <asm/hitachi_se.h>

/*
 * SolutionEngine
 *
 * 0xB8400000 : Common Memory
 * 0xB8500000 : Attribute
 * 0xB8600000 : I/O
 */

static int __init cf_init_se(void)
{
	if ((ctrl_inw(MRSHPC_CSR) & 0x000c) != 0)
		return 0;	/* Not detected */

	if ((ctrl_inw(MRSHPC_CSR) & 0x0080) == 0) {
		ctrl_outw(0x0674, MRSHPC_CPWCR); /* Card Vcc is 3.3v? */
	} else {
		ctrl_outw(0x0678, MRSHPC_CPWCR); /* Card Vcc is 5V */
	}

	/*
	 *  PC-Card window open 
	 *  flag == COMMON/ATTRIBUTE/IO
	 */
	/* common window open */
	ctrl_outw(0x8a84, MRSHPC_MW0CR1);/* window 0xb8400000 */
	if((ctrl_inw(MRSHPC_CSR) & 0x4000) != 0)
		/* common mode & bus width 16bit SWAP = 1*/
		ctrl_outw(0x0b00, MRSHPC_MW0CR2);
	else
		/* common mode & bus width 16bit SWAP = 0*/
		ctrl_outw(0x0300, MRSHPC_MW0CR2); 

	/* attribute window open */
	ctrl_outw(0x8a85, MRSHPC_MW1CR1);/* window 0xb8500000 */
	if ((ctrl_inw(MRSHPC_CSR) & 0x4000) != 0)
		/* attribute mode & bus width 16bit SWAP = 1*/
		ctrl_outw(0x0a00, MRSHPC_MW1CR2);
	else
		/* attribute mode & bus width 16bit SWAP = 0*/
		ctrl_outw(0x0200, MRSHPC_MW1CR2);

	/* I/O window open */
	ctrl_outw(0x8a86, MRSHPC_IOWCR1);/* I/O window 0xb8600000 */
	ctrl_outw(0x0008, MRSHPC_CDCR);	 /* I/O card mode */
	if ((ctrl_inw(MRSHPC_CSR) & 0x4000) != 0)
		ctrl_outw(0x0a00, MRSHPC_IOWCR2); /* bus width 16bit SWAP = 1*/
	else
		ctrl_outw(0x0200, MRSHPC_IOWCR2); /* bus width 16bit SWAP = 0*/

	ctrl_outw(0x2000, MRSHPC_ICR);
	ctrl_outb(0x00, PA_MRSHPC_MW2 + 0x206);
	ctrl_outb(0x42, PA_MRSHPC_MW2 + 0x200);
	return 0;
}
#endif

int __init cf_init(void)
{
#if defined(CONFIG_SH_GENERIC) || defined(CONFIG_SH_SOLUTION_ENGINE)
	if (MACH_SE)
		return cf_init_se();
#endif
	return cf_init_default();
}

__initcall (cf_init);
