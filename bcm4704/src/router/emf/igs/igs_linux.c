/*
 * IGMP Snooping layer linux specific code
 *
 * Copyright 2007, Broadcom Corporation
 * All Rights Reserved.                
 *                                     
 * This is UNPUBLISHED PROPRIETARY SOURCE CODE of Broadcom Corporation;   
 * the contents of this file may not be disclosed to third parties, copied
 * or duplicated in any form, in whole or in part, without the prior      
 * written permission of Broadcom Corporation.                            
 *
 * $Id: igs_linux.c,v 1.1.1.1 2010/03/05 07:31:37 reynolds Exp $
 */
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/proc_fs.h>
#include <linux/netlink.h>
#include <net/sock.h>
#include <linux/if.h>
#include <proto/ethernet.h>
#include <bcmnvram.h>
#include <bcmutils.h>
#include <osl.h>
#include "osl_linux.h"
#include "igs_cfg.h"
#include "igsc_export.h"
#include "igs_linux.h"

static igs_struct_t igs;

/*
 * Description: This function is called by IGS Common code when it wants
 *              to send a packet on to all the LAN ports. It allocates
 *              the native OS packet buffer, adds mac header and forwards
 *              a copy of frame on to LAN ports.
 *
 * Input:       igs_info - IGS instance information.
 *              ip       - Pointer to the buffer containing the frame to
 *                         send.
 *              length   - Length of the buffer.
 *              mgrp_ip  - Multicast destination address.
 *
 * Return:      SUCCESS or FAILURE
 */
int32
igs_broadcast(igs_info_t *igs_info, uint8 *ip, uint32 length, uint32 mgrp_ip)
{
	struct sk_buff *skb;
	struct net_device *br_dev;
	struct ether_header *eh;

	br_dev = igs_info->br_dev;

	ASSERT(br_dev);

	if ((br_dev->flags & IFF_UP) == 0)
	{
		IGS_ERROR("Bridge interface %s is down\n", br_dev->name);
		return (FAILURE);
	}

	skb = dev_alloc_skb(length + ETHER_HDR_LEN);

	if (skb == NULL)
	{
		IGS_ERROR("Out of memory allocating IGMP Query packet\n");
		return (FAILURE);
	}

	IGS_DEBUG("Allocated pkt buffer for IGMP Query\n");

	skb_pull(skb, ETHER_HDR_LEN);
	memcpy(skb->data, ip, length);
	skb_put(skb, length);

	/* Add the ethernet header */
	eh = (struct ether_header *)skb_push(skb, ETH_HLEN);
	eh->ether_type = __constant_htons(ETH_P_IP);
	eh->ether_dhost[0] = 0x01;
	eh->ether_dhost[1] = 0x00;
	eh->ether_dhost[2] = 0x5e;
	eh->ether_dhost[5] = mgrp_ip & 0xff; mgrp_ip >>= 8;
	eh->ether_dhost[4] = mgrp_ip & 0xff; mgrp_ip >>= 8;
	eh->ether_dhost[3] = mgrp_ip & 0x7f;

	/* Send the frame on to the bridge device */
	memcpy(eh->ether_shost, br_dev->dev_addr, br_dev->addr_len);
	skb->mac.raw = skb->data;
	skb->dev = br_dev;
	dev_queue_xmit(skb);

	IGS_DEBUG("IGMP Query sent on %s\n", br_dev->name);

	return (SUCCESS);
}

#ifdef CONFIG_PROC_FS
/*
 * IGSL Packet Counters/Statistics Function
 */
