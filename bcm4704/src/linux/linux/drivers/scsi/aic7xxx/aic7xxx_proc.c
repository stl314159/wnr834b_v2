/*
 * Copyright (c) 2000-2001 Adaptec Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce at minimum a disclaimer
 *    substantially similar to the "NO WARRANTY" disclaimer below
 *    ("Disclaimer") and any redistribution must be conditioned upon
 *    including a substantially similar Disclaimer requirement for further
 *    binary redistribution.
 * 3. Neither the names of the above-listed copyright holders nor the names
 *    of any contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * NO WARRANTY
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGES.
 *
 * String handling code courtesy of Gerard Roudier's <groudier@club-internet.fr>
 * sym driver.
 *
 * $Id: aic7xxx_proc.c,v 1.1.1.1 2010/03/05 07:31:34 reynolds Exp $
 */
#include "aic7xxx_osm.h"
#include "aic7xxx_inline.h"
#include "aic7xxx_93cx6.h"

static void	copy_mem_info(struct info_str *info, char *data, int len);
static int	copy_info(struct info_str *info, char *fmt, ...);
static u_int	scsi_calc_syncsrate(u_int period_factor);
static void	ahc_dump_target_state(struct ahc_softc *ahc,
				      struct info_str *info,
				      u_int our_id, char channel,
				      u_int target_id, u_int target_offset);
static void	ahc_dump_device_state(struct info_str *info,
				      struct ahc_linux_device *dev);
static int	ahc_proc_write_seeprom(struct ahc_softc *ahc,
				       char *buffer, int length);
				       

int
ahc_acquire_seeprom(struct ahc_softc *ahc, struct seeprom_descriptor *sd)
{
	int wait;

	if ((ahc->features & AHC_SPIOCAP) != 0
	 && (ahc_inb(ahc, SPIOCAP) & SEEPROM) == 0)
		return (0);

	/*
	 * Request access of the memory port.  When access is
	 * granted, SEERDY will go high.  We use a 1 second
	 * timeout which should be near 1 second more than
	 * is needed.  Reason: after the chip reset, there
	 * should be no contention.
	 */
	SEEPROM_OUTB(sd, sd->sd_MS);
	wait = 1000;  /* 1 second timeout in msec */
	while (--wait && ((SEEPROM_STATUS_INB(sd) & sd->sd_RDY) == 0)) {
		ahc_delay(1000);  /* delay 1 msec */
	}
	if ((SEEPROM_STATUS_INB(sd) & sd->sd_RDY) == 0) {
		SEEPROM_OUTB(sd, 0); 
		return (0);
	}
	return(1);
}


void
ahc_release_seeprom(struct seeprom_descriptor *sd)
{
	/* Release access to the memory port and the serial EEPROM. */
	SEEPROM_OUTB(sd, 0);
}


static void
copy_mem_info(struct info_str *info, char *data, int len)
{
	if (info->pos + len > info->offset + info->length)
		len = info->offset + info->length - info->pos;

	if (info->pos + len < info->offset) {
		info->pos += len;
		return;
	}

	if (info->pos < info->offset) {
		off_t partial;

		partial = info->offset - info->pos;
		data += partial;
		info->pos += partial;
		len  -= partial;
	}

	if (len > 0) {
		memcpy(info->buffer, data, len);
		info->pos += len;
		info->buffer += len;
	}
}

static int
copy_info(struct info_str *info, char *fmt, ...)
{
	va_list args;
	char buf[256];
	int len;

	va_start(args, fmt);
	len = vsprintf(buf, fmt, args);
	va_end(args);

	copy_mem_info(info, buf, len);
	return (len);
}

/*
 * Table of syncrates that don't follow the "divisible by 4"
 * rule. This table will be expanded in future SCSI specs.
 */
static struct {
	u_int period_factor;
	u_int period;	/* in 10ths of ns */
} scsi_syncrates[] = {
	{ 0x09, 125 },	/* FAST-80 */
	{ 0x0a, 250 },	/* FAST-40 40MHz */
	{ 0x0b, 303 },	/* FAST-40 33MHz */
	{ 0x0c, 500 }	/* FAST-20 */
};
 
/*
 * Return the frequency in kHz corresponding to the given
 * sync period factor.
 */
static u_int
scsi_calc_syncsrate(u_int period_factor)
{
	int i; 
	int num_syncrates;
 
	num_syncrates = sizeof(scsi_syncrates) / sizeof(scsi_syncrates[0]);
	/* See if the period is in the "exception" table */
	for (i = 0; i < num_syncrates; i++) {

		if (period_factor == scsi_syncrates[i].period_factor) {
       			/* Period in kHz */
			return (10000000 / scsi_syncrates[i].period);
		}
	}

	/*
	 * Wasn't in the table, so use the standard
	 * 4 times conversion.
	 */
	return (10000000 / (period_factor * 4 * 10));
}

