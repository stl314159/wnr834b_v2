/* $Id: message.c,v 1.1.1.1 2010/03/05 07:31:29 reynolds Exp $
 *
 * functions for sending and receiving control messages
 *
 * Copyright (C) 1996  SpellCaster Telecommunications Inc.
 *
 * This software may be used and distributed according to the terms
 * of the GNU General Public License, incorporated herein by reference.
 *
 * For more information, please contact gpl-info@spellcast.com or write:
 *
 *     SpellCaster Telecommunications Inc.
 *     5621 Finch Avenue East, Unit #3
 *     Scarborough, Ontario  Canada
 *     M1B 2T9
 *     +1 (416) 297-8565
 *     +1 (416) 297-6433 Facsimile
 */

#define __NO_VERSION__
#include "includes.h"
#include "hardware.h"
#include "message.h"
#include "card.h"

extern board *adapter[];
extern unsigned int cinst;

/*
 * Obligatory function prototypes
 */
extern int indicate_status(int,ulong,char*);
extern int scm_command(isdn_ctrl *);
extern void *memcpy_fromshmem(int, void *, const void *, size_t);


/*
 * receive a message from the board
 */
int receivemessage(int card, RspMessage *rspmsg) 
{
	DualPortMemory *dpm;
	unsigned long flags;

	if (!IS_VALID_CARD(card)) {
		pr_debug("Invalid param: %d is not a valid card id\n", card);
		return -EINVAL;
	}
	
	pr_debug("%s: Entered receivemessage\n",adapter[card]->devicename);

	/*
	 * See if there are messages waiting
	 */
	if (inb(adapter[card]->ioport[FIFO_STATUS]) & RF_HAS_DATA) {
		/*
		 * Map in the DPM to the base page and copy the message
		 */
		save_flags(flags);
		cli();
		outb((adapter[card]->shmem_magic >> 14) | 0x80,
			adapter[card]->ioport[adapter[card]->shmem_pgport]); 
		dpm = (DualPortMemory *) adapter[card]->rambase;
		memcpy_fromio(rspmsg, &(dpm->rsp_queue[dpm->rsp_tail]), 
			MSG_LEN);
		dpm->rsp_tail = (dpm->rsp_tail+1) % MAX_MESSAGES;
		inb(adapter[card]->ioport[FIFO_READ]);
		restore_flags(flags);
		
		/*
		 * Tell the board that the message is received
		 */
		pr_debug("%s: Received Message seq:%d pid:%d time:%d cmd:%d "
				"cnt:%d (type,class,code):(%d,%d,%d) "
				"link:%d stat:0x%x\n",
					adapter[card]->devicename,
					rspmsg->sequence_no,
					rspmsg->process_id,
					rspmsg->time_stamp,
					rspmsg->cmd_sequence_no,
					rspmsg->msg_byte_cnt,
					rspmsg->type,
					rspmsg->class,
					rspmsg->code,
					rspmsg->phy_link_no, 
					rspmsg->rsp_status);

		return 0;
	}
	return -ENOMSG;
}
	
/*
 * send a message to the board
 */
