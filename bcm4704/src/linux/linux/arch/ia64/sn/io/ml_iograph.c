/* $Id: ml_iograph.c,v 1.1.1.1 2010/03/05 07:31:12 reynolds Exp $
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1992 - 1997, 2000-2002 Silicon Graphics, Inc. All rights reserved.
 */

#include <linux/types.h>
#include <linux/config.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <asm/sn/sgi.h>
#include <asm/sn/sn_sal.h>
#include <asm/sn/io.h>
#include <asm/sn/sn_cpuid.h>
#include <asm/sn/iograph.h>
#include <asm/sn/invent.h>
#include <asm/sn/hcl.h>
#include <asm/sn/hcl_util.h>
#include <asm/sn/labelcl.h>
#include <asm/sn/xtalk/xbow.h>
#include <asm/sn/pci/bridge.h>
#include <asm/sn/klconfig.h>
#include <asm/sn/eeprom.h>
#include <asm/sn/sn_private.h>
#include <asm/sn/pci/pcibr.h>
#include <asm/sn/xtalk/xtalk.h>
#include <asm/sn/xtalk/xswitch.h>
#include <asm/sn/xtalk/xwidget.h>
#include <asm/sn/xtalk/xtalk_private.h>
#include <asm/sn/xtalk/xtalkaddrs.h>

/* #define IOGRAPH_DEBUG */
#ifdef IOGRAPH_DEBUG
#define DBG(x...) printk(x)
#else
#define DBG(x...)
#endif /* IOGRAPH_DEBUG */

/* #define PROBE_TEST */

/* At most 2 hubs can be connected to an xswitch */
#define NUM_XSWITCH_VOLUNTEER 2

/*
 * Track which hubs have volunteered to manage devices hanging off of
 * a Crosstalk Switch (e.g. xbow).  This structure is allocated,
 * initialized, and hung off the xswitch vertex early on when the
 * xswitch vertex is created.
 */
typedef struct xswitch_vol_s {
	mutex_t xswitch_volunteer_mutex;
	int		xswitch_volunteer_count;
	devfs_handle_t	xswitch_volunteer[NUM_XSWITCH_VOLUNTEER];
} *xswitch_vol_t;

void
xswitch_vertex_init(devfs_handle_t xswitch)
{
	xswitch_vol_t xvolinfo;
	int rc;

	xvolinfo = kmalloc(sizeof(struct xswitch_vol_s), GFP_KERNEL);
	mutex_init(&xvolinfo->xswitch_volunteer_mutex);
	xvolinfo->xswitch_volunteer_count = 0;
	rc = hwgraph_info_add_LBL(xswitch, 
			INFO_LBL_XSWITCH_VOL,
			(arbitrary_info_t)xvolinfo);
	ASSERT(rc == GRAPH_SUCCESS); rc = rc;
}


/*
 * When assignment of hubs to widgets is complete, we no longer need the
 * xswitch volunteer structure hanging around.  Destroy it.
 */
static void
xswitch_volunteer_delete(devfs_handle_t xswitch)
{
	xswitch_vol_t xvolinfo;
	int rc;

	rc = hwgraph_info_remove_LBL(xswitch, 
				INFO_LBL_XSWITCH_VOL,
				(arbitrary_info_t *)&xvolinfo);
#ifdef LATER
	ASSERT(rc == GRAPH_SUCCESS); rc = rc;
#endif

	kfree(xvolinfo);
}
/*
 * A Crosstalk master volunteers to manage xwidgets on the specified xswitch.
 */
/* ARGSUSED */
static void
volunteer_for_widgets(devfs_handle_t xswitch, devfs_handle_t master)
{
	xswitch_vol_t xvolinfo = NULL;

	(void)hwgraph_info_get_LBL(xswitch, 
				INFO_LBL_XSWITCH_VOL, 
				(arbitrary_info_t *)&xvolinfo);
	if (xvolinfo == NULL) {
#ifdef LATER
	    if (!is_headless_node_vertex(master)) {
#if defined(SUPPORT_PRINTING_V_FORMAT)
		printk(KERN_WARNING  "volunteer for widgets: vertex %v has no info label",
			xswitch);
#else
		printk(KERN_WARNING  "volunteer for widgets: vertex 0x%x has no info label",
			xswitch);
#endif
	    }
#endif	/* LATER */
	    return;
	}

	mutex_lock(&xvolinfo->xswitch_volunteer_mutex);
	ASSERT(xvolinfo->xswitch_volunteer_count < NUM_XSWITCH_VOLUNTEER);
	xvolinfo->xswitch_volunteer[xvolinfo->xswitch_volunteer_count] = master;
	xvolinfo->xswitch_volunteer_count++;
	mutex_unlock(&xvolinfo->xswitch_volunteer_mutex);
}

extern int xbow_port_io_enabled(nasid_t nasid, int widgetnum);

/*
 * Assign all the xwidgets hanging off the specified xswitch to the
 * Crosstalk masters that have volunteered for xswitch duty.
 */
/* ARGSUSED */
static void
assign_widgets_to_volunteers(devfs_handle_t xswitch, devfs_handle_t hubv)
{
	int curr_volunteer, num_volunteer;
	xwidgetnum_t widgetnum;
	xswitch_info_t xswitch_info;
	xswitch_vol_t xvolinfo = NULL;
	nasid_t nasid;
	hubinfo_t hubinfo;

	hubinfo_get(hubv, &hubinfo);
	nasid = hubinfo->h_nasid;
	
	xswitch_info = xswitch_info_get(xswitch);
	ASSERT(xswitch_info != NULL);

	(void)hwgraph_info_get_LBL(xswitch, 
				INFO_LBL_XSWITCH_VOL, 
				(arbitrary_info_t *)&xvolinfo);
	if (xvolinfo == NULL) {
#ifdef LATER
	    if (!is_headless_node_vertex(hubv)) {
#if defined(SUPPORT_PRINTING_V_FORMAT)
		printk(KERN_WARNING  "assign_widgets_to_volunteers:vertex %v has "
			" no info label",
			xswitch);
#else
		printk(KERN_WARNING  "assign_widgets_to_volunteers:vertex 0x%x has "
			" no info label",
			xswitch);
#endif
	    }
#endif	/* LATER */
	    return;
	}

	num_volunteer = xvolinfo->xswitch_volunteer_count;
	ASSERT(num_volunteer > 0);
	curr_volunteer = 0;

	/* Assign master hub for xswitch itself.  */
	if (HUB_WIDGET_ID_MIN > 0) {
		hubv = xvolinfo->xswitch_volunteer[0];
		xswitch_info_master_assignment_set(xswitch_info, (xwidgetnum_t)0, hubv);
	}

	/*
	 * TBD: Use administrative information to alter assignment of
	 * widgets to hubs.
	 */
	for (widgetnum=HUB_WIDGET_ID_MIN; widgetnum <= HUB_WIDGET_ID_MAX; widgetnum++) {

		/*
		 * Ignore disabled/empty ports.
		 */
		if (!xbow_port_io_enabled(nasid, widgetnum)) 
		    continue;

		/*
		 * If this is the master IO board, assign it to the same 
		 * hub that owned it in the prom.
		 */
		if (is_master_nasid_widget(nasid, widgetnum)) {
			int i;

			for (i=0; i<num_volunteer; i++) {
				hubv = xvolinfo->xswitch_volunteer[i];
				hubinfo_get(hubv, &hubinfo);
				nasid = hubinfo->h_nasid;
				if (nasid == get_console_nasid())
					goto do_assignment;
			}
#ifdef LATER
			PRINT_PANIC("Nasid == %d, console nasid == %d",
				nasid, get_console_nasid());
#endif
		}


		/*
		 * Do a round-robin assignment among the volunteer nodes.
		 */
		hubv = xvolinfo->xswitch_volunteer[curr_volunteer];
		curr_volunteer = (curr_volunteer + 1) % num_volunteer;
		/* fall through */

do_assignment:
		/*
		 * At this point, we want to make hubv the master of widgetnum.
		 */
		xswitch_info_master_assignment_set(xswitch_info, widgetnum, hubv);
	}

	xswitch_volunteer_delete(xswitch);
}