static int32
igs_stats_get(char *buf, char **start, off_t offset, int32 size,
              int32 *eof, void *data)
{
	igs_info_t *igs_info;
	igs_cfg_request_t cfg;
	igs_stats_t *stats;
	struct bcmstrbuf b;

	igs_info = (igs_info_t *)data;

	strcpy(cfg.inst_id, igs_info->inst_id);
	cfg.command_id = IGSCFG_CMD_IGS_STATS;
	cfg.oper_type = IGSCFG_OPER_TYPE_GET;
	cfg.size = sizeof(cfg.arg);
	stats = (igs_stats_t *)cfg.arg;

	igsc_cfg_request_process(igs_info->igsc_info, &cfg);
	if (cfg.status != IGSCFG_STATUS_SUCCESS)
	{
		IGS_ERROR("Unable to get the IGS stats\n");
		return (FAILURE);
	}

	bcm_binit(&b, buf, size);
	bcm_bprintf(&b, "IgmpPkts        IgmpQueries     "
	            "IgmpReports     IgmpV2Reports   IgmpLeaves\n");
	bcm_bprintf(&b, "%-15d %-15d %-15d %-15d %d\n",
	            stats->igmp_packets, stats->igmp_queries,
	            stats->igmp_reports, stats->igmp_v2reports,
	            stats->igmp_leaves);
	bcm_bprintf(&b, "IgmpNotHandled  McastGroups     "
	            "McastMembers    MemTimeouts\n");
	bcm_bprintf(&b, "%-15d %-15d %-15d %d\n",
	            stats->igmp_not_handled, stats->igmp_mcast_groups,
	            stats->igmp_mcast_members, stats->igmp_mem_timeouts);

	if (b.size == 0)
	{
		IGS_ERROR("Input buffer overflow\n");
		return (FAILURE);
	}

	return (b.buf - b.origbuf);
}

static int32
igs_sdb_list(char *buf, char **start, off_t offset, int32 size,
             int32 *eof, void *data)
{
	igs_info_t *igs_info;
	igs_cfg_request_t cfg;
	igs_cfg_sdb_list_t *list;
	int32 i;
	struct bcmstrbuf b;

	igs_info = (igs_info_t *)data;

	strcpy(cfg.inst_id, igs_info->inst_id);
	cfg.command_id = IGSCFG_CMD_IGSDB_LIST;
	cfg.oper_type = IGSCFG_OPER_TYPE_GET;
	cfg.size = sizeof(cfg.arg);
	list = (igs_cfg_sdb_list_t *)cfg.arg;

	igsc_cfg_request_process(igs_info->igsc_info, &cfg);
	if (cfg.status != IGSCFG_STATUS_SUCCESS)
	{
		IGS_ERROR("Unable to get the IGSDB list\n");
		return (FAILURE);
	}

	bcm_binit(&b, buf, size);
	bcm_bprintf(&b, "Group           Members         Interface\n");

	for (i = 0; i < list->num_entries; i++)
	{
		bcm_bprintf(&b, "%08x        ", list->sdb_entry[i].mgrp_ip);
		bcm_bprintf(&b, "%08x        ", list->sdb_entry[i].mh_ip);
		bcm_bprintf(&b, "%s\n", list->sdb_entry[i].if_name);
	}

	if (b.size == 0)
	{
		IGS_ERROR("Input buffer overflow\n");
		return (FAILURE);
	}

	return (b.buf - b.origbuf);
}
#endif /* CONFIG_PROC_FS */

/*
 * Description: This function is called when user application enables snooping
 *              on a bridge interface. It primarily allocates memory for IGS
 *              instance data and calls common code initialization function.
 */
static igs_info_t *
igs_instance_add(int8 *inst_id, struct net_device *br_ptr)
{
	igs_info_t *igs_info;
	osl_t *osh;
	uint8 proc_name[64];

	if (igs.inst_count > IGS_MAX_INST)
	{
		IGS_ERROR("Max instance limit %d exceeded\n", IGS_MAX_INST);
		return (NULL);
	}

	igs.inst_count++;

	IGS_INFO("Creating IGS instance for %s\n", inst_id);

	osh = osl_attach(NULL, PCI_BUS , FALSE);

	ASSERT(osh);

	/* Allocate os specfic IGS info object */
	igs_info = MALLOC(osh, sizeof(igs_info_t));
	if (igs_info == NULL)
	{
		IGS_ERROR("Out of memory allocating igs_info\n");
		osl_detach(osh);
		return (NULL);
	}

	igs_info->osh = osh;

	/* Save the IGS instance identifier */
	strncpy(igs_info->inst_id, inst_id, IFNAMSIZ);
	igs_info->inst_id[IFNAMSIZ - 1] = 0;

	/* Save the device pointer */
	igs_info->br_dev = br_ptr;

	/* Initialize IGSC layer */
	if ((igs_info->igsc_info = igsc_init(inst_id, (void *)igs_info, osh)) == NULL)
	{
		IGS_ERROR("IGSC init failed\n");
		MFREE(osh, igs_info, sizeof(igs_info_t));
		osl_detach(osh);
		return (NULL);
	}

#ifdef CONFIG_PROC_FS
	sprintf(proc_name, "net/igs_stats_%s", inst_id);
	create_proc_read_entry(proc_name, 0, 0, igs_stats_get, igs_info);
	sprintf(proc_name, "net/igsdb_%s", inst_id);
	create_proc_read_entry(proc_name, 0, 0, igs_sdb_list, igs_info);
#endif /* CONFIG_PROC_FS */

	IGS_INFO("Created IGSC instance for %s\n", inst_id);

	/* Add to global IGS instance list */
	OSL_LOCK(igs.lock);
	igs_info->next = igs.list_head;
	igs.list_head = igs_info;
	OSL_UNLOCK(igs.lock);

	return (igs_info);
}

