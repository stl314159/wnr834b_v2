/*
 *  $Id: ipconfig.c,v 1.1.1.1 2010/03/05 07:31:11 reynolds Exp $
 *
 *  Automatic Configuration of IP -- use DHCP, BOOTP, RARP, or
 *  user-supplied information to configure own IP address and routes.
 *
 *  Copyright (C) 1996-1998 Martin Mares <mj@atrey.karlin.mff.cuni.cz>
 *
 *  Derived from network configuration code in fs/nfs/nfsroot.c,
 *  originally Copyright (C) 1995, 1996 Gero Kuhlmann and me.
 *
 *  BOOTP rewritten to construct and analyse packets itself instead
 *  of misusing the IP layer. num_bugs_causing_wrong_arp_replies--;
 *					     -- MJ, December 1998
 *  
 *  Fixed ip_auto_config_setup calling at startup in the new "Linker Magic"
 *  initialization scheme.
 *	- Arnaldo Carvalho de Melo <acme@conectiva.com.br>, 08/11/1999
 *
 *  DHCP support added.  To users this looks like a whole separate
 *  protocol, but we know it's just a bag on the side of BOOTP.
 *		-- Chip Salzenberg <chip@valinux.com>, May 2000
 *
 *  Ported DHCP support from 2.2.16 to 2.4.0-test4
 *              -- Eric Biederman <ebiederman@lnxi.com>, 30 Aug 2000
 *
 *  Merged changes from 2.2.19 into 2.4.3
 *              -- Eric Biederman <ebiederman@lnxi.com>, 22 April Aug 2001
 *
 *  Multipe Nameservers in /proc/net/pnp
 *              --  Josef Siemes <jsiemes@web.de>, Aug 2002
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/random.h>
#include <linux/init.h>
#include <linux/utsname.h>
#include <linux/in.h>
#include <linux/if.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/socket.h>
#include <linux/route.h>
#include <linux/udp.h>
#include <linux/proc_fs.h>
#include <linux/major.h>
#include <net/arp.h>
#include <net/ip.h>
#include <net/ipconfig.h>

#include <asm/uaccess.h>
#include <asm/checksum.h>
#include <asm/processor.h>

/* Define this to allow debugging output */
#undef IPCONFIG_DEBUG

#ifdef IPCONFIG_DEBUG
#define DBG(x) printk x
#else
#define DBG(x) do { } while(0)
#endif

#if defined(CONFIG_IP_PNP_DHCP)
#define IPCONFIG_DHCP
#endif
#if defined(CONFIG_IP_PNP_BOOTP) || defined(CONFIG_IP_PNP_DHCP)
#define IPCONFIG_BOOTP
#endif
#if defined(CONFIG_IP_PNP_RARP)
#define IPCONFIG_RARP
#endif
#if defined(IPCONFIG_BOOTP) || defined(IPCONFIG_RARP)
#define IPCONFIG_DYNAMIC
#endif

/* Define the friendly delay before and after opening net devices */
#define CONF_PRE_OPEN		(HZ/2)	/* Before opening: 1/2 second */
#define CONF_POST_OPEN		(1*HZ)	/* After opening: 1 second */

/* Define the timeout for waiting for a DHCP/BOOTP/RARP reply */
#define CONF_OPEN_RETRIES 	2	/* (Re)open devices twice */
#define CONF_SEND_RETRIES 	6	/* Send six requests per open */
#define CONF_INTER_TIMEOUT	(HZ/2)	/* Inter-device timeout: 1/2 second */
#define CONF_BASE_TIMEOUT	(HZ*2)	/* Initial timeout: 2 seconds */
#define CONF_TIMEOUT_RANDOM	(HZ)	/* Maximum amount of randomization */
#define CONF_TIMEOUT_MULT	*7/4	/* Rate of timeout growth */
#define CONF_TIMEOUT_MAX	(HZ*30)	/* Maximum allowed timeout */
#define CONF_NAMESERVERS_MAX   3       /* Maximum number of nameservers  
                                           - '3' from resolv.h */


/*
 * Public IP configuration
 */

/* This is used by platforms which might be able to set the ipconfig
 * variables using firmware environment vars.  If this is set, it will
 * ignore such firmware variables.
 */
int ic_set_manually __initdata = 0;		/* IPconfig parameters set manually */

int ic_enable __initdata = 0;			/* IP config enabled? */

/* Protocol choice */
int ic_proto_enabled __initdata = 0
#ifdef IPCONFIG_BOOTP
			| IC_BOOTP
#endif
#ifdef CONFIG_IP_PNP_DHCP
			| IC_USE_DHCP
#endif
#ifdef IPCONFIG_RARP
			| IC_RARP
#endif
			;

int ic_host_name_set __initdata = 0;		/* Host name set by us? */

u32 ic_myaddr __initdata = INADDR_NONE;		/* My IP address */
u32 ic_netmask __initdata = INADDR_NONE;	/* Netmask for local subnet */
u32 ic_gateway __initdata = INADDR_NONE;	/* Gateway IP address */

u32 ic_servaddr __initdata = INADDR_NONE;	/* Boot server IP address */

u32 root_server_addr __initdata = INADDR_NONE;	/* Address of NFS server */
u8 root_server_path[256] __initdata = { 0, };	/* Path to mount as root */

/* Persistent data: */

int ic_proto_used;			/* Protocol used, if any */
u32 ic_nameservers[CONF_NAMESERVERS_MAX]; /* DNS Server IP addresses */
u8 ic_domain[64];		/* DNS (not NIS) domain name */

/*
 * Private state.
 */

/* Name of user-selected boot device */
static char user_dev_name[IFNAMSIZ] __initdata = { 0, };

/* Protocols supported by available interfaces */
static int ic_proto_have_if __initdata = 0;

