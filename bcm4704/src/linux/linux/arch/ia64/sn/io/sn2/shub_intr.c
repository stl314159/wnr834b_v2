/* $Id: shub_intr.c,v 1.1.1.1 2010/03/05 07:31:13 reynolds Exp $
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1992-1997, 2000-2002 Silicon Graphics, Inc.  All Rights Reserved.
 */

#include <linux/types.h>
#include <linux/slab.h>
#include <asm/sn/types.h>
#include <asm/sn/sgi.h>
#include <asm/sn/driver.h>
#include <asm/sn/iograph.h>
#include <asm/param.h>
#include <asm/sn/pio.h>
#include <asm/sn/xtalk/xwidget.h>
#include <asm/sn/io.h>
#include <asm/sn/sn_private.h>
#include <asm/sn/addrs.h>
#include <asm/sn/invent.h>
#include <asm/sn/hcl.h>
#include <asm/sn/hcl_util.h>
#include <asm/sn/intr.h>
#include <asm/sn/xtalk/xtalkaddrs.h>
#include <asm/sn/klconfig.h>
#include <asm/sn/sn_cpuid.h>

extern void hub_device_desc_update(device_desc_t, ilvl_t, cpuid_t);

/* ARGSUSED */
void
hub_intr_init(devfs_handle_t hubv)
{
	extern void sn_cpei_handler(int, void *, struct pt_regs *);
	extern void sn_init_cpei_timer(void);

	if (request_irq(SGI_SHUB_ERROR_VECTOR, sn_cpei_handler, 0, "SN hub error", NULL) ) {
		printk("hub_intr_init: Couldn't register SGI_SHUB_ERROR_VECTOR = %x\n",SGI_SHUB_ERROR_VECTOR);
	}
	sn_init_cpei_timer();
}

xwidgetnum_t
hub_widget_id(nasid_t nasid)
{
        hubii_wcr_t     ii_wcr; /* the control status register */
        
        ii_wcr.wcr_reg_value = REMOTE_HUB_L(nasid,IIO_WCR);
        
        return ii_wcr.wcr_fields_s.wcr_widget_id;
}

static hub_intr_t
do_hub_intr_alloc(devfs_handle_t dev,
		device_desc_t dev_desc,
		devfs_handle_t owner_dev,
		int uncond_nothread)
{
	cpuid_t		cpu = 0;
	int		vector;
	hub_intr_t	intr_hdl;
	cnodeid_t	cnode;
	int		cpuphys, slice;
	int		nasid;
	iopaddr_t	xtalk_addr;
	struct xtalk_intr_s	*xtalk_info;
	xwidget_info_t	xwidget_info;
	ilvl_t		intr_swlevel = 0;

	cpu = intr_heuristic(dev, dev_desc, -1, 0, owner_dev, NULL, &vector);

	if (cpu == CPU_NONE) {
		printk("Unable to allocate interrupt for 0x%p\n", (void *)owner_dev);
		return(0);
	}

	cpuphys = cpu_physical_id(cpu);
	slice = cpu_physical_id_to_slice(cpuphys);
	nasid = cpu_physical_id_to_nasid(cpuphys);
	cnode = cpuid_to_cnodeid(cpu);

	if (slice) {
		xtalk_addr = SH_II_INT1 | GLOBAL_MMR_SPACE |
			((unsigned long)nasid << 36) | (1UL << 47);
	} else {
		xtalk_addr = SH_II_INT0 | GLOBAL_MMR_SPACE |
			((unsigned long)nasid << 36) | (1UL << 47);
	}

	intr_hdl = snia_kmem_alloc_node(sizeof(struct hub_intr_s), KM_NOSLEEP, cnode);
	ASSERT_ALWAYS(intr_hdl);

	xtalk_info = &intr_hdl->i_xtalk_info;
	xtalk_info->xi_dev = dev;
	xtalk_info->xi_vector = vector;
	xtalk_info->xi_addr = xtalk_addr;

	xwidget_info = xwidget_info_get(dev);
	if (xwidget_info) {
		xtalk_info->xi_target = xwidget_info_masterid_get(xwidget_info);
	}

	intr_hdl->i_swlevel = intr_swlevel;
	intr_hdl->i_cpuid = cpu;
	intr_hdl->i_bit = vector;
	intr_hdl->i_flags |= HUB_INTR_IS_ALLOCED;

	hub_device_desc_update(dev_desc, intr_swlevel, cpu);
	return(intr_hdl);
}