void
ahc_format_transinfo(struct info_str *info, struct ahc_transinfo *tinfo)
{
	u_int speed;
	u_int freq;
	u_int mb;

        speed = 3300;
        freq = 0;
	if (tinfo->offset != 0) {
		freq = scsi_calc_syncsrate(tinfo->period);
		speed = freq;
	}
	speed *= (0x01 << tinfo->width);
        mb = speed / 1000;
        if (mb > 0)
		copy_info(info, "%d.%03dMB/s transfers", mb, speed % 1000);
        else
		copy_info(info, "%dKB/s transfers", speed);

	if (freq != 0) {
		copy_info(info, " (%d.%03dMHz%s, offset %d",
			 freq / 1000, freq % 1000,
			 (tinfo->ppr_options & MSG_EXT_PPR_DT_REQ) != 0
			 ? " DT" : "", tinfo->offset);
	}

	if (tinfo->width > 0) {
		if (freq != 0) {
			copy_info(info, ", ");
		} else {
			copy_info(info, " (");
		}
		copy_info(info, "%dbit)", 8 * (0x01 << tinfo->width));
	} else if (freq != 0) {
		copy_info(info, ")");
	}
	copy_info(info, "\n");
}

static void
ahc_dump_target_state(struct ahc_softc *ahc, struct info_str *info,
		      u_int our_id, char channel, u_int target_id,
		      u_int target_offset)
{
	struct	ahc_linux_target *targ;
	struct	ahc_initiator_tinfo *tinfo;
	struct	ahc_tmode_tstate *tstate;
	int	lun;

	tinfo = ahc_fetch_transinfo(ahc, channel, our_id,
				    target_id, &tstate);
	copy_info(info, "Channel %c Target %d Negotiation Settings\n",
		  channel, target_id);
	copy_info(info, "\tUser: ");
	ahc_format_transinfo(info, &tinfo->user);
	targ = ahc->platform_data->targets[target_offset];
	if (targ == NULL)
		return;

	copy_info(info, "\tGoal: ");
	ahc_format_transinfo(info, &tinfo->goal);
	copy_info(info, "\tCurr: ");
	ahc_format_transinfo(info, &tinfo->curr);

	for (lun = 0; lun < AHC_NUM_LUNS; lun++) {
		struct ahc_linux_device *dev;

		dev = targ->devices[lun];

		if (dev == NULL)
			continue;

		ahc_dump_device_state(info, dev);
	}
}

static void
ahc_dump_device_state(struct info_str *info, struct ahc_linux_device *dev)
{
	copy_info(info, "\tChannel %c Target %d Lun %d Settings\n",
		  dev->target->channel + 'A', dev->target->target, dev->lun);

	copy_info(info, "\t\tCommands Queued %ld\n", dev->commands_issued);
	copy_info(info, "\t\tCommands Active %d\n", dev->active);
	copy_info(info, "\t\tCommand Openings %d\n", dev->openings);
	copy_info(info, "\t\tMax Tagged Openings %d\n", dev->maxtags);
	copy_info(info, "\t\tDevice Queue Frozen Count %d\n", dev->qfrozen);
}