#ifdef IPCONFIG_DYNAMIC
static spinlock_t ic_recv_lock = SPIN_LOCK_UNLOCKED;
static volatile int ic_got_reply __initdata = 0;    /* Proto(s) that replied */
#endif
#ifdef IPCONFIG_DHCP
static int ic_dhcp_msgtype __initdata = 0;	/* DHCP msg type received */
#endif


/*
 *	Network devices
 */

struct ic_device {
	struct ic_device *next;
	struct net_device *dev;
	unsigned short flags;
	short able;
	u32 xid;
};

static struct ic_device *ic_first_dev __initdata = NULL;/* List of open device */
static struct net_device *ic_dev __initdata = NULL;	/* Selected device */

static int __init ic_open_devs(void)
{
	struct ic_device *d, **last;
	struct net_device *dev;
	unsigned short oflags;

	last = &ic_first_dev;
	rtnl_shlock();
	for (dev = dev_base; dev; dev = dev->next) {
		if (user_dev_name[0] ? !strcmp(dev->name, user_dev_name) :
		    (!(dev->flags & IFF_LOOPBACK) &&
		     (dev->flags & (IFF_POINTOPOINT|IFF_BROADCAST)) &&
		     strncmp(dev->name, "dummy", 5))) {
			int able = 0;
			if (dev->mtu >= 364)
				able |= IC_BOOTP;
			else
				printk(KERN_WARNING "DHCP/BOOTP: Ignoring device %s, MTU %d too small", dev->name, dev->mtu);
			if (!(dev->flags & IFF_NOARP))
				able |= IC_RARP;
			able &= ic_proto_enabled;
			if (ic_proto_enabled && !able)
				continue;
			oflags = dev->flags;
			if (dev_change_flags(dev, oflags | IFF_UP) < 0) {
				printk(KERN_ERR "IP-Config: Failed to open %s\n", dev->name);
				continue;
			}
			if (!(d = kmalloc(sizeof(struct ic_device), GFP_KERNEL))) {
				rtnl_shunlock();
				return -1;
			}
			d->dev = dev;
			*last = d;
			last = &d->next;
			d->flags = oflags;
			d->able = able;
			if (able & IC_BOOTP)
				get_random_bytes(&d->xid, sizeof(u32));
			else
				d->xid = 0;
			ic_proto_have_if |= able;
			DBG(("IP-Config: %s UP (able=%d, xid=%08x)\n",
				dev->name, able, d->xid));
		}
	}
	rtnl_shunlock();

	*last = NULL;

	if (!ic_first_dev) {
		if (user_dev_name[0])
			printk(KERN_ERR "IP-Config: Device `%s' not found.\n", user_dev_name);
		else
			printk(KERN_ERR "IP-Config: No network devices available.\n");
		return -1;
	}
	return 0;
}

static void __init ic_close_devs(void)
{
	struct ic_device *d, *next;
	struct net_device *dev;

	rtnl_shlock();
	next = ic_first_dev;
	while ((d = next)) {
		next = d->next;
		dev = d->dev;
		if (dev != ic_dev) {
			DBG(("IP-Config: Downing %s\n", dev->name));
			dev_change_flags(dev, d->flags);
		}
		kfree(d);
	}
	rtnl_shunlock();
}

/*
 *	Interface to various network functions.
 */

static inline void
set_sockaddr(struct sockaddr_in *sin, u32 addr, u16 port)
{
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = addr;
	sin->sin_port = port;
}

static int __init ic_dev_ioctl(unsigned int cmd, struct ifreq *arg)
{
	int res;

	mm_segment_t oldfs = get_fs();
	set_fs(get_ds());
	res = devinet_ioctl(cmd, arg);
	set_fs(oldfs);
	return res;
}

static int __init ic_route_ioctl(unsigned int cmd, struct rtentry *arg)
{
	int res;

	mm_segment_t oldfs = get_fs();
	set_fs(get_ds());
	res = ip_rt_ioctl(cmd, arg);
	set_fs(oldfs);
	return res;
}

/*
 *	Set up interface addresses and routes.
 */

static int __init ic_setup_if(void)
{
	struct ifreq ir;
	struct sockaddr_in *sin = (void *) &ir.ifr_ifru.ifru_addr;
	int err;

	memset(&ir, 0, sizeof(ir));
	strcpy(ir.ifr_ifrn.ifrn_name, ic_dev->name);
	set_sockaddr(sin, ic_myaddr, 0);
	if ((err = ic_dev_ioctl(SIOCSIFADDR, &ir)) < 0) {
		printk(KERN_ERR "IP-Config: Unable to set interface address (%d).\n", err);
		return -1;
	}
	set_sockaddr(sin, ic_netmask, 0);
	if ((err = ic_dev_ioctl(SIOCSIFNETMASK, &ir)) < 0) {
		printk(KERN_ERR "IP-Config: Unable to set interface netmask (%d).\n", err);
		return -1;
	}
	set_sockaddr(sin, ic_myaddr | ~ic_netmask, 0);
	if ((err = ic_dev_ioctl(SIOCSIFBRDADDR, &ir)) < 0) {
		printk(KERN_ERR "IP-Config: Unable to set interface broadcast address (%d).\n", err);
		return -1;
	}
	return 0;
}

static int __init ic_setup_routes(void)
{
	/* No need to setup device routes, only the default route... */

	if (ic_gateway != INADDR_NONE) {
		struct rtentry rm;
		int err;

		memset(&rm, 0, sizeof(rm));
		if ((ic_gateway ^ ic_myaddr) & ic_netmask) {
			printk(KERN_ERR "IP-Config: Gateway not on directly connected network.\n");
			return -1;
		}
		set_sockaddr((struct sockaddr_in *) &rm.rt_dst, 0, 0);
		set_sockaddr((struct sockaddr_in *) &rm.rt_genmask, 0, 0);
		set_sockaddr((struct sockaddr_in *) &rm.rt_gateway, ic_gateway, 0);
		rm.rt_flags = RTF_UP | RTF_GATEWAY;
		if ((err = ic_route_ioctl(SIOCADDRT, &rm)) < 0) {
			printk(KERN_ERR "IP-Config: Cannot add default route (%d).\n", err);
			return -1;
		}
	}

	return 0;
}

