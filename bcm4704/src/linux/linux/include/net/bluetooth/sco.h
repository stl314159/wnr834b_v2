/* 
   BlueZ - Bluetooth protocol stack for Linux
   Copyright (C) 2000-2001 Qualcomm Incorporated

   Written 2000,2001 by Maxim Krasnyansky <maxk@qualcomm.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 2 as
   published by the Free Software Foundation;

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS.
   IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) AND AUTHOR(S) BE LIABLE FOR ANY
   CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES 
   WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN 
   ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF 
   OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

   ALL LIABILITY, INCLUDING LIABILITY FOR INFRINGEMENT OF ANY PATENTS, 
   COPYRIGHTS, TRADEMARKS OR OTHER RIGHTS, RELATING TO USE OF THIS 
   SOFTWARE IS DISCLAIMED.
*/

/*
 *  $Id: sco.h,v 1.1.1.1 2010/03/05 07:31:15 reynolds Exp $
 */

#ifndef __SCO_H
#define __SCO_H

/* SCO defaults */
#define SCO_DEFAULT_MTU 	500
#define SCO_DEFAULT_FLUSH_TO	0xFFFF

#define SCO_CONN_TIMEOUT 	(HZ * 40)
#define SCO_DISCONN_TIMEOUT 	(HZ * 2)
#define SCO_CONN_IDLE_TIMEOUT	(HZ * 60)

/* SCO socket address */
struct sockaddr_sco {
	sa_family_t	sco_family;
	bdaddr_t	sco_bdaddr;
};

/* set/get sockopt defines */
#define SCO_OPTIONS  0x01
struct sco_options {
	__u16 mtu;
};

#define SCO_CONNINFO  0x02
struct sco_conninfo {
	__u16 hci_handle;
};

/* ---- SCO connections ---- */
struct sco_conn {
	struct hci_conn	*hcon;

	bdaddr_t 	*dst;
	bdaddr_t 	*src;
	
	spinlock_t	lock;
	struct sock 	*sk;

	unsigned int    mtu;
};

#define sco_conn_lock(c)	spin_lock(&c->lock);
#define sco_conn_unlock(c)	spin_unlock(&c->lock);

/* ----- SCO socket info ----- */
#define sco_pi(sk)   ((struct sco_pinfo *) &sk->tp_pinfo)

struct sco_pinfo {
	__u32		flags;
	struct sco_conn	*conn;
};

#endif /* __SCO_H */