/*
 * Early iograph initialization.  Called by master CPU in mlreset().
 * Useful for including iograph.o in kernel.o.
 */
void
iograph_early_init(void)
{
/*
 * Need new way to get this information ..
 */
	cnodeid_t cnode;
	nasid_t nasid;
	lboard_t *board;
	
	/*
	 * Init. the board-to-hwgraph link early, so FRU analyzer
	 * doesn't trip on leftover values if we panic early on.
	 */
	for(cnode = 0; cnode < numnodes; cnode++) {
		nasid = COMPACT_TO_NASID_NODEID(cnode);
		board = (lboard_t *)KL_CONFIG_INFO(nasid);
		DBG("iograph_early_init: Found board 0x%p\n", board);

		/* Check out all the board info stored on a node */
		while(board) {
			board->brd_graph_link = GRAPH_VERTEX_NONE;
			board = KLCF_NEXT(board);
			DBG("iograph_early_init: Found board 0x%p\n", board);


		}
	}

	hubio_init();
}

#ifdef LINUX_KERNEL_THREADS
static struct semaphore io_init_sema;
#endif

/*
 * Let boot processor know that we're done initializing our node's IO
 * and then exit.
 */
/* ARGSUSED */
static void
io_init_done(cnodeid_t cnodeid,cpu_cookie_t c)
{
	/* Let boot processor know that we're done. */
#ifdef LINUX_KERNEL_THREADS
	up(&io_init_sema);
#endif
#ifdef LATER
	/* This is for the setnoderun done when the io_init thread
	 * started 
	 */
	restorenoderun(c);
	sthread_exit();
#endif
}

/* 
 * Probe to see if this hub's xtalk link is active.  If so,
 * return the Crosstalk Identification of the widget that we talk to.  
 * This is called before any of the Crosstalk infrastructure for 
 * this hub is set up.  It's usually called on the node that we're
 * probing, but not always.
 *
 * TBD: Prom code should actually do this work, and pass through 
 * hwid for our use.
 */
static void
early_probe_for_widget(devfs_handle_t hubv, xwidget_hwid_t hwid)
{
	hubreg_t llp_csr_reg;
	nasid_t nasid;
	hubinfo_t hubinfo;

	hubinfo_get(hubv, &hubinfo);
	nasid = hubinfo->h_nasid;

	llp_csr_reg = REMOTE_HUB_L(nasid, IIO_LLP_CSR);
	/* 
	 * If link is up, read the widget's part number.
	 * A direct connect widget must respond to widgetnum=0.
	 */
	if (llp_csr_reg & IIO_LLP_CSR_IS_UP) {
		/* TBD: Put hub into "indirect" mode */
		/*
		 * We're able to read from a widget because our hub's 
		 * WIDGET_ID was set up earlier.
		 */
		widgetreg_t widget_id = *(volatile widgetreg_t *)
			(RAW_NODE_SWIN_BASE(nasid, 0x0) + WIDGET_ID);

		DBG("early_probe_for_widget: Hub Vertex 0x%p is UP widget_id = 0x%x Register 0x%p\n", hubv, widget_id,
		(volatile widgetreg_t *)(RAW_NODE_SWIN_BASE(nasid, 0x0) + WIDGET_ID) );

		hwid->part_num = XWIDGET_PART_NUM(widget_id);
		hwid->rev_num = XWIDGET_REV_NUM(widget_id);
		hwid->mfg_num = XWIDGET_MFG_NUM(widget_id);

		/* TBD: link reset */
	} else {

		hwid->part_num = XWIDGET_PART_NUM_NONE;
		hwid->rev_num = XWIDGET_REV_NUM_NONE;
		hwid->mfg_num = XWIDGET_MFG_NUM_NONE;
	}

}

/* Add inventory information to the widget vertex 
 * Right now (module,slot,revision) is being
 * added as inventory information.
 */
static void
xwidget_inventory_add(devfs_handle_t 		widgetv,
		      lboard_t 			*board,
		      struct xwidget_hwid_s 	hwid)
{
	if (!board)
		return;
	/* Donot add inventory information for the baseio
	 * on a speedo with an xbox. It has already been
	 * taken care of in SN00_vmc.
	 * Speedo with xbox's baseio comes in at slot io1 (widget 9)
	 */
	device_inventory_add(widgetv,INV_IOBD,board->brd_type,
			     board->brd_module,
			     SLOTNUM_GETSLOT(board->brd_slot),
			     hwid.rev_num);
}

/*
 * io_xswitch_widget_init
 *	
 */

/* defined in include/linux/ctype.h  */
/* #define toupper(c)	(islower(c) ? (c) - 'a' + 'A' : (c)) */