/*
 *	Fill in default values for all missing parameters.
 */

static int __init ic_defaults(void)
{
	/*
	 *	At this point we have no userspace running so need not
	 *	claim locks on system_utsname
	 */
	 
	if (!ic_host_name_set)
		sprintf(system_utsname.nodename, "%u.%u.%u.%u", NIPQUAD(ic_myaddr));

	if (root_server_addr == INADDR_NONE)
		root_server_addr = ic_servaddr;

	if (ic_netmask == INADDR_NONE) {
		if (IN_CLASSA(ntohl(ic_myaddr)))
			ic_netmask = htonl(IN_CLASSA_NET);
		else if (IN_CLASSB(ntohl(ic_myaddr)))
			ic_netmask = htonl(IN_CLASSB_NET);
		else if (IN_CLASSC(ntohl(ic_myaddr)))
			ic_netmask = htonl(IN_CLASSC_NET);
		else {
			printk(KERN_ERR "IP-Config: Unable to guess netmask for address %u.%u.%u.%u\n",
				NIPQUAD(ic_myaddr));
			return -1;
		}
		printk("IP-Config: Guessing netmask %u.%u.%u.%u\n", NIPQUAD(ic_netmask));
	}

	return 0;
}

/*
 *	RARP support.
 */

#ifdef IPCONFIG_RARP

static int ic_rarp_recv(struct sk_buff *skb, struct net_device *dev, struct packet_type *pt);

static struct packet_type rarp_packet_type __initdata = {
	type:	__constant_htons(ETH_P_RARP),
	func:	ic_rarp_recv,
};

static inline void ic_rarp_init(void)
{
	dev_add_pack(&rarp_packet_type);
}

static inline void ic_rarp_cleanup(void)
{
	dev_remove_pack(&rarp_packet_type);
}

/*
 *  Process received RARP packet.
 */
static int __init
ic_rarp_recv(struct sk_buff *skb, struct net_device *dev, struct packet_type *pt)
{
	struct arphdr *rarp = (struct arphdr *)skb->h.raw;
	unsigned char *rarp_ptr = (unsigned char *) (rarp + 1);
	unsigned long sip, tip;
	unsigned char *sha, *tha;		/* s for "source", t for "target" */
	struct ic_device *d;

	/* One reply at a time, please. */
	spin_lock(&ic_recv_lock);

	/* If we already have a reply, just drop the packet */
	if (ic_got_reply)
		goto drop;

	/* Find the ic_device that the packet arrived on */
	d = ic_first_dev;
	while (d && d->dev != dev)
		d = d->next;
	if (!d)
		goto drop;	/* should never happen */

	/* If this test doesn't pass, it's not IP, or we should ignore it anyway */
	if (rarp->ar_hln != dev->addr_len || dev->type != ntohs(rarp->ar_hrd))
		goto drop;

	/* If it's not a RARP reply, delete it. */
	if (rarp->ar_op != htons(ARPOP_RREPLY))
		goto drop;

	/* If it's not Ethernet, delete it. */
	if (rarp->ar_pro != htons(ETH_P_IP))
		goto drop;

	/* Extract variable-width fields */
	sha = rarp_ptr;
	rarp_ptr += dev->addr_len;
	memcpy(&sip, rarp_ptr, 4);
	rarp_ptr += 4;
	tha = rarp_ptr;
	rarp_ptr += dev->addr_len;
	memcpy(&tip, rarp_ptr, 4);

	/* Discard packets which are not meant for us. */
	if (memcmp(tha, dev->dev_addr, dev->addr_len))
		goto drop;

	/* Discard packets which are not from specified server. */
	if (ic_servaddr != INADDR_NONE && ic_servaddr != sip)
		goto drop;

	/* We have a winner! */
	ic_dev = dev;
	if (ic_myaddr == INADDR_NONE)
		ic_myaddr = tip;
	ic_servaddr = sip;
	ic_got_reply = IC_RARP;

drop:
	/* Show's over.  Nothing to see here.  */
	spin_unlock(&ic_recv_lock);

	/* Throw the packet out. */
	kfree_skb(skb);
	return 0;
}


/*
 *  Send RARP request packet over a single interface.
 */
static void __init ic_rarp_send_if(struct ic_device *d)
{
	struct net_device *dev = d->dev;
	arp_send(ARPOP_RREQUEST, ETH_P_RARP, 0, dev, 0, NULL,
		 dev->dev_addr, dev->dev_addr);
}
#endif

/*
 *	DHCP/BOOTP support.
 */

#ifdef IPCONFIG_BOOTP

struct bootp_pkt {		/* BOOTP packet format */
	struct iphdr iph;	/* IP header */
	struct udphdr udph;	/* UDP header */
	u8 op;			/* 1=request, 2=reply */
	u8 htype;		/* HW address type */
	u8 hlen;		/* HW address length */
	u8 hops;		/* Used only by gateways */
	u32 xid;		/* Transaction ID */
	u16 secs;		/* Seconds since we started */
	u16 flags;		/* Just what it says */
	u32 client_ip;		/* Client's IP address if known */
	u32 your_ip;		/* Assigned IP address */
	u32 server_ip;		/* (Next, e.g. NFS) Server's IP address */
	u32 relay_ip;		/* IP address of BOOTP relay */
	u8 hw_addr[16];		/* Client's HW address */
	u8 serv_name[64];	/* Server host name */
	u8 boot_file[128];	/* Name of boot file */
	u8 exten[312];		/* DHCP options / BOOTP vendor extensions */
};

/* packet ops */
#define BOOTP_REQUEST	1
#define BOOTP_REPLY	2

