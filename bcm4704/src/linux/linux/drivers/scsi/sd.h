/*
 *    sd.h Copyright (C) 1992 Drew Eckhardt 
 *      SCSI disk driver header file by
 *              Drew Eckhardt 
 *
 *      <drew@colorado.edu>
 *
 *       Modified by Eric Youngdale eric@andante.org to
 *       add scatter-gather, multiple outstanding request, and other
 *       enhancements.
 */
#ifndef _SD_H
#define _SD_H
/*
   $Header: /home/vault/cvs/wnr834b_v2/bcm4704/src/linux/linux/drivers/scsi/sd.h,v 1.1.1.1 2010/03/05 07:31:32 reynolds Exp $
 */

#ifndef _SCSI_H
#include "scsi.h"
#endif

#ifndef _GENDISK_H
#include <linux/genhd.h>
#endif

typedef struct scsi_disk {
	unsigned capacity;	/* size in blocks */
	Scsi_Device *device;
	unsigned char ready;	/* flag ready for FLOPTICAL */
	unsigned char write_prot;	/* flag write_protect for rmvable dev */
	unsigned char sector_bit_size;	/* sector_size = 2 to the  bit size power */
	unsigned char sector_bit_shift;		/* power of 2 sectors per FS block */
	unsigned has_part_table:1;	/* has partition table */
} Scsi_Disk;

extern int revalidate_scsidisk(kdev_t dev, int maxusage);

/*
 * Used by pmac to find the device associated with a target.
 */
extern kdev_t sd_find_target(void *host, int tgt);

#define N_SD_MAJORS	8

#define SD_MAJOR_MASK	(N_SD_MAJORS - 1)

#endif

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-indent-level: 4
 * c-brace-imaginary-offset: 0
 * c-brace-offset: -4
 * c-argdecl-indent: 4
 * c-label-offset: -4
 * c-continued-statement-offset: 4
 * c-continued-brace-offset: 0
 * indent-tabs-mode: nil
 * tab-width: 8
 * End:
 */