void
io_xswitch_widget_init(devfs_handle_t  	xswitchv,
		       devfs_handle_t	hubv,
		       xwidgetnum_t	widgetnum,
		       async_attach_t	aa)
{
	xswitch_info_t		xswitch_info;
	xwidgetnum_t		hub_widgetid;
	devfs_handle_t		widgetv;
	cnodeid_t		cnode;
	widgetreg_t		widget_id;
	nasid_t			nasid, peer_nasid;
	struct xwidget_hwid_s 	hwid;
	hubinfo_t		hubinfo;
	/*REFERENCED*/
	int			rc;
	char			slotname[SLOTNUM_MAXLENGTH];
	char 			pathname[128];
	char			new_name[64];
	moduleid_t		module;
	slotid_t		slot;
	lboard_t		*board = NULL;
	char			buffer[16];
	slotid_t get_widget_slotnum(int xbow, int widget);
	
	DBG("\nio_xswitch_widget_init: hubv 0x%p, xswitchv 0x%p, widgetnum 0x%x\n", hubv, xswitchv, widgetnum);
	/*
	 * Verify that xswitchv is indeed an attached xswitch.
	 */
	xswitch_info = xswitch_info_get(xswitchv);
	ASSERT(xswitch_info != NULL);

	hubinfo_get(hubv, &hubinfo);
	nasid = hubinfo->h_nasid;
	cnode = NASID_TO_COMPACT_NODEID(nasid);
	hub_widgetid = hubinfo->h_widgetid;


	/* Who's the other guy on out crossbow (if anyone) */
	peer_nasid = NODEPDA(cnode)->xbow_peer;
	if (peer_nasid == INVALID_NASID)
		/* If I don't have a peer, use myself. */
		peer_nasid = nasid;


	/* Check my xbow structure and my peer's */
	if (!xbow_port_io_enabled(nasid, widgetnum) &&
	    !xbow_port_io_enabled(peer_nasid, widgetnum)) {
		return;
	}

	if (xswitch_info_link_ok(xswitch_info, widgetnum)) {
		char			name[4];
		/*
		 * If the current hub is not supposed to be the master 
		 * for this widgetnum, then skip this widget.
		 */
		if (xswitch_info_master_assignment_get(xswitch_info,
						       widgetnum) != hubv) {
			return;
		}

		module  = NODEPDA(cnode)->module_id;
#ifdef XBRIDGE_REGS_SIM
		/* hardwire for now...could do this with something like:
		 * xbow_soft_t soft = hwgraph_fastinfo_get(vhdl);
		 * xbow_t xbow = soft->base;
		 * xbowreg_t xwidget_id = xbow->xb_wid_id;
		 * but I don't feel like figuring out vhdl right now..
		 * and I know for a fact the answer is 0x2d000049 
		 */
		DBG("io_xswitch_widget_init: XBRIDGE_REGS_SIM FIXME: reading xwidget id: hardwired to xbridge (0x2d000049).\n");
		DBG("XWIDGET_PART_NUM(0x2d000049)= 0x%x\n", XWIDGET_PART_NUM(0x2d000049));
		if (XWIDGET_PART_NUM(0x2d000049)==XXBOW_WIDGET_PART_NUM) {
#else
		if (nasid_has_xbridge(nasid)) {
#endif /* XBRIDGE_REGS_SIM */
			board = find_lboard_module_class(
				(lboard_t *)KL_CONFIG_INFO(nasid),
				module,
				KLTYPE_IOBRICK);

DBG("io_xswitch_widget_init: Board 0x%p\n", board);
{
		lboard_t dummy;


			if (board) {
				DBG("io_xswitch_widget_init: Found KLTYPE_IOBRICK Board 0x%p brd_type 0x%x\n", board, board->brd_type);
			} else {
				DBG("io_xswitch_widget_init: FIXME did not find IOBOARD\n");
				board = &dummy;
			}
				
}

			/*
	 		 * Make sure we really want to say xbrick, pbrick,
			 * etc. rather than XIO, graphics, etc.
	 		 */

#ifdef SUPPORT_PRINTING_M_FORMAT
			sprintf(pathname, EDGE_LBL_MODULE "/%M/"
				"%cbrick" "/%s/%d",
				NODEPDA(cnode)->module_id,
				
#else
			memset(buffer, 0, 16);
			format_module_id(buffer, NODEPDA(cnode)->module_id, MODULE_FORMAT_BRIEF);
			sprintf(pathname, EDGE_LBL_MODULE "/%s/"
				"%cbrick" "/%s/%d",
				buffer,
#endif

				(board->brd_type == KLTYPE_IBRICK) ? 'I' :
				(board->brd_type == KLTYPE_PBRICK) ? 'P' :
				(board->brd_type == KLTYPE_XBRICK) ? 'X' : '?',
				EDGE_LBL_XTALK, widgetnum);
		} 
		
		DBG("io_xswitch_widget_init: path= %s\n", pathname);
		rc = hwgraph_path_add(hwgraph_root, pathname, &widgetv);
		
		ASSERT(rc == GRAPH_SUCCESS);

		/* This is needed to let the user programs to map the
		 * module,slot numbers to the corresponding widget numbers
		 * on the crossbow.
		 */
		rc = device_master_set(hwgraph_connectpt_get(widgetv), hubv);

		/* If we are looking at the global master io6
		 * then add information about the version of
		 * the io6prom as a part of "detailed inventory"
		 * information.
		 */
		if (is_master_baseio(nasid,
				     NODEPDA(cnode)->module_id,
 				     get_widget_slotnum(0,widgetnum))) {
			extern void klhwg_baseio_inventory_add(devfs_handle_t,
							       cnodeid_t);
			module 	= NODEPDA(cnode)->module_id;

#ifdef XBRIDGE_REGS_SIM
			DBG("io_xswitch_widget_init: XBRIDGE_REGS_SIM FIXME: reading xwidget id: hardwired to xbridge (0x2d000049).\n");
			if (XWIDGET_PART_NUM(0x2d000049)==XXBOW_WIDGET_PART_NUM) {
#else
			if (nasid_has_xbridge(nasid)) {
#endif /* XBRIDGE_REGS_SIM */
				board = find_lboard_module(
					(lboard_t *)KL_CONFIG_INFO(nasid),
					module);
				/*
				 * Change iobrick to correct i/o brick
				 */
#ifdef SUPPORT_PRINTING_M_FORMAT
				sprintf(pathname, EDGE_LBL_MODULE "/%M/"
#else
				sprintf(pathname, EDGE_LBL_MODULE "/%x/"
#endif
					"iobrick" "/%s/%d",
					NODEPDA(cnode)->module_id,
					EDGE_LBL_XTALK, widgetnum);
			} else {
				slot = get_widget_slotnum(0, widgetnum);
				board = get_board_name(nasid, module, slot,
								new_name);
				/*
			 	 * Create the vertex for the widget, 
				 * using the decimal 
			 	 * widgetnum as the name of the primary edge.
			 	 */
#ifdef SUPPORT_PRINTING_M_FORMAT
				sprintf(pathname, EDGE_LBL_MODULE "/%M/"
                                                EDGE_LBL_SLOT "/%s/%s",
                                        NODEPDA(cnode)->module_id,
                                        slotname, new_name);
#else
				memset(buffer, 0, 16);
				format_module_id(buffer, NODEPDA(cnode)->module_id, MODULE_FORMAT_BRIEF);
				sprintf(pathname, EDGE_LBL_MODULE "/%s/"
					  	EDGE_LBL_SLOT "/%s/%s",
					buffer,
					slotname, new_name);
#endif
			}

			rc = hwgraph_path_add(hwgraph_root, pathname, &widgetv);
			DBG("io_xswitch_widget_init: (2) path= %s\n", pathname);
		        /*
		         * This is a weird ass code needed for error injection
		         * purposes.
		         */
		        rc = device_master_set(hwgraph_connectpt_get(widgetv), hubv);
			
			klhwg_baseio_inventory_add(widgetv,cnode);
		}
		sprintf(name, "%d", widgetnum);
		DBG("io_xswitch_widget_init: FIXME hwgraph_edge_add %s xswitchv 0x%p, widgetv 0x%p\n", name, xswitchv, widgetv);
		rc = hwgraph_edge_add(xswitchv, widgetv, name);
		
		/*
		 * crosstalk switch code tracks which
		 * widget is attached to each link.
		 */
		xswitch_info_vhdl_set(xswitch_info, widgetnum, widgetv);
		
		/*
		 * Peek at the widget to get its crosstalk part and
		 * mfgr numbers, then present it to the generic xtalk
		 * bus provider to have its driver attach routine
		 * called (or not).
		 */
#ifdef XBRIDGE_REGS_SIM
		widget_id = 0x2d000049;
		DBG("io_xswitch_widget_init: XBRIDGE_REGS_SIM FIXME: id hardwired to widget_id\n");
#else
		widget_id = XWIDGET_ID_READ(nasid, widgetnum);
#endif /* XBRIDGE_REGS_SIM */
		hwid.part_num = XWIDGET_PART_NUM(widget_id);
		hwid.rev_num = XWIDGET_REV_NUM(widget_id);
		hwid.mfg_num = XWIDGET_MFG_NUM(widget_id);
		/* Store some inventory information about
		 * the xwidget in the hardware graph.
		 */
		xwidget_inventory_add(widgetv,board,hwid);
		
		(void)xwidget_register(&hwid, widgetv, widgetnum,
				       hubv, hub_widgetid,
				       aa);

#ifdef	SN0_USE_BTE
		bte_bpush_war(cnode, (void *)board);
#endif
	}

}


static void
io_init_xswitch_widgets(devfs_handle_t xswitchv, cnodeid_t cnode)
{
	xwidgetnum_t		widgetnum;
	async_attach_t          aa;

	aa = async_attach_new();
	
	DBG("io_init_xswitch_widgets: xswitchv 0x%p for cnode %d\n", xswitchv, cnode);

	for (widgetnum = HUB_WIDGET_ID_MIN; widgetnum <= HUB_WIDGET_ID_MAX; 
	     widgetnum++) {
		io_xswitch_widget_init(xswitchv,
				       cnodeid_to_vertex(cnode),
				       widgetnum, aa);
	}
	/* 
	 * Wait for parallel attach threads, if any, to complete.
	 */
	async_attach_waitall(aa);
	async_attach_free(aa);
}

/*
 * For each PCI bridge connected to the xswitch, add a link from the
 * board's klconfig info to the bridge's hwgraph vertex.  This lets
 * the FRU analyzer find the bridge without traversing the hardware
 * graph and risking hangs.
 */
static void
io_link_xswitch_widgets(devfs_handle_t xswitchv, cnodeid_t cnodeid)
{
	xwidgetnum_t		widgetnum;
	char 			pathname[128];
	devfs_handle_t		vhdl;
	nasid_t			nasid, peer_nasid;
	lboard_t		*board;



	/* And its connected hub's nasids */
	nasid = COMPACT_TO_NASID_NODEID(cnodeid);
	peer_nasid = NODEPDA(cnodeid)->xbow_peer;

	/* 
	 * Look for paths matching "<widgetnum>/pci" under xswitchv.
	 * For every widget, init. its lboard's hwgraph link.  If the
	 * board has a PCI bridge, point the link to it.
	 */
	for (widgetnum = HUB_WIDGET_ID_MIN; widgetnum <= HUB_WIDGET_ID_MAX;
		 widgetnum++) {
		sprintf(pathname, "%d", widgetnum);
		if (hwgraph_traverse(xswitchv, pathname, &vhdl) !=
		    GRAPH_SUCCESS)
			continue;

		board = find_lboard_module((lboard_t *)KL_CONFIG_INFO(nasid),
				NODEPDA(cnodeid)->module_id);
		if (board == NULL && peer_nasid != INVALID_NASID) {
			/*
			 * Try to find the board on our peer
			 */
			board = find_lboard_module(
				(lboard_t *)KL_CONFIG_INFO(peer_nasid),
				NODEPDA(cnodeid)->module_id);
		}
		if (board == NULL) {
#if defined(SUPPORT_PRINTING_V_FORMAT)
			printk(KERN_WARNING  "Could not find PROM info for vertex %v, "
				"FRU analyzer may fail",
				vhdl);
#else
			printk(KERN_WARNING  "Could not find PROM info for vertex 0x%p, "
				"FRU analyzer may fail",
				(void *)vhdl);
#endif
			return;
		}

		sprintf(pathname, "%d/"EDGE_LBL_PCI, widgetnum);
		if (hwgraph_traverse(xswitchv, pathname, &vhdl) == 
		    GRAPH_SUCCESS)
			board->brd_graph_link = vhdl;
		else
			board->brd_graph_link = GRAPH_VERTEX_NONE;
	}
}

/*
 * Initialize all I/O on the specified node.
 */
static void
io_init_node(cnodeid_t cnodeid)
{
	/*REFERENCED*/
	devfs_handle_t hubv, switchv, widgetv;
	struct xwidget_hwid_s hwid;
	hubinfo_t hubinfo;
	int is_xswitch;
	nodepda_t	*npdap;
	struct semaphore *peer_sema = 0;
	uint32_t	widget_partnum;
	nodepda_router_info_t *npda_rip;
	cpu_cookie_t	c = 0;
	extern int hubdev_docallouts(devfs_handle_t);

#ifdef LATER
	/* Try to execute on the node that we're initializing. */
	c = setnoderun(cnodeid);
#endif
	npdap = NODEPDA(cnodeid);

	/*
	 * Get the "top" vertex for this node's hardware
	 * graph; it will carry the per-hub hub-specific
	 * data, and act as the crosstalk provider master.
	 * It's canonical path is probably something of the
	 * form /hw/module/%M/slot/%d/node
	 */
	hubv = cnodeid_to_vertex(cnodeid);
	DBG("io_init_node: Initialize IO for cnode %d hubv(node) 0x%p npdap 0x%p\n", cnodeid, hubv, npdap);

	ASSERT(hubv != GRAPH_VERTEX_NONE);

	hubdev_docallouts(hubv);

	/*
	 * Set up the dependent routers if we have any.
	 */
	npda_rip = npdap->npda_rip_first;

	while(npda_rip) {
		/* If the router info has not been initialized
		 * then we need to do the router initialization
		 */
		if (!npda_rip->router_infop) {
			router_init(cnodeid,0,npda_rip);
		}
		npda_rip = npda_rip->router_next;
	}

	/*
	 * Read mfg info on this hub
	 */
#ifdef LATER
	printk("io_init_node: FIXME need to implement HUB_VERTEX_MFG_INFO\n");
	HUB_VERTEX_MFG_INFO(hubv);
#endif /* LATER */

	/* 
	 * If nothing connected to this hub's xtalk port, we're done.
	 */
	early_probe_for_widget(hubv, &hwid);
	if (hwid.part_num == XWIDGET_PART_NUM_NONE) {
#ifdef PROBE_TEST
		if ((cnodeid == 1) || (cnodeid == 2)) {
			int index;

			for (index = 0; index < 600; index++)
				DBG("Interfering with device probing!!!\n");
		}
#endif
		/* io_init_done takes cpu cookie as 2nd argument 
		 * to do a restorenoderun for the setnoderun done 
		 * at the start of this thread 
		 */
		
		DBG("**** io_init_node: Node's 0x%p hub widget has XWIDGET_PART_NUM_NONE ****\n", hubv);
		return;
		/* NOTREACHED */
	}

	/* 
	 * attach our hub_provider information to hubv,
	 * so we can use it as a crosstalk provider "master"
	 * vertex.
	 */
	xtalk_provider_register(hubv, &hub_provider);
	xtalk_provider_startup(hubv);

	/*
	 * Create a vertex to represent the crosstalk bus
	 * attached to this hub, and a vertex to be used
	 * as the connect point for whatever is out there
	 * on the other side of our crosstalk connection.
	 *
	 * Crosstalk Switch drivers "climb up" from their
	 * connection point to try and take over the switch
	 * point.
	 *
	 * Of course, the edges and verticies may already
	 * exist, in which case our net effect is just to
	 * associate the "xtalk_" driver with the connection
	 * point for the device.
	 */

	(void)hwgraph_path_add(hubv, EDGE_LBL_XTALK, &switchv);

	DBG("io_init_node: Created 'xtalk' entry to '../node/' xtalk vertex 0x%p\n", switchv);

	ASSERT(switchv != GRAPH_VERTEX_NONE);

	(void)hwgraph_edge_add(hubv, switchv, EDGE_LBL_IO);

	DBG("io_init_node: Created symlink 'io' from ../node/io to ../node/xtalk \n");

	/*
	 * We need to find the widget id and update the basew_id field
	 * accordingly. In particular, SN00 has direct connected bridge,
	 * and hence widget id is Not 0.
	 */

	widget_partnum = (((*(volatile int32_t *)(NODE_SWIN_BASE(COMPACT_TO_NASID_NODEID(cnodeid), 0) + WIDGET_ID))) & WIDGET_PART_NUM) >> WIDGET_PART_NUM_SHFT;

	if (widget_partnum == BRIDGE_WIDGET_PART_NUM ||
				widget_partnum == XBRIDGE_WIDGET_PART_NUM){
		npdap->basew_id = (((*(volatile int32_t *)(NODE_SWIN_BASE(COMPACT_TO_NASID_NODEID(cnodeid), 0) + BRIDGE_WID_CONTROL))) & WIDGET_WIDGET_ID);

		DBG("io_init_node: Found XBRIDGE widget_partnum= 0x%x\n", widget_partnum);

	} else if (widget_partnum == XBOW_WIDGET_PART_NUM ||
				widget_partnum == XXBOW_WIDGET_PART_NUM) {
		/* 
		 * Xbow control register does not have the widget ID field.
		 * So, hard code the widget ID to be zero.
		 */
		DBG("io_init_node: Found XBOW widget_partnum= 0x%x\n", widget_partnum);
		npdap->basew_id = 0;

	} else if (widget_partnum == XG_WIDGET_PART_NUM) {
		/* 
		 * OK, WTF do we do here if we have an XG direct connected to a HUB/Bedrock???
		 * So, hard code the widget ID to be zero?
		 */
		npdap->basew_id = 0;
		npdap->basew_id = (((*(volatile int32_t *)(NODE_SWIN_BASE(COMPACT_TO_NASID_NODEID(cnodeid), 0) + BRIDGE_WID_CONTROL))) & WIDGET_WIDGET_ID);
	} else { 
		npdap->basew_id = (((*(volatile int32_t *)(NODE_SWIN_BASE(COMPACT_TO_NASID_NODEID(cnodeid), 0) + BRIDGE_WID_CONTROL))) & WIDGET_WIDGET_ID);

		panic(" ****io_init_node: Unknown Widget Part Number 0x%x Widgt ID 0x%x attached to Hubv 0x%p ****\n", widget_partnum, npdap->basew_id, (void *)hubv);

		/*NOTREACHED*/
	}
	{
		char widname[10];
		sprintf(widname, "%x", npdap->basew_id);
		(void)hwgraph_path_add(switchv, widname, &widgetv);
		DBG("io_init_node: Created '%s' to '..node/xtalk/' vertex 0x%p\n", widname, widgetv);
		ASSERT(widgetv != GRAPH_VERTEX_NONE);
	}
	
	nodepda->basew_xc = widgetv;

	is_xswitch = xwidget_hwid_is_xswitch(&hwid);

	/* 
	 * Try to become the master of the widget.  If this is an xswitch
	 * with multiple hubs connected, only one will succeed.  Mastership
	 * of an xswitch is used only when touching registers on that xswitch.
	 * The slave xwidgets connected to the xswitch can be owned by various
	 * masters.
	 */
	if (device_master_set(widgetv, hubv) == 0) {

		/* Only one hub (thread) per Crosstalk device or switch makes
		 * it to here.
		 */

		/* 
		 * Initialize whatever xwidget is hanging off our hub.
		 * Whatever it is, it's accessible through widgetnum 0.
		 */
		hubinfo_get(hubv, &hubinfo);

		(void)xwidget_register(&hwid, widgetv, npdap->basew_id, hubv, hubinfo->h_widgetid, NULL);

		if (!is_xswitch) {
			/* io_init_done takes cpu cookie as 2nd argument 
			 * to do a restorenoderun for the setnoderun done 
			 * at the start of this thread 
			 */
			io_init_done(cnodeid,c);
			/* NOTREACHED */
		}

		/* 
		 * Special handling for Crosstalk Switches (e.g. xbow).
		 * We need to do things in roughly the following order:
		 *	1) Initialize xswitch hardware (done above)
		 *	2) Determine which hubs are available to be widget masters
		 *	3) Discover which links are active from the xswitch
		 *	4) Assign xwidgets hanging off the xswitch to hubs
		 *	5) Initialize all xwidgets on the xswitch
		 */

		volunteer_for_widgets(switchv, hubv);

		/* If there's someone else on this crossbow, recognize him */
		if (npdap->xbow_peer != INVALID_NASID) {
			nodepda_t *peer_npdap = NODEPDA(NASID_TO_COMPACT_NODEID(npdap->xbow_peer));
			peer_sema = &peer_npdap->xbow_sema;
			volunteer_for_widgets(switchv, peer_npdap->node_vertex);
		}

		assign_widgets_to_volunteers(switchv, hubv);

		/* Signal that we're done */
		if (peer_sema) {
			mutex_unlock(peer_sema);
		}
		
	}
	else {
	    /* Wait 'til master is done assigning widgets. */
	    mutex_lock(&npdap->xbow_sema);
	}

#ifdef PROBE_TEST
	if ((cnodeid == 1) || (cnodeid == 2)) {
		int index;

		for (index = 0; index < 500; index++)
			DBG("Interfering with device probing!!!\n");
	}
#endif
	/* Now both nodes can safely inititialize widgets */
	io_init_xswitch_widgets(switchv, cnodeid);
	io_link_xswitch_widgets(switchv, cnodeid);

	/* io_init_done takes cpu cookie as 2nd argument 
	 * to do a restorenoderun for the setnoderun done 
	 * at the start of this thread 
	 */
	io_init_done(cnodeid,c);

	DBG("\nio_init_node: DONE INITIALIZED ALL I/O FOR CNODEID %d\n\n", cnodeid);
}


#define IOINIT_STKSZ	(16 * 1024)

#define __DEVSTR1 	"/../.master/"
#define __DEVSTR2 	"/target/"
#define __DEVSTR3 	"/lun/0/disk/partition/"
#define	__DEVSTR4	"/../ef"

#if defined(CONFIG_IA64_SGI_SN1)
/*
 * Currently, we need to allow for 5 IBrick slots with 1 FC each
 * plus an internal 1394.
 *
 * ioconfig starts numbering SCSI's at NUM_BASE_IO_SCSI_CTLR.
 */
#define NUM_BASE_IO_SCSI_CTLR 6
#else
#define NUM_BASE_IO_SCSI_CTLR 6
#endif
int num_base_io_scsi_ctlr = 2; /* used by syssgi */
devfs_handle_t		base_io_scsi_ctlr_vhdl[NUM_BASE_IO_SCSI_CTLR];
static devfs_handle_t	baseio_enet_vhdl,baseio_console_vhdl;

/*
 * Put the logical controller number information in the 
 * scsi controller vertices for each scsi controller that
 * is in a "fixed position".
 */
static void
scsi_ctlr_nums_add(devfs_handle_t pci_vhdl)
{
	{
		int i;

		num_base_io_scsi_ctlr = NUM_BASE_IO_SCSI_CTLR;

		/* Initialize base_io_scsi_ctlr_vhdl array */
		for (i=0; i<NUM_BASE_IO_SCSI_CTLR; i++)
			base_io_scsi_ctlr_vhdl[i] = GRAPH_VERTEX_NONE;
	}
	{
	/*
	 * May want to consider changing the SN0 code, above, to work more like
	 * the way this works.
	 */
	devfs_handle_t base_ibrick_xbridge_vhdl;
	devfs_handle_t base_ibrick_xtalk_widget_vhdl;
	devfs_handle_t scsi_ctlr_vhdl;
	int i;
	graph_error_t rv;

	/*
	 * This is a table of "well-known" SCSI controllers and their well-known
	 * controller numbers.  The names in the table start from the base IBrick's
	 * Xbridge vertex, so the first component is the xtalk widget number.
	 */
	static struct {
		char	*base_ibrick_scsi_path;
		int	controller_number;
	} hardwired_scsi_controllers[] = {
		{"15/" EDGE_LBL_PCI "/1/" EDGE_LBL_SCSI_CTLR "/0", 0},
		{"15/" EDGE_LBL_PCI "/2/" EDGE_LBL_SCSI_CTLR "/0", 1},
		{"15/" EDGE_LBL_PCI "/3/" EDGE_LBL_SCSI_CTLR "/0", 2},
		{"14/" EDGE_LBL_PCI "/1/" EDGE_LBL_SCSI_CTLR "/0", 3},
		{"14/" EDGE_LBL_PCI "/2/" EDGE_LBL_SCSI_CTLR "/0", 4},
		{"15/" EDGE_LBL_PCI "/6/ohci/0/" EDGE_LBL_SCSI_CTLR "/0", 5},
		{NULL, -1} /* must be last */
	};

	base_ibrick_xtalk_widget_vhdl = hwgraph_connectpt_get(pci_vhdl);
	ASSERT_ALWAYS(base_ibrick_xtalk_widget_vhdl != GRAPH_VERTEX_NONE);

	base_ibrick_xbridge_vhdl = hwgraph_connectpt_get(base_ibrick_xtalk_widget_vhdl);
	ASSERT_ALWAYS(base_ibrick_xbridge_vhdl != GRAPH_VERTEX_NONE);
	hwgraph_vertex_unref(base_ibrick_xtalk_widget_vhdl);

	/*
	 * Iterate through the list of well-known SCSI controllers.
	 * For each controller found, set it's controller number according
	 * to the table.
	 */
	for (i=0; hardwired_scsi_controllers[i].base_ibrick_scsi_path != NULL; i++) {
		rv = hwgraph_path_lookup(base_ibrick_xbridge_vhdl,
			hardwired_scsi_controllers[i].base_ibrick_scsi_path, &scsi_ctlr_vhdl, NULL);

		if (rv != GRAPH_SUCCESS) /* No SCSI at this path */
			continue;

		ASSERT(hardwired_scsi_controllers[i].controller_number < NUM_BASE_IO_SCSI_CTLR);
		base_io_scsi_ctlr_vhdl[hardwired_scsi_controllers[i].controller_number] = scsi_ctlr_vhdl;
		device_controller_num_set(scsi_ctlr_vhdl, hardwired_scsi_controllers[i].controller_number);
		hwgraph_vertex_unref(scsi_ctlr_vhdl); /* (even though we're actually keeping a reference) */
	}

	hwgraph_vertex_unref(base_ibrick_xbridge_vhdl);
	}
}


#include <asm/sn/ioerror_handling.h>
devfs_handle_t		sys_critical_graph_root = GRAPH_VERTEX_NONE;

/* Define the system critical vertices and connect them through
 * a canonical parent-child relationships for easy traversal
 * during io error handling.
 */
static void
sys_critical_graph_init(void)
{
	devfs_handle_t		bridge_vhdl,master_node_vhdl;
	devfs_handle_t  		xbow_vhdl = GRAPH_VERTEX_NONE;
	extern devfs_handle_t	hwgraph_root;
	devfs_handle_t		pci_slot_conn;
	int			slot;
	devfs_handle_t		baseio_console_conn;

	DBG("sys_critical_graph_init: FIXME.\n");
	baseio_console_conn = hwgraph_connectpt_get(baseio_console_vhdl);

	if (baseio_console_conn == NULL) {
		return;
	}

	/* Get the vertex handle for the baseio bridge */
	bridge_vhdl = device_master_get(baseio_console_conn);

	/* Get the master node of the baseio card */
	master_node_vhdl = cnodeid_to_vertex(
				master_node_get(baseio_console_vhdl));
	
	/* Add the "root->node" part of the system critical graph */

	sys_critical_graph_vertex_add(hwgraph_root,master_node_vhdl);

	/* Check if we have a crossbow */
	if (hwgraph_traverse(master_node_vhdl,
			     EDGE_LBL_XTALK"/0",
			     &xbow_vhdl) == GRAPH_SUCCESS) {
		/* We have a crossbow.Add "node->xbow" part of the system 
		 * critical graph.
		 */
		sys_critical_graph_vertex_add(master_node_vhdl,xbow_vhdl);
		
		/* Add "xbow->baseio bridge" of the system critical graph */
		sys_critical_graph_vertex_add(xbow_vhdl,bridge_vhdl);

		hwgraph_vertex_unref(xbow_vhdl);
	} else 
		/* We donot have a crossbow. Add "node->baseio_bridge"
		 * part of the system critical graph.
		 */
		sys_critical_graph_vertex_add(master_node_vhdl,bridge_vhdl);

	/* Add all the populated PCI slot vertices to the system critical
	 * graph with the bridge vertex as the parent.
	 */
	for (slot = 0 ; slot < 8; slot++) {
		char	slot_edge[10];

		sprintf(slot_edge,"%d",slot);
		if (hwgraph_traverse(bridge_vhdl,slot_edge, &pci_slot_conn)
		    != GRAPH_SUCCESS)
			continue;
		sys_critical_graph_vertex_add(bridge_vhdl,pci_slot_conn);
		hwgraph_vertex_unref(pci_slot_conn);
	}

	hwgraph_vertex_unref(bridge_vhdl);

	/* Add the "ioc3 pci connection point  -> console ioc3" part 
	 * of the system critical graph
	 */

	if (hwgraph_traverse(baseio_console_vhdl,"..",&pci_slot_conn) ==
	    GRAPH_SUCCESS) {
		sys_critical_graph_vertex_add(pci_slot_conn, 
					      baseio_console_vhdl);
		hwgraph_vertex_unref(pci_slot_conn);
	}

	/* Add the "ethernet pci connection point  -> base ethernet" part of 
	 * the system  critical graph
	 */
	if (hwgraph_traverse(baseio_enet_vhdl,"..",&pci_slot_conn) ==
	    GRAPH_SUCCESS) {
		sys_critical_graph_vertex_add(pci_slot_conn, 
					      baseio_enet_vhdl);
		hwgraph_vertex_unref(pci_slot_conn);
	}

	/* Add the "scsi controller pci connection point  -> base scsi 
	 * controller" part of the system critical graph
	 */
	if (hwgraph_traverse(base_io_scsi_ctlr_vhdl[0],
			     "../..",&pci_slot_conn) == GRAPH_SUCCESS) {
		sys_critical_graph_vertex_add(pci_slot_conn, 
					      base_io_scsi_ctlr_vhdl[0]);
		hwgraph_vertex_unref(pci_slot_conn);
	}
	if (hwgraph_traverse(base_io_scsi_ctlr_vhdl[1],
			     "../..",&pci_slot_conn) == GRAPH_SUCCESS) {
		sys_critical_graph_vertex_add(pci_slot_conn, 
					      base_io_scsi_ctlr_vhdl[1]);
		hwgraph_vertex_unref(pci_slot_conn);
	}
	hwgraph_vertex_unref(baseio_console_conn);

}

static void
baseio_ctlr_num_set(void)
{
	char 			name[MAXDEVNAME];
	devfs_handle_t		console_vhdl, pci_vhdl, enet_vhdl;
	devfs_handle_t		ioc3_console_vhdl_get(void);


	DBG("baseio_ctlr_num_set; FIXME\n");
	console_vhdl = ioc3_console_vhdl_get();
	if (console_vhdl == GRAPH_VERTEX_NONE)
		return;
	/* Useful for setting up the system critical graph */
	baseio_console_vhdl = console_vhdl;

	vertex_to_name(console_vhdl,name,MAXDEVNAME);

	strcat(name,__DEVSTR1);
	pci_vhdl =  hwgraph_path_to_vertex(name);
	scsi_ctlr_nums_add(pci_vhdl);
	/* Unref the pci_vhdl due to the reference by hwgraph_path_to_vertex
	 */
	hwgraph_vertex_unref(pci_vhdl);

	vertex_to_name(console_vhdl, name, MAXDEVNAME);
	strcat(name, __DEVSTR4);
	enet_vhdl = hwgraph_path_to_vertex(name);

	/* Useful for setting up the system critical graph */
	baseio_enet_vhdl = enet_vhdl;

	device_controller_num_set(enet_vhdl, 0);
	/* Unref the enet_vhdl due to the reference by hwgraph_path_to_vertex
	 */
	hwgraph_vertex_unref(enet_vhdl);
}
/* #endif */

void
sn00_rrb_alloc(devfs_handle_t vhdl, int *vendor_list)
{
	/* REFERENCED */
	int rtn_val;

	/* 
	** sn00 population:		errb	orrb
	**	0- ql			3+?
	**	1- ql			        2
	**	2- ioc3 ethernet	2+?
	**	3- ioc3 secondary	        1
	**	4-                      0
	** 	5- PCI slot
	** 	6- PCI slot
	** 	7- PCI slot
	*/	
	
	/* The following code implements this heuristic for getting 
	 * maximum usage out of the rrbs
	 *
	 * constraints:
	 *  8 bit ql1 needs 1+1
	 *  ql0 or ql5,6,7 wants 1+2
	 *  ethernet wants 2 or more
	 *
	 * rules for even rrbs:
	 *  if nothing in slot 6 
	 *   4 rrbs to 0 and 2  (0xc8889999)
	 *  else 
         *   3 2 3 to slots 0 2 6  (0xc8899bbb)
	 *
         * rules for odd rrbs
	 *  if nothing in slot 5 or 7  (0xc8889999)
	 *   4 rrbs to 1 and 3
	 *  else if 1 thing in 5 or 7  (0xc8899aaa) or (0xc8899bbb)
         *   3 2 3 to slots 1 3 5|7
         *  else
         *   2 1 3 2 to slots 1 3 5 7 (note: if there's a ql card in 7 this
	 *           (0xc89aaabb)      may short what it wants therefore the
	 *			       rule should be to plug pci slots in order)
	 */


	if (vendor_list[6] != PCIIO_VENDOR_ID_NONE) {
		/* something in slot 6 */
		rtn_val = pcibr_alloc_all_rrbs(vhdl, 0, 3,1, 2,0, 0,0, 3,0);
	}
	else {
		rtn_val = pcibr_alloc_all_rrbs(vhdl, 0, 4,1, 4,0, 0,0, 0,0);
	}
	if (rtn_val)
		printk(KERN_WARNING  "sn00_rrb_alloc: pcibr_alloc_all_rrbs failed");

	if ((vendor_list[5] != PCIIO_VENDOR_ID_NONE) && 
	    (vendor_list[7] != PCIIO_VENDOR_ID_NONE)) {
		/* soemthing in slot 5 and 7 */
		rtn_val = pcibr_alloc_all_rrbs(vhdl, 1, 2,1, 1,0, 3,0, 2,0);
	}
	else if (vendor_list[5] != PCIIO_VENDOR_ID_NONE) {
		/* soemthing in slot 5 but not 7 */
		rtn_val = pcibr_alloc_all_rrbs(vhdl, 1, 3,1, 2,0, 3,0, 0,0);
	}
	else if (vendor_list[7] != PCIIO_VENDOR_ID_NONE) {
		/* soemthing in slot 7 but not 5 */
		rtn_val = pcibr_alloc_all_rrbs(vhdl, 1, 3,1, 2,0, 0,0, 3,0);
	}
	else {
		/* nothing in slot 5 or 7 */
		rtn_val = pcibr_alloc_all_rrbs(vhdl, 1, 4,1, 4,0, 0,0, 0,0);
	}
	if (rtn_val)
		printk(KERN_WARNING  "sn00_rrb_alloc: pcibr_alloc_all_rrbs failed");
}


/*
 * Initialize all I/O devices.  Starting closest to nodes, probe and
 * initialize outward.
 */
void
init_all_devices(void)
{
	/* Governor on init threads..bump up when safe 
	 * (beware many devfs races) 
	 */
#ifdef LATER
	int io_init_node_threads = 2;	
#endif
	cnodeid_t cnodeid, active;

#ifdef LINUX_KERNEL_THREADS
	sema_init(&io_init_sema, 0);
#endif

	active = 0;
	for (cnodeid = 0; cnodeid < numnodes; cnodeid++) {
#ifdef LINUX_KERNEL_THREADS
		char thread_name[16];
		extern int io_init_pri;

		/*
		 * Spawn a service thread for each node to initialize all
		 * I/O on that node.  Each thread attempts to bind itself 
		 * to the node whose I/O it's initializing.
		 */
		sprintf(thread_name, "IO_init[%d]", cnodeid);

		(void)sthread_create(thread_name, 0, IOINIT_STKSZ, 0,
			io_init_pri, KT_PS, (st_func_t *)io_init_node,
			(void *)(long)cnodeid, 0, 0, 0);
#else
                DBG("init_all_devices: Calling io_init_node() for cnode %d\n", cnodeid);
                io_init_node(cnodeid);

		DBG("init_all_devices: Done io_init_node() for cnode %d\n", cnodeid);

#endif /* LINUX_KERNEL_THREADS */

#ifdef LINUX_KERNEL_THREADS
		/* Limit how many nodes go at once, to not overload hwgraph */
		/* TBD: Should timeout */
		DBG("started thread for cnode %d\n", cnodeid);
		active++;
		if (io_init_node_threads && 
			active >= io_init_node_threads) {
			down(&io_init_sema);
			active--;
		}
#endif /* LINUX_KERNEL_THREADS */
	}

#ifdef LINUX_KERNEL_THREADS
	/* Wait until all IO_init threads are done */

	while (active > 0) {
#ifdef AA_DEBUG
	    DBG("waiting, %d still active\n", active);
#endif
	    down(&io_init_sema);
	    active--;
	}

#endif /* LINUX_KERNEL_THREADS */

	for (cnodeid = 0; cnodeid < numnodes; cnodeid++)
		/*
	 	 * Update information generated by IO init.
		 */
		update_node_information(cnodeid);

	baseio_ctlr_num_set();
	/* Setup the system critical graph (which is a subgraph of the
	 * main hwgraph). This information is useful during io error
	 * handling.
	 */
	sys_critical_graph_init();

#if HWG_PRINT
	hwgraph_print();
#endif

}

#define toint(x) ((int)(x) - (int)('0'))

void
devnamefromarcs(char *devnm)
{
	int 			val;
	char 			tmpnm[MAXDEVNAME];
	char 			*tmp1, *tmp2;
	
	val = strncmp(devnm, "dks", 3);
	if (val != 0) 
		return;
	tmp1 = devnm + 3;
	if (!isdigit(*tmp1))
		return;

	val = 0;
	while (isdigit(*tmp1)) {
		val = 10*val+toint(*tmp1);
		tmp1++;
	}

	if(*tmp1 != 'd')
		return;
	else
		tmp1++;

	if ((val < 0) || (val >= NUM_BASE_IO_SCSI_CTLR)) {
		int i;
		int viable_found = 0;

		DBG("Only controller numbers 0..%d  are supported for\n", NUM_BASE_IO_SCSI_CTLR-1);
		DBG("prom \"root\" variables of the form dksXdXsX.\n");
		DBG("To use another disk you must use the full hardware graph path\n\n");
		DBG("Possible controller numbers for use in 'dksXdXsX' on this system: ");
		for (i=0; i<NUM_BASE_IO_SCSI_CTLR; i++) {
			if (base_io_scsi_ctlr_vhdl[i] != GRAPH_VERTEX_NONE) {
				DBG("%d ", i);
				viable_found=1;
			}
		}
		if (viable_found)
			DBG("\n");
		else
			DBG("none found!\n");

#ifdef LATER
		if (kdebug)
			debug("ring");
#endif
		DELAY(15000000);
		//prom_reboot();
		panic("FIXME: devnamefromarcs: should call prom_reboot here.\n");
		/* NOTREACHED */
	}
		
	ASSERT(base_io_scsi_ctlr_vhdl[val] != GRAPH_VERTEX_NONE);
	vertex_to_name(base_io_scsi_ctlr_vhdl[val],
		       tmpnm,
		       MAXDEVNAME);
	tmp2 = 	tmpnm + strlen(tmpnm);
	strcpy(tmp2, __DEVSTR2);
	tmp2 += strlen(__DEVSTR2);
	while (*tmp1 != 's') {
		if((*tmp2++ = *tmp1++) == '\0')
			return;
	}	
	tmp1++;
	strcpy(tmp2, __DEVSTR3);
	tmp2 += strlen(__DEVSTR3);
	while ( (*tmp2++ = *tmp1++) )
		;
	tmp2--;
	*tmp2++ = '/';
	strcpy(tmp2, EDGE_LBL_BLOCK);
	strcpy(devnm,tmpnm);
}

static
struct io_brick_map_s io_brick_tab[] = {

/* Ibrick widget number to PCI bus number map */
  {
        'I',                                    /* Ibrick type    */ 
    /*  PCI Bus #                                  Widget #       */
     {  0, 0, 0, 0, 0, 0, 0, 0,                 /* 0x0 - 0x7      */
        0,                                      /* 0x8            */
        0,                                      /* 0x9            */
        0, 0,                                   /* 0xa - 0xb      */
        0,                                      /* 0xc            */
        0,                                      /* 0xd            */
        2,                                      /* 0xe            */
        1                                       /* 0xf            */
     }
  },

/* Pbrick widget number to PCI bus number map */
  {
        'P',                                    /* Pbrick type    */ 
    /*  PCI Bus #                                  Widget #       */
     {  0, 0, 0, 0, 0, 0, 0, 0,                 /* 0x0 - 0x7      */
        2,                                      /* 0x8            */
        1,                                      /* 0x9            */
        0, 0,                                   /* 0xa - 0xb      */
        5,                                      /* 0xc            */
        6,                                      /* 0xd            */
        4,                                      /* 0xe            */
        3                                       /* 0xf            */
     }
  },

/* Xbrick widget to XIO slot map */
  {
        'X',                                    /* Xbrick type    */ 
    /*  XIO Slot #                                 Widget #       */
     {  0, 0, 0, 0, 0, 0, 0, 0,                 /* 0x0 - 0x7      */
        1,                                      /* 0x8            */
        2,                                      /* 0x9            */
        0, 0,                                   /* 0xa - 0xb      */
        3,                                      /* 0xc            */
        4,                                      /* 0xd            */
        0,                                      /* 0xe            */
        0                                       /* 0xf            */
     }
  }
};

/*
 * Use the brick's type to map a widget number to a meaningful int
 */
int
io_brick_map_widget(char brick_type, int widget_num)
{
        int num_bricks, i;

        /* Calculate number of bricks in table */
        num_bricks = sizeof(io_brick_tab)/sizeof(io_brick_tab[0]);

        /* Look for brick prefix in table */
        for (i = 0; i < num_bricks; i++) {
               if (brick_type == io_brick_tab[i].ibm_type)
                       return(io_brick_tab[i].ibm_map_wid[widget_num]);
        }

        return 0;

}

/*
 * Use the device's vertex to map the device's widget to a meaningful int
 */
int
io_path_map_widget(devfs_handle_t vertex)
{
        char hw_path_name[MAXDEVNAME];
        char *wp, *bp, *sp = NULL;
        int  widget_num;
	long atoi(char *);
	int hwgraph_vertex_name_get(devfs_handle_t vhdl, char *buf, uint buflen);


        /* Get the full path name of the vertex */
        if (GRAPH_SUCCESS != hwgraph_vertex_name_get(vertex, hw_path_name,
                                                     MAXDEVNAME))
                return 0;

        /* Find the widget number in the path name */
        wp = strstr(hw_path_name, "/"EDGE_LBL_XTALK"/");
        if (wp == NULL)
                return 0;
        widget_num = atoi(wp+7);
        if (widget_num < XBOW_PORT_8 || widget_num > XBOW_PORT_F)
                return 0;

        /* Find "brick" in the path name */
        bp = strstr(hw_path_name, "brick");
        if (bp == NULL)
                return 0;

        /* Find preceding slash */
        sp = bp;
        while (sp > hw_path_name) {
                sp--;
                if (*sp == '/')
                        break;
        }

        /* Invalid if no preceding slash */
        if (!sp)
                return 0;

        /* Bump slash pointer to "brick" prefix */
        sp++;
        /*
         * Verify "brick" prefix length;  valid exaples:
         * 'I' from "/Ibrick"
         * 'P' from "/Pbrick"
         * 'X' from "/Xbrick"
         */
         if ((bp - sp) != 1)
                return 0;

        return (io_brick_map_widget(*sp, widget_num));

}