/* DHCP message types */
#define DHCPDISCOVER	1
#define DHCPOFFER	2
#define DHCPREQUEST	3
#define DHCPDECLINE	4
#define DHCPACK		5
#define DHCPNAK		6
#define DHCPRELEASE	7
#define DHCPINFORM	8

static int ic_bootp_recv(struct sk_buff *skb, struct net_device *dev, struct packet_type *pt);

static struct packet_type bootp_packet_type __initdata = {
	type:	__constant_htons(ETH_P_IP),
	func:	ic_bootp_recv,
};


/*
 *  Initialize DHCP/BOOTP extension fields in the request.
 */

static const u8 ic_bootp_cookie[4] = { 99, 130, 83, 99 };

#ifdef IPCONFIG_DHCP

static void __init
ic_dhcp_init_options(u8 *options)
{
	u8 mt = ((ic_servaddr == INADDR_NONE)
		 ? DHCPDISCOVER : DHCPREQUEST);
	u8 *e = options;

#ifdef IPCONFIG_DEBUG
	printk("DHCP: Sending message type %d\n", mt);
#endif

	memcpy(e, ic_bootp_cookie, 4);	/* RFC1048 Magic Cookie */
	e += 4;

	*e++ = 53;		/* DHCP message type */
	*e++ = 1;
	*e++ = mt;

	if (mt == DHCPREQUEST) {
		*e++ = 54;	/* Server ID (IP address) */
		*e++ = 4;
		memcpy(e, &ic_servaddr, 4);
		e += 4;

		*e++ = 50;	/* Requested IP address */
		*e++ = 4;
		memcpy(e, &ic_myaddr, 4);
		e += 4;
	}

	/* always? */
	{
		static const u8 ic_req_params[] = {
			1,	/* Subnet mask */
			3,	/* Default gateway */
			6,	/* DNS server */
			12,	/* Host name */
			15,	/* Domain name */
			17,	/* Boot path */
			40,	/* NIS domain name */
		};

		*e++ = 55;	/* Parameter request list */
		*e++ = sizeof(ic_req_params);
		memcpy(e, ic_req_params, sizeof(ic_req_params));
		e += sizeof(ic_req_params);
	}

	*e++ = 255;	/* End of the list */
}

#endif /* IPCONFIG_DHCP */

static void __init ic_bootp_init_ext(u8 *e)
{
	memcpy(e, ic_bootp_cookie, 4);	/* RFC1048 Magic Cookie */
	e += 4;
	*e++ = 1;		/* Subnet mask request */
	*e++ = 4;
	e += 4;
	*e++ = 3;		/* Default gateway request */
	*e++ = 4;
	e += 4;
	*e++ = 5;		/* Name server reqeust */
	*e++ = 8;
	e += 8;
	*e++ = 12;		/* Host name request */
	*e++ = 32;
	e += 32;
	*e++ = 40;		/* NIS Domain name request */
	*e++ = 32;
	e += 32;
	*e++ = 17;		/* Boot path */
	*e++ = 40;
	e += 40;

	*e++ = 57;		/* set extension buffer size for reply */ 
	*e++ = 2;
	*e++ = 1;		/* 128+236+8+20+14, see dhcpd sources */ 
	*e++ = 150;

	*e++ = 255;		/* End of the list */
}


/*
 *  Initialize the DHCP/BOOTP mechanism.
 */
static inline void ic_bootp_init(void)
{
	int i;

	for (i = 0; i < CONF_NAMESERVERS_MAX; i++)
		ic_nameservers[i] = INADDR_NONE;

	dev_add_pack(&bootp_packet_type);
}


/*
 *  DHCP/BOOTP cleanup.
 */
static inline void ic_bootp_cleanup(void)
{
	dev_remove_pack(&bootp_packet_type);
}


/*
 *  Send DHCP/BOOTP request to single interface.
 */
static void __init ic_bootp_send_if(struct ic_device *d, unsigned long jiffies_diff)
{
	struct net_device *dev = d->dev;
	struct sk_buff *skb;
	struct bootp_pkt *b;
	int hh_len = (dev->hard_header_len + 15) & ~15;
	struct iphdr *h;

	/* Allocate packet */
	skb = alloc_skb(sizeof(struct bootp_pkt) + hh_len + 15, GFP_KERNEL);
	if (!skb)
		return;
	skb_reserve(skb, hh_len);
	b = (struct bootp_pkt *) skb_put(skb, sizeof(struct bootp_pkt));
	memset(b, 0, sizeof(struct bootp_pkt));

	/* Construct IP header */
	skb->nh.iph = h = &b->iph;
	h->version = 4;
	h->ihl = 5;
	h->tot_len = htons(sizeof(struct bootp_pkt));
	h->frag_off = htons(IP_DF);
	h->ttl = 64;
	h->protocol = IPPROTO_UDP;
	h->daddr = INADDR_BROADCAST;
	h->check = ip_fast_csum((unsigned char *) h, h->ihl);

	/* Construct UDP header */
	b->udph.source = htons(68);
	b->udph.dest = htons(67);
	b->udph.len = htons(sizeof(struct bootp_pkt) - sizeof(struct iphdr));
	/* UDP checksum not calculated -- explicitly allowed in BOOTP RFC */

	/* Construct DHCP/BOOTP header */
	b->op = BOOTP_REQUEST;
	if (dev->type < 256) /* check for false types */
		b->htype = dev->type;
	else if (dev->type == ARPHRD_IEEE802_TR) /* fix for token ring */
		b->htype = ARPHRD_IEEE802;
	else {
		printk("Unknown ARP type 0x%04x for device %s\n", dev->type, dev->name);
		b->htype = dev->type; /* can cause undefined behavior */
	}
	b->hlen = dev->addr_len;
	b->your_ip = INADDR_NONE;
	b->server_ip = INADDR_NONE;
	memcpy(b->hw_addr, dev->dev_addr, dev->addr_len);
	b->secs = htons(jiffies_diff / HZ);
	b->xid = d->xid;

	/* add DHCP options or BOOTP extensions */
#ifdef IPCONFIG_DHCP
	if (ic_proto_enabled & IC_USE_DHCP)
		ic_dhcp_init_options(b->exten);
	else
#endif
		ic_bootp_init_ext(b->exten);

	/* Chain packet down the line... */
	skb->dev = dev;
	skb->protocol = htons(ETH_P_IP);
	if ((dev->hard_header &&
	     dev->hard_header(skb, dev, ntohs(skb->protocol), dev->broadcast, dev->dev_addr, skb->len) < 0) ||
	    dev_queue_xmit(skb) < 0)
		printk("E");
}


