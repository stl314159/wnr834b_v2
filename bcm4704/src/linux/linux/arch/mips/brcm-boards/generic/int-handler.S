/*
 * Generic interrupt handler for Broadcom MIPS boards
 *
 * Copyright 2007, Broadcom Corporation
 * All Rights Reserved.
 * 
 * THIS SOFTWARE IS OFFERED "AS IS", AND BROADCOM GRANTS NO WARRANTIES OF ANY
 * KIND, EXPRESS OR IMPLIED, BY STATUTE, COMMUNICATION OR OTHERWISE. BROADCOM
 * SPECIFICALLY DISCLAIMS ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A SPECIFIC PURPOSE OR NONINFRINGEMENT CONCERNING THIS SOFTWARE.
 *
 * $Id: int-handler.S,v 1.1.1.1 2010/03/05 07:31:14 reynolds Exp $
 */

#include <linux/config.h>

#include <asm/asm.h>
#include <asm/mipsregs.h>
#include <asm/regdef.h>
#include <asm/stackframe.h>

/*
 *	MIPS IRQ	Source
 *      --------        ------
 *             0	Software (ignored)
 *             1        Software (ignored)
 *             2        Combined hardware interrupt (hw0)
 *             3        Hardware
 *             4        Hardware
 *             5        Hardware
 *             6        Hardware
 *             7        R4k timer
 */

	.text
	.set	noreorder
	.set	noat
	.align	5
	NESTED(brcmIRQ, PT_SIZE, sp)
	SAVE_ALL
	CLI
	.set	at
	.set    noreorder

	jal	brcm_irq_dispatch
	move	a0, sp

	j	ret_from_irq
	nop

	END(brcmIRQ)