int sendmessage(int card,
		unsigned int procid,
		unsigned int type, 
		unsigned int class, 
		unsigned int code,
		unsigned int link, 
		unsigned int data_len, 
		unsigned int *data) 
{
	DualPortMemory *dpm;
	ReqMessage sndmsg;
	unsigned long flags;

	if (!IS_VALID_CARD(card)) {
		pr_debug("Invalid param: %d is not a valid card id\n", card);
		return -EINVAL;
	}

	/*
	 * Make sure we only send CEPID messages when the engine is up
	 * and CMPID messages when it is down
	 */
	if(adapter[card]->EngineUp && procid == CMPID) {
		pr_debug("%s: Attempt to send CM message with engine up\n",
			adapter[card]->devicename);
		return -ESRCH;
	}

	if(!adapter[card]->EngineUp && procid == CEPID) {
		pr_debug("%s: Attempt to send CE message with engine down\n",
			adapter[card]->devicename);
		return -ESRCH;
	}

	memset(&sndmsg, 0, MSG_LEN);
	sndmsg.msg_byte_cnt = 4;
	sndmsg.type = type;
	sndmsg.class = class;
	sndmsg.code = code;
	sndmsg.phy_link_no = link;

	if (data_len > 0) {
		if (data_len > MSG_DATA_LEN)
			data_len = MSG_DATA_LEN;
		memcpy(&(sndmsg.msg_data), data, data_len);
		sndmsg.msg_byte_cnt = data_len + 8;
	}

	sndmsg.process_id = procid;
	sndmsg.sequence_no = adapter[card]->seq_no++ % 256;

	/*
	 * wait for an empty slot in the queue
	 */
	while (!(inb(adapter[card]->ioport[FIFO_STATUS]) & WF_NOT_FULL))
		udelay(1);

	/*
	 * Disable interrupts and map in shared memory
	 */
	save_flags(flags);
	cli();
	outb((adapter[card]->shmem_magic >> 14) | 0x80,
		adapter[card]->ioport[adapter[card]->shmem_pgport]); 
	dpm = (DualPortMemory *) adapter[card]->rambase;	/* Fix me */
	memcpy_toio(&(dpm->req_queue[dpm->req_head]),&sndmsg,MSG_LEN);
	dpm->req_head = (dpm->req_head+1) % MAX_MESSAGES;
	outb(sndmsg.sequence_no, adapter[card]->ioport[FIFO_WRITE]);
	restore_flags(flags);
		
	pr_debug("%s: Sent Message seq:%d pid:%d time:%d "
			"cnt:%d (type,class,code):(%d,%d,%d) "
			"link:%d\n ",
				adapter[card]->devicename,
				sndmsg.sequence_no,
				sndmsg.process_id,
				sndmsg.time_stamp,
				sndmsg.msg_byte_cnt,
				sndmsg.type,
				sndmsg.class,
				sndmsg.code,
				sndmsg.phy_link_no); 
		
	return 0;
}

int send_and_receive(int card,
		unsigned int procid, 
		unsigned char type,
		unsigned char class, 
		unsigned char code,
		unsigned char link,
	 	unsigned char data_len, 
		unsigned char *data, 
		RspMessage *mesgdata,
		int timeout) 
{
	int retval;
	int tries;

	if (!IS_VALID_CARD(card)) {
		pr_debug("Invalid param: %d is not a valid card id\n", card);
		return -EINVAL;
	}

	adapter[card]->want_async_messages = 1;
	retval = sendmessage(card, procid, type, class, code, link, 
			data_len, (unsigned int *) data);
  
	if (retval) {
		pr_debug("%s: SendMessage failed in SAR\n",
			adapter[card]->devicename);
		adapter[card]->want_async_messages = 0;
		return -EIO;
	}

	tries = 0;
	/* wait for the response */
	while (tries < timeout) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(1);
		
		pr_debug("SAR waiting..\n");

		/*
		 * See if we got our message back
		 */
		if ((adapter[card]->async_msg.type == type) &&
		    (adapter[card]->async_msg.class == class) &&
		    (adapter[card]->async_msg.code == code) &&
		    (adapter[card]->async_msg.phy_link_no == link)) {

			/*
			 * Got it!
			 */
			pr_debug("%s: Got ASYNC message\n",
				adapter[card]->devicename);
			memcpy(mesgdata, &(adapter[card]->async_msg), 
				sizeof(RspMessage));
			adapter[card]->want_async_messages = 0;
			return 0;
		}

   		tries++;
	}

	pr_debug("%s: SAR message timeout\n", adapter[card]->devicename);
	adapter[card]->want_async_messages = 0;
	return -ETIME;
}