static int
ahc_proc_write_seeprom(struct ahc_softc *ahc, char *buffer, int length)
{
	struct seeprom_descriptor sd;
	int have_seeprom;
	u_long s;
	int paused;
	int written;

	/* Default to failure. */
	written = -EINVAL;
	ahc_lock(ahc, &s);
	paused = ahc_is_paused(ahc);
	if (!paused)
		ahc_pause(ahc);

	if (length != sizeof(struct seeprom_config)) {
		printf("ahc_proc_write_seeprom: incorrect buffer size\n");
		goto done;
	}

	have_seeprom = ahc_verify_cksum((struct seeprom_config*)buffer);
	if (have_seeprom == 0) {
		printf("ahc_proc_write_seeprom: cksum verification failed\n");
		goto done;
	}

	sd.sd_ahc = ahc;
	if ((ahc->chip & AHC_VL) != 0) {
		sd.sd_control_offset = SEECTL_2840;
		sd.sd_status_offset = STATUS_2840;
		sd.sd_dataout_offset = STATUS_2840;		
		sd.sd_chip = C46;
		sd.sd_MS = 0;
		sd.sd_RDY = EEPROM_TF;
		sd.sd_CS = CS_2840;
		sd.sd_CK = CK_2840;
		sd.sd_DO = DO_2840;
		sd.sd_DI = DI_2840;
		have_seeprom = TRUE;
	} else {
		sd.sd_control_offset = SEECTL;
		sd.sd_status_offset = SEECTL;
		sd.sd_dataout_offset = SEECTL;
		if (ahc->flags & AHC_LARGE_SEEPROM)
			sd.sd_chip = C56_66;
		else
			sd.sd_chip = C46;
		sd.sd_MS = SEEMS;
		sd.sd_RDY = SEERDY;
		sd.sd_CS = SEECS;
		sd.sd_CK = SEECK;
		sd.sd_DO = SEEDO;
		sd.sd_DI = SEEDI;
		have_seeprom = ahc_acquire_seeprom(ahc, &sd);
	}

	if (!have_seeprom) {
		printf("ahc_proc_write_seeprom: No Serial EEPROM\n");
		goto done;
	} else {
		u_int start_addr;

		if (ahc->seep_config == NULL) {
			ahc->seep_config = malloc(sizeof(*ahc->seep_config),
						  M_DEVBUF, M_NOWAIT);
			if (ahc->seep_config == NULL) {
				printf("aic7xxx: Unable to allocate serial "
				       "eeprom buffer.  Write failing\n");
				goto done;
			}
		}
		printf("aic7xxx: Writing Serial EEPROM\n");
		start_addr = 32 * (ahc->channel - 'A');
		ahc_write_seeprom(&sd, (u_int16_t *)buffer, start_addr,
				  sizeof(struct seeprom_config)/2);
		ahc_read_seeprom(&sd, (uint16_t *)ahc->seep_config,
				 start_addr, sizeof(struct seeprom_config)/2);
		if ((ahc->chip & AHC_VL) == 0)
			ahc_release_seeprom(&sd);
		written = length;
	}

done:
	if (!paused)
		ahc_unpause(ahc);
	ahc_unlock(ahc, &s);
	return (written);
}

/*
 * Return information to handle /proc support for the driver.
 */
int
ahc_linux_proc_info(char *buffer, char **start, off_t offset,
		  int length, int hostno, int inout)
{
	struct	ahc_softc *ahc;
	struct	info_str info;
	char	ahc_info[256];
	u_long	s;
	u_int	max_targ;
	u_int	i;
	int	retval;

	retval = -EINVAL;
	ahc_list_lock(&s);
	TAILQ_FOREACH(ahc, &ahc_tailq, links) {
		if (ahc->platform_data->host->host_no == hostno)
			break;
	}

	if (ahc == NULL)
		goto done;

	 /* Has data been written to the file? */ 
	if (inout == TRUE) {
		retval = ahc_proc_write_seeprom(ahc, buffer, length);
		goto done;
	}

	if (start)
		*start = buffer;

	info.buffer	= buffer;
	info.length	= length;
	info.offset	= offset;
	info.pos	= 0;

	copy_info(&info, "Adaptec AIC7xxx driver version: %s\n",
		  AIC7XXX_DRIVER_VERSION);
	ahc_controller_info(ahc, ahc_info);
	copy_info(&info, "%s\n\n", ahc_info);

	if (ahc->seep_config == NULL)
		copy_info(&info, "No Serial EEPROM\n");
	else {
		copy_info(&info, "Serial EEPROM:\n");
		for (i = 0; i < sizeof(*ahc->seep_config)/2; i++) {
			if (((i % 8) == 0) && (i != 0)) {
				copy_info(&info, "\n");
			}
			copy_info(&info, "0x%.4x ",
				  ((uint16_t*)ahc->seep_config)[i]);
		}
		copy_info(&info, "\n");
	}
	copy_info(&info, "\n");

	max_targ = 15;
	if ((ahc->features & (AHC_WIDE|AHC_TWIN)) == 0)
		max_targ = 7;

	for (i = 0; i <= max_targ; i++) {
		u_int our_id;
		u_int target_id;
		char channel;

		channel = 'A';
		our_id = ahc->our_id;
		target_id = i;
		if (i > 7 && (ahc->features & AHC_TWIN) != 0) {
			channel = 'B';
			our_id = ahc->our_id_b;
			target_id = i % 8;
		}

		ahc_dump_target_state(ahc, &info, our_id,
				      channel, target_id, i);
	}
	retval = info.pos > info.offset ? info.pos - info.offset : 0;
done:
	ahc_list_unlock(&s);
	return (retval);
}