/*
 *  Copy BOOTP-supplied string if not already set.
 */
static int __init ic_bootp_string(char *dest, char *src, int len, int max)
{
	if (!len)
		return 0;
	if (len > max-1)
		len = max-1;
	memcpy(dest, src, len);
	dest[len] = '\0';
	return 1;
}


/*
 *  Process BOOTP extensions.
 */
static void __init ic_do_bootp_ext(u8 *ext)
{
	u8 servers;
	int i;

#ifdef IPCONFIG_DEBUG
	u8 *c;

	printk("DHCP/BOOTP: Got extension %d:",*ext);
	for(c=ext+2; c<ext+2+ext[1]; c++)
		printk(" %02x", *c);
	printk("\n");
#endif

	switch (*ext++) {
		case 1:		/* Subnet mask */
			if (ic_netmask == INADDR_NONE)
				memcpy(&ic_netmask, ext+1, 4);
			break;
		case 3:		/* Default gateway */
			if (ic_gateway == INADDR_NONE)
				memcpy(&ic_gateway, ext+1, 4);
			break;
		case 6:		/* DNS server */
			servers= *ext/4;
			if (servers > CONF_NAMESERVERS_MAX)
				servers = CONF_NAMESERVERS_MAX;
			for (i = 0; i < servers; i++) {
				if (ic_nameservers[i] == INADDR_NONE)
					memcpy(&ic_nameservers[i], ext+1+4*i, 4);
			}
			break;
		case 12:	/* Host name */
			ic_bootp_string(system_utsname.nodename, ext+1, *ext, __NEW_UTS_LEN);
			ic_host_name_set = 1;
			break;
		case 15:	/* Domain name (DNS) */
			ic_bootp_string(ic_domain, ext+1, *ext, sizeof(ic_domain));
			break;
		case 17:	/* Root path */
			if (!root_server_path[0])
				ic_bootp_string(root_server_path, ext+1, *ext, sizeof(root_server_path));
			break;
		case 40:	/* NIS Domain name (_not_ DNS) */
			ic_bootp_string(system_utsname.domainname, ext+1, *ext, __NEW_UTS_LEN);
			break;
	}
}


/*
 *  Receive BOOTP reply.
 */
static int __init ic_bootp_recv(struct sk_buff *skb, struct net_device *dev, struct packet_type *pt)
{
	struct bootp_pkt *b = (struct bootp_pkt *) skb->nh.iph;
	struct iphdr *h = &b->iph;
	struct ic_device *d;
	int len;

	/* One reply at a time, please. */
	spin_lock(&ic_recv_lock);

	/* If we already have a reply, just drop the packet */
	if (ic_got_reply)
		goto drop;

	/* Find the ic_device that the packet arrived on */
	d = ic_first_dev;
	while (d && d->dev != dev)
		d = d->next;
	if (!d)
		goto drop;  /* should never happen */

	/* Check whether it's a BOOTP packet */
	if (skb->pkt_type == PACKET_OTHERHOST ||
	    skb->len < sizeof(struct udphdr) + sizeof(struct iphdr) ||
	    h->ihl != 5 ||
	    h->version != 4 ||
	    ip_fast_csum((char *) h, h->ihl) != 0 ||
	    skb->len < ntohs(h->tot_len) ||
	    h->protocol != IPPROTO_UDP ||
	    b->udph.source != htons(67) ||
	    b->udph.dest != htons(68) ||
	    ntohs(h->tot_len) < ntohs(b->udph.len) + sizeof(struct iphdr))
		goto drop;

	/* Fragments are not supported */
	if (h->frag_off & htons(IP_OFFSET | IP_MF)) {
		printk(KERN_ERR "DHCP/BOOTP: Ignoring fragmented reply.\n");
		goto drop;
	}

	/* Is it a reply to our BOOTP request? */
	len = ntohs(b->udph.len) - sizeof(struct udphdr);
	if (len < 300 ||				    /* See RFC 951:2.1 */
	    b->op != BOOTP_REPLY ||
	    b->xid != d->xid) {
		printk("?");
		goto drop;
	}

	/* Parse extensions */
	if (!memcmp(b->exten, ic_bootp_cookie, 4)) { /* Check magic cookie */
                u8 *end = (u8 *) b + ntohs(b->iph.tot_len);
		u8 *ext;

#ifdef IPCONFIG_DHCP
		if (ic_proto_enabled & IC_USE_DHCP) {
			u32 server_id = INADDR_NONE;
			int mt = 0;

			ext = &b->exten[4];
			while (ext < end && *ext != 0xff) {
				u8 *opt = ext++;
				if (*opt == 0)	/* Padding */
					continue;
				ext += *ext + 1;
				if (ext >= end)
					break;
				switch (*opt) {
				case 53:	/* Message type */
					if (opt[1])
						mt = opt[2];
					break;
				case 54:	/* Server ID (IP address) */
					if (opt[1] >= 4)
						memcpy(&server_id, opt + 2, 4);
					break;
				};
			}

#ifdef IPCONFIG_DEBUG
			printk("DHCP: Got message type %d\n", mt);
#endif

			switch (mt) {
			case DHCPOFFER:
				/* While in the process of accepting one offer,
				 * ignore all others.
				 */
				if (ic_myaddr != INADDR_NONE)
					goto drop;

				/* Let's accept that offer. */
				ic_myaddr = b->your_ip;
				ic_servaddr = server_id;
#ifdef IPCONFIG_DEBUG
				printk("DHCP: Offered address %u.%u.%u.%u",
				       NIPQUAD(ic_myaddr));
				printk(" by server %u.%u.%u.%u\n",
				       NIPQUAD(ic_servaddr));
#endif
				/* The DHCP indicated server address takes
				 * precedence over the bootp header one if
				 * they are different.
				 */
				if ((server_id != INADDR_NONE) &&
				    (b->server_ip != server_id))
					b->server_ip = ic_servaddr;
				break;

			case DHCPACK:
				/* Yeah! */
				break;

			default:
				/* Urque.  Forget it*/
				ic_myaddr = INADDR_NONE;
				ic_servaddr = INADDR_NONE;
				goto drop;
			};

			ic_dhcp_msgtype = mt;

		}
#endif /* IPCONFIG_DHCP */

		ext = &b->exten[4];
		while (ext < end && *ext != 0xff) {
			u8 *opt = ext++;
			if (*opt == 0)	/* Padding */
				continue;
			ext += *ext + 1;
			if (ext < end)
				ic_do_bootp_ext(opt);
		}
	}

	/* We have a winner! */
	ic_dev = dev;
	ic_myaddr = b->your_ip;
	if (ic_servaddr == INADDR_NONE)
		ic_servaddr = b->server_ip;
	if (ic_gateway == INADDR_NONE && b->relay_ip)
		ic_gateway = b->relay_ip;
	if (ic_nameservers[0] == INADDR_NONE)
		ic_nameservers[0] = ic_servaddr;
	ic_got_reply = IC_BOOTP;

drop:
	/* Show's over.  Nothing to see here.  */
	spin_unlock(&ic_recv_lock);

	/* Throw the packet out. */
	kfree_skb(skb);

	return 0;
}	