static int32
igs_instance_del(igs_info_t *igs_info)
{
	bool found = FALSE;
	osl_t *osh;
	igs_info_t *ptr, *prev;
	uint8 proc_name[64];

	OSL_LOCK(igs.lock);

	/* Delete the IGS instance */
	prev = NULL;
	for (ptr = igs.list_head; ptr != NULL; prev = ptr, ptr = ptr->next)
	{
		if (ptr == igs_info)
		{
			found = TRUE;
			if (prev != NULL)
				prev->next = ptr->next;
			else
				igs.list_head = NULL;
			break;
		}
	}

	OSL_UNLOCK(igs.lock);

	if (!found)
	{
		IGS_ERROR("IGS instance not found\n");
		return (FAILURE);
	}

	/* Free the IGS instance */
	igsc_exit(igs_info->igsc_info);

#ifdef CONFIG_PROC_FS
	sprintf(proc_name, "net/igs_stats_%s", igs_info->inst_id);
	remove_proc_entry(proc_name, 0);
	sprintf(proc_name, "net/igsdb_%s", igs_info->inst_id);
	remove_proc_entry(proc_name, 0);
#endif /* CONFIG_PROC_FS */

	osh = igs_info->osh;
	MFREE(igs_info->osh, igs_info, sizeof(igs_info_t));
	osl_detach(osh);

	return (SUCCESS);
}

static void
igs_instances_clear(void)
{
	igs_info_t *ptr, *tmp;

	OSL_LOCK(igs.lock);

	ptr = igs.list_head;

	while (ptr != NULL)
	{
		tmp = ptr->next;
		igs_instance_del(ptr);
		ptr = tmp;
	}

	OSL_UNLOCK(igs.lock);

	return;
}

static igs_info_t *
igs_instance_find(int8 *inst_id)
{
	igs_info_t *igs_info;

	ASSERT(inst_id != NULL);

	OSL_LOCK(igs.lock);

	for (igs_info = igs.list_head; igs_info != NULL; igs_info = igs_info->next)
	{
		if (strcmp(igs_info->inst_id, inst_id) == 0)
		{
			OSL_UNLOCK(igs.lock);
			return (igs_info);
		}
	}

	OSL_UNLOCK(igs.lock);

	return (NULL);
}

static void *
igs_if_name_validate(uint8 *if_name)
{
	struct net_device *dev;

	/* Get the interface pointer */
	dev = dev_get_by_name(if_name);

	if (dev == NULL)
	{
		IGS_ERROR("Interface %s doesn't exist\n", if_name);
		return (NULL);
	}

	dev_put(dev);

	return (dev);
}

/*
 * Description: This function handles the OS specific processing
 *              required for configuration commands.
 *
 * Input:       data - Configuration command parameters
 */