hub_intr_t
hub_intr_alloc(devfs_handle_t dev,
		device_desc_t dev_desc,
		devfs_handle_t owner_dev)
{
	return(do_hub_intr_alloc(dev, dev_desc, owner_dev, 0));
}

hub_intr_t
hub_intr_alloc_nothd(devfs_handle_t dev,
		device_desc_t dev_desc,
		devfs_handle_t owner_dev)
{
	return(do_hub_intr_alloc(dev, dev_desc, owner_dev, 1));
}

void
hub_intr_free(hub_intr_t intr_hdl)
{
	cpuid_t		cpu = intr_hdl->i_cpuid;
	int		vector = intr_hdl->i_bit;
	xtalk_intr_t	xtalk_info;

	if (intr_hdl->i_flags & HUB_INTR_IS_CONNECTED) {
		xtalk_info = &intr_hdl->i_xtalk_info;
		xtalk_info->xi_dev = NODEV;
		xtalk_info->xi_vector = 0;
		xtalk_info->xi_addr = 0;
		hub_intr_disconnect(intr_hdl);
	}

	if (intr_hdl->i_flags & HUB_INTR_IS_ALLOCED) {
		kfree(intr_hdl);
	}
	intr_unreserve_level(cpu, vector);
}

int
hub_intr_connect(hub_intr_t intr_hdl,
		xtalk_intr_setfunc_t setfunc,
		void *setfunc_arg)
{
	int		rv;
	cpuid_t		cpu = intr_hdl->i_cpuid;
	int 		vector = intr_hdl->i_bit;

	ASSERT(intr_hdl->i_flags & HUB_INTR_IS_ALLOCED);

	rv = intr_connect_level(cpu, vector, intr_hdl->i_swlevel, NULL);

	if (rv < 0) {
		return rv;
	}

	intr_hdl->i_xtalk_info.xi_setfunc = setfunc;
	intr_hdl->i_xtalk_info.xi_sfarg = setfunc_arg;

	if (setfunc) {
		(*setfunc)((xtalk_intr_t)intr_hdl);
	}

	intr_hdl->i_flags |= HUB_INTR_IS_CONNECTED;

	return 0;
}

/*
 * Disassociate handler with the specified interrupt.
 */
void
hub_intr_disconnect(hub_intr_t intr_hdl)
{
	/*REFERENCED*/
	int rv;
	cpuid_t cpu = intr_hdl->i_cpuid;
	int bit = intr_hdl->i_bit;
	xtalk_intr_setfunc_t setfunc;

	setfunc = intr_hdl->i_xtalk_info.xi_setfunc;

	/* TBD: send disconnected interrupts somewhere harmless */
	if (setfunc) (*setfunc)((xtalk_intr_t)intr_hdl);

	rv = intr_disconnect_level(cpu, bit);
	ASSERT(rv == 0);
	intr_hdl->i_flags &= ~HUB_INTR_IS_CONNECTED;
}


/*
 * Return a hwgraph vertex that represents the CPU currently
 * targeted by an interrupt.
 */
devfs_handle_t
hub_intr_cpu_get(hub_intr_t intr_hdl)
{
	cpuid_t cpuid = intr_hdl->i_cpuid;

	ASSERT(cpuid != CPU_NONE);

	return(cpuid_to_vertex(cpuid));
}