#endif


/*
 *	Dynamic IP configuration -- DHCP, BOOTP, RARP.
 */

#ifdef IPCONFIG_DYNAMIC

static int __init ic_dynamic(void)
{
	int retries;
	struct ic_device *d;
	unsigned long start_jiffies, timeout, jiff;
	int do_bootp = ic_proto_have_if & IC_BOOTP;
	int do_rarp = ic_proto_have_if & IC_RARP;

	/*
	 * If none of DHCP/BOOTP/RARP was selected, return with an error.
	 * This routine gets only called when some pieces of information
	 * are missing, and without DHCP/BOOTP/RARP we are unable to get it.
	 */
	if (!ic_proto_enabled) {
		printk(KERN_ERR "IP-Config: Incomplete network configuration information.\n");
		return -1;
	}

#ifdef IPCONFIG_BOOTP
	if ((ic_proto_enabled ^ ic_proto_have_if) & IC_BOOTP)
		printk(KERN_ERR "DHCP/BOOTP: No suitable device found.\n");
#endif
#ifdef IPCONFIG_RARP
	if ((ic_proto_enabled ^ ic_proto_have_if) & IC_RARP)
		printk(KERN_ERR "RARP: No suitable device found.\n");
#endif

	if (!ic_proto_have_if)
		/* Error message already printed */
		return -1;

	/*
	 * Setup protocols
	 */
#ifdef IPCONFIG_BOOTP
	if (do_bootp)
		ic_bootp_init();
#endif
#ifdef IPCONFIG_RARP
	if (do_rarp)
		ic_rarp_init();
#endif

	/*
	 * Send requests and wait, until we get an answer. This loop
	 * seems to be a terrible waste of CPU time, but actually there is
	 * only one process running at all, so we don't need to use any
	 * scheduler functions.
	 * [Actually we could now, but the nothing else running note still 
	 *  applies.. - AC]
	 */
	printk(KERN_NOTICE "Sending %s%s%s requests .",
	       do_bootp
		? ((ic_proto_enabled & IC_USE_DHCP) ? "DHCP" : "BOOTP") : "",
	       (do_bootp && do_rarp) ? " and " : "",
	       do_rarp ? "RARP" : "");

	start_jiffies = jiffies;
	d = ic_first_dev;
	retries = CONF_SEND_RETRIES;
	get_random_bytes(&timeout, sizeof(timeout));
	timeout = CONF_BASE_TIMEOUT + (timeout % (unsigned) CONF_TIMEOUT_RANDOM);
	for(;;) {
#ifdef IPCONFIG_BOOTP
		if (do_bootp && (d->able & IC_BOOTP))
			ic_bootp_send_if(d, jiffies - start_jiffies);
#endif
#ifdef IPCONFIG_RARP
		if (do_rarp && (d->able & IC_RARP))
			ic_rarp_send_if(d);
#endif

		jiff = jiffies + (d->next ? CONF_INTER_TIMEOUT : timeout);
		while (time_before(jiffies, jiff) && !ic_got_reply) {
			barrier();
			cpu_relax();
		}
#ifdef IPCONFIG_DHCP
		/* DHCP isn't done until we get a DHCPACK. */
		if ((ic_got_reply & IC_BOOTP)
		    && (ic_proto_enabled & IC_USE_DHCP)
		    && ic_dhcp_msgtype != DHCPACK)
		{
			ic_got_reply = 0;
			printk(",");
			continue;
		}
#endif /* IPCONFIG_DHCP */

		if (ic_got_reply) {
			printk(" OK\n");
			break;
		}

		if ((d = d->next))
			continue;

		if (! --retries) {
			printk(" timed out!\n");
			break;
		}

		d = ic_first_dev;

		timeout = timeout CONF_TIMEOUT_MULT;
		if (timeout > CONF_TIMEOUT_MAX)
			timeout = CONF_TIMEOUT_MAX;

		printk(".");
	}

#ifdef IPCONFIG_BOOTP
	if (do_bootp)
		ic_bootp_cleanup();
#endif
#ifdef IPCONFIG_RARP
	if (do_rarp)
		ic_rarp_cleanup();
#endif

	if (!ic_got_reply)
		return -1;

	printk("IP-Config: Got %s answer from %u.%u.%u.%u, ",
		((ic_got_reply & IC_RARP) ? "RARP" 
		 : (ic_proto_enabled & IC_USE_DHCP) ? "DHCP" : "BOOTP"),
		NIPQUAD(ic_servaddr));
	printk("my address is %u.%u.%u.%u\n", NIPQUAD(ic_myaddr));

	return 0;
}