void
igs_cfg_request_process(igs_cfg_request_t *cfg)
{
	igs_info_t *igs_info;
	struct net_device *br_ptr;

	if (cfg == NULL)
	{
		cfg->status = IGSCFG_STATUS_FAILURE;
		cfg->size = sprintf(cfg->arg, "Invalid input buffer passed\n");
		return;
	}

	/* Validate the instance identifier */
	br_ptr = igs_if_name_validate(cfg->inst_id);
	if (br_ptr == NULL)
	{
		cfg->status = IGSCFG_STATUS_FAILURE;
		cfg->size = sprintf(cfg->arg, "Unknown instance identifier %s\n",
		                    cfg->inst_id);
		return;
	}

	/* Locate the IGS instance */
	igs_info = igs_instance_find(cfg->inst_id);
	if ((igs_info == NULL) && (cfg->command_id != IGSCFG_CMD_BR_ADD))
	{
		cfg->status = IGSCFG_STATUS_FAILURE;
		cfg->size = sprintf(cfg->arg, "Invalid instance identifier %s\n",
		                    cfg->inst_id);
		return;
	}

	/* Convert the interface name in arguments to interface pointer */
	switch (cfg->command_id)
	{
		case IGSCFG_CMD_BR_ADD:
			if (igs_info != NULL)
			{
				cfg->status = IGSCFG_STATUS_FAILURE;
				cfg->size = sprintf(cfg->arg,
				                    "IGMP Snooping already enabled on %s\n",
				                    cfg->inst_id);
				return;
			}

			/* Create a new IGS instance corresponding to the bridge
			 * interface.
			 */
			igs_info = igs_instance_add(cfg->inst_id, br_ptr);

			if (igs_info == NULL)
			{
				cfg->status = IGSCFG_STATUS_FAILURE;
				cfg->size = sprintf(cfg->arg,
				                    "IGMP Snooping enable on %s failed\n",
				                    cfg->inst_id);
				return;
			}

			cfg->status = IGSCFG_STATUS_SUCCESS;
			break;

		case IGSCFG_CMD_BR_DEL:
			/* Delete and free the IGS instance */
			if (igs_instance_del(igs_info) != SUCCESS)
			{
				cfg->status = IGSCFG_STATUS_FAILURE;
				cfg->size = sprintf(cfg->arg,
				                    "IGMP Snooping disable failed\n");
				return;
			}

			cfg->status = IGSCFG_STATUS_SUCCESS;
			break;

		case IGSCFG_CMD_BR_LIST:
			break;

		default:
			igsc_cfg_request_process(igs_info->igsc_info, cfg);
			break;
	}
	return;
}

/*
 * Description: This function is called by Linux kernel when user
 *              applications sends a message on netlink socket. It
 *              dequeues the message, calls the functions to process
 *              the commands and sends the result back to user.
 *
 * Input:       sk  - Kernel socket structure
 *              len - Length of the message received from user app.
 */
static void
igs_netlink_sock_cb(struct sock *sk, int32 len)
{
	struct sk_buff	*skb;
	struct nlmsghdr	*nlh = NULL;
	uint8 *data = NULL;

	IGS_DEBUG("Length of the command buffer %d\n", len);

	/* Dequeue the message from netlink socket */
	while ((skb = skb_dequeue(&sk->receive_queue)) != NULL)
	{
		/* Check the buffer for min size */
		if (skb->len < sizeof(igs_cfg_request_t))
		{
			IGS_ERROR("Configuration request size not > %d\n",
			          sizeof(igs_cfg_request_t));
			return;
		}


		/* Buffer contains netlink header followed by data */
		nlh = (struct nlmsghdr *)skb->data;
		data = NLMSG_DATA(nlh);

		/* Process the message */
		igs_cfg_request_process((igs_cfg_request_t *)data);

		/* Send the result to user process */
		NETLINK_CB(skb).groups = 0;
		NETLINK_CB(skb).pid = 0;
		NETLINK_CB(skb).dst_groups = 0;
		NETLINK_CB(skb).dst_pid = nlh->nlmsg_pid;

		netlink_unicast(igs.nl_sk, skb, nlh->nlmsg_pid, MSG_DONTWAIT);
	}

	return;
}

/*
 * Description: This function is called during module load time. It
 *              primarily allocates memory for IGS OS specific instance
 *              data and calls the common code initialization function.
 */
static int32 __init
igs_module_init(void)
{
#define NETLINK_IGSC 18
	igs.nl_sk = netlink_kernel_create(NETLINK_IGSC, igs_netlink_sock_cb);

	if (igs.nl_sk == NULL)
	{
		IGS_ERROR("Netlink kernel socket create failed\n");
		return (FAILURE);
	}

	igs.lock = OSL_LOCK_CREATE("IGS Instance List");

	if (igs.lock == NULL)
	{
		IGS_ERROR("IGS instance list lock create failed\n");
		return (FAILURE);
	}

	return (SUCCESS);
}

static void __exit
igs_module_exit(void)
{
	sock_release(igs.nl_sk->socket);
	igs_instances_clear();
	OSL_LOCK_DESTROY(igs.lock);

	return;
}

module_init(igs_module_init);
module_exit(igs_module_exit);
