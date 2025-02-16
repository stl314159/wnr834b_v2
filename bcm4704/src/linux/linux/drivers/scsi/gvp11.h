#ifndef GVP11_H

/* $Id: gvp11.h,v 1.1.1.1 2010/03/05 07:31:32 reynolds Exp $
 *
 * Header file for the GVP Series II SCSI controller for Linux
 *
 * Written and (C) 1993, Ralf Baechle, see gvp11.c for more info
 * based on a2091.h (C) 1993 by Hamish Macdonald
 *
 */

#include <linux/types.h>

int gvp11_detect(Scsi_Host_Template *);
int gvp11_release(struct Scsi_Host *);
const char *wd33c93_info(void);
int wd33c93_queuecommand(Scsi_Cmnd *, void (*done)(Scsi_Cmnd *));
int wd33c93_abort(Scsi_Cmnd *);
int wd33c93_reset(Scsi_Cmnd *, unsigned int);

#ifndef NULL
#define NULL 0
#endif

#ifndef CMD_PER_LUN
#define CMD_PER_LUN 2
#endif

#ifndef CAN_QUEUE
#define CAN_QUEUE 16
#endif

#ifdef HOSTS_C

#define GVP11_SCSI {  proc_name:	   "GVP11", \
		      name:                "GVP Series II SCSI", \
		      detect:              gvp11_detect,    \
		      release:             gvp11_release,   \
		      queuecommand:        wd33c93_queuecommand, \
		      abort:               wd33c93_abort,   \
		      reset:               wd33c93_reset,   \
		      can_queue:           CAN_QUEUE,       \
		      this_id:             7,               \
		      sg_tablesize:        SG_ALL,          \
		      cmd_per_lun:	   CMD_PER_LUN,     \
		      use_clustering:      DISABLE_CLUSTERING }
#else

/*
 * if the transfer address ANDed with this results in a non-zero
 * result, then we can't use DMA.
 */
#define GVP11_XFER_MASK  (0xff000001)

typedef struct {
             unsigned char      pad1[64];
    volatile unsigned short     CNTR;
             unsigned char      pad2[31];
    volatile unsigned char      SASR;
             unsigned char      pad3;
    volatile unsigned char      SCMD;
             unsigned char      pad4[4];
    volatile unsigned short     BANK;
             unsigned char      pad5[6];
    volatile unsigned long      ACR;
    volatile unsigned short     secret1; /* store 0 here */
    volatile unsigned short     ST_DMA;
    volatile unsigned short     SP_DMA;
    volatile unsigned short     secret2; /* store 1 here */
    volatile unsigned short     secret3; /* store 15 here */
} gvp11_scsiregs;

/* bits in CNTR */
#define GVP11_DMAC_BUSY		(1<<0)
#define GVP11_DMAC_INT_PENDING	(1<<1)
#define GVP11_DMAC_INT_ENABLE	(1<<3)
#define GVP11_DMAC_DIR_WRITE	(1<<4)

#endif /* else def HOSTS_C */

#endif /* GVP11_H */