#endif /* IPCONFIG_DYNAMIC */

#ifdef CONFIG_PROC_FS

static int pnp_get_info(char *buffer, char **start,
			off_t offset, int length)
{
	int len;
	int i;

	if (ic_proto_used & IC_PROTO)
	    sprintf(buffer, "#PROTO: %s\n",
		    (ic_proto_used & IC_RARP) ? "RARP"
		    : (ic_proto_used & IC_USE_DHCP) ? "DHCP" : "BOOTP");
	else
	    strcpy(buffer, "#MANUAL\n");
	len = strlen(buffer);

	if (ic_domain[0])
		len += sprintf(buffer + len,
			       "domain %s\n", ic_domain);
	for (i = 0; i < CONF_NAMESERVERS_MAX; i++) {
		if (ic_nameservers[i] != INADDR_NONE)
			len += sprintf(buffer + len,
				       "nameserver %u.%u.%u.%u\n",
				       NIPQUAD(ic_nameservers[i]));
	}

	if (offset > len)
		offset = len;
	*start = buffer + offset;

	if (offset + length > len)
		length = len - offset;
	return length;
}

#endif /* CONFIG_PROC_FS */

/*
 *  Extract IP address from the parameter string if needed. Note that we
 *  need to have root_server_addr set _before_ IPConfig gets called as it
 *  can override it.
 */
u32 __init root_nfs_parse_addr(char *name)
{
	u32 addr;
	int octets = 0;
	char *cp, *cq;

	cp = cq = name;
	while (octets < 4) {
		while (*cp >= '0' && *cp <= '9')
			cp++;
		if (cp == cq || cp - cq > 3)
			break;
		if (*cp == '.' || octets == 3)
			octets++;
		if (octets < 4)
			cp++;
		cq = cp;
	}
	if (octets == 4 && (*cp == ':' || *cp == '\0')) {
		if (*cp == ':')
			*cp++ = '\0';
		addr = in_aton(name);
		strcpy(name, cp);
	} else
		addr = INADDR_NONE;

	return addr;
}

/*
 *	IP Autoconfig dispatcher.
 */

static int __init ip_auto_config(void)
{
	unsigned long jiff;
	u32 addr;

#ifdef CONFIG_PROC_FS
	proc_net_create("pnp", 0, pnp_get_info);
#endif /* CONFIG_PROC_FS */

	if (!ic_enable)
		return 0;

	DBG(("IP-Config: Entered.\n"));

#ifdef IPCONFIG_DYNAMIC
 try_try_again:
#endif
	/* Give hardware a chance to settle */
	jiff = jiffies + CONF_PRE_OPEN;
	while (time_before(jiffies, jiff))
		;

	/* Setup all network devices */
	if (ic_open_devs() < 0)
		return -1;

	/* Give drivers a chance to settle */
	jiff = jiffies + CONF_POST_OPEN;
	while (time_before(jiffies, jiff))
			;

	/*
	 * If the config information is insufficient (e.g., our IP address or
	 * IP address of the boot server is missing or we have multiple network
	 * interfaces and no default was set), use BOOTP or RARP to get the
	 * missing values.
	 */
	if (ic_myaddr == INADDR_NONE ||
#ifdef CONFIG_ROOT_NFS
	    (MAJOR(ROOT_DEV) == UNNAMED_MAJOR
	     && root_server_addr == INADDR_NONE
	     && ic_servaddr == INADDR_NONE) ||
#endif
	    ic_first_dev->next) {
#ifdef IPCONFIG_DYNAMIC

		int retries = CONF_OPEN_RETRIES;

		if (ic_dynamic() < 0) {
			ic_close_devs();

			/*
			 * I don't know why, but sometimes the
			 * eepro100 driver (at least) gets upset and
			 * doesn't work the first time it's opened.
			 * But then if you close it and reopen it, it
			 * works just fine.  So we need to try that at
			 * least once before giving up.
			 *
			 * Also, if the root will be NFS-mounted, we
			 * have nowhere to go if DHCP fails.  So we
			 * just have to keep trying forever.
			 *
			 * 				-- Chip
			 */
#ifdef CONFIG_ROOT_NFS
			if (ROOT_DEV == MKDEV(UNNAMED_MAJOR, 255)) {
				printk(KERN_ERR 
					"IP-Config: Retrying forever (NFS root)...\n");
				goto try_try_again;
			}
#endif

			if (--retries) {
				printk(KERN_ERR 
				       "IP-Config: Reopening network devices...\n");
				goto try_try_again;
			}

			/* Oh, well.  At least we tried. */
			printk(KERN_ERR "IP-Config: Auto-configuration of network failed.\n");
			return -1;
		}
#else /* !DYNAMIC */
		printk(KERN_ERR "IP-Config: Incomplete network configuration information.\n");
		ic_close_devs();
		return -1;
#endif /* IPCONFIG_DYNAMIC */
	} else {
		/* Device selected manually or only one device -> use it */
		ic_dev = ic_first_dev->dev;
	}

	addr = root_nfs_parse_addr(root_server_path);
	if (root_server_addr == INADDR_NONE)
		root_server_addr = addr;

	/*
	 * Use defaults whereever applicable.
	 */
	if (ic_defaults() < 0)
		return -1;

	/*
	 * Close all network devices except the device we've
	 * autoconfigured and set up routes.
	 */
	ic_close_devs();
	if (ic_setup_if() < 0 || ic_setup_routes() < 0)
		return -1;

	/*
	 * Record which protocol was actually used.
	 */
#ifdef IPCONFIG_DYNAMIC
	ic_proto_used = ic_got_reply | (ic_proto_enabled & IC_USE_DHCP);
#endif

#ifndef IPCONFIG_SILENT
	/*
	 * Clue in the operator.
	 */
	printk("IP-Config: Complete:");
	printk("\n      device=%s", ic_dev->name);
	printk(", addr=%u.%u.%u.%u", NIPQUAD(ic_myaddr));
	printk(", mask=%u.%u.%u.%u", NIPQUAD(ic_netmask));
	printk(", gw=%u.%u.%u.%u", NIPQUAD(ic_gateway));
	printk(",\n     host=%s, domain=%s, nis-domain=%s",
	       system_utsname.nodename, ic_domain, system_utsname.domainname);
	printk(",\n     bootserver=%u.%u.%u.%u", NIPQUAD(ic_servaddr));
	printk(", rootserver=%u.%u.%u.%u", NIPQUAD(root_server_addr));
	printk(", rootpath=%s", root_server_path);
	printk("\n");
#endif /* !SILENT */

	return 0;
}

module_init(ip_auto_config);


/*
 *  Decode any IP configuration options in the "ip=" or "nfsaddrs=" kernel
 *  command line parameter. It consists of option fields separated by colons in
 *  the following order:
 *
 *  <client-ip>:<server-ip>:<gw-ip>:<netmask>:<host name>:<device>:<PROTO>
 *
 *  Any of the fields can be empty which means to use a default value:
 *	<client-ip>	- address given by BOOTP or RARP
 *	<server-ip>	- address of host returning BOOTP or RARP packet
 *	<gw-ip>		- none, or the address returned by BOOTP
 *	<netmask>	- automatically determined from <client-ip>, or the
 *			  one returned by BOOTP
 *	<host name>	- <client-ip> in ASCII notation, or the name returned
 *			  by BOOTP
 *	<device>	- use all available devices
 *	<PROTO>:
 *	   off|none	    - don't do autoconfig at all (DEFAULT)
 *	   on|any           - use any configured protocol
 *	   dhcp|bootp|rarp  - use only the specified protocol
 *	   both             - use both BOOTP and RARP (not DHCP)
 */
static int __init ic_proto_name(char *name)
{
	if (!strcmp(name, "on") || !strcmp(name, "any")) {
		return 1;
	}
#ifdef CONFIG_IP_PNP_DHCP
	else if (!strcmp(name, "dhcp")) {
		ic_proto_enabled &= ~IC_RARP;
		return 1;
	}
#endif
#ifdef CONFIG_IP_PNP_BOOTP
	else if (!strcmp(name, "bootp")) {
		ic_proto_enabled &= ~(IC_RARP | IC_USE_DHCP);
		return 1;
	}
#endif
#ifdef CONFIG_IP_PNP_RARP
	else if (!strcmp(name, "rarp")) {
		ic_proto_enabled &= ~(IC_BOOTP | IC_USE_DHCP);
		return 1;
	}
#endif
#ifdef IPCONFIG_DYNAMIC
	else if (!strcmp(name, "both")) {
		ic_proto_enabled &= ~IC_USE_DHCP; /* backward compat :-( */
		return 1;
	}
#endif
	return 0;
}

static int __init ip_auto_config_setup(char *addrs)
{
	char *cp, *ip, *dp;
	int num = 0;

	ic_set_manually = 1;

	ic_enable = (*addrs && 
		(strcmp(addrs, "off") != 0) && 
		(strcmp(addrs, "none") != 0));
	if (!ic_enable)
		return 1;

	if (ic_proto_name(addrs))
		return 1;

	/* Parse the whole string */
	ip = addrs;
	while (ip && *ip) {
		if ((cp = strchr(ip, ':')))
			*cp++ = '\0';
		if (strlen(ip) > 0) {
			DBG(("IP-Config: Parameter #%d: `%s'\n", num, ip));
			switch (num) {
			case 0:
				if ((ic_myaddr = in_aton(ip)) == INADDR_ANY)
					ic_myaddr = INADDR_NONE;
				break;
			case 1:
				if ((ic_servaddr = in_aton(ip)) == INADDR_ANY)
					ic_servaddr = INADDR_NONE;
				break;
			case 2:
				if ((ic_gateway = in_aton(ip)) == INADDR_ANY)
					ic_gateway = INADDR_NONE;
				break;
			case 3:
				if ((ic_netmask = in_aton(ip)) == INADDR_ANY)
					ic_netmask = INADDR_NONE;
				break;
			case 4:
				if ((dp = strchr(ip, '.'))) {
					*dp++ = '\0';
					strncpy(system_utsname.domainname, dp, __NEW_UTS_LEN);
					system_utsname.domainname[__NEW_UTS_LEN] = '\0';
				}
				strncpy(system_utsname.nodename, ip, __NEW_UTS_LEN);
				system_utsname.nodename[__NEW_UTS_LEN] = '\0';
				ic_host_name_set = 1;
				break;
			case 5:
				strncpy(user_dev_name, ip, IFNAMSIZ);
				user_dev_name[IFNAMSIZ-1] = '\0';
				break;
			case 6:
				ic_proto_name(ip);
				break;
			}
		}
		ip = cp;
		num++;
	}

	return 1;
}

static int __init nfsaddrs_config_setup(char *addrs)
{
	return ip_auto_config_setup(addrs);
}

__setup("ip=", ip_auto_config_setup);
__setup("nfsaddrs=", nfsaddrs_config_setup);
