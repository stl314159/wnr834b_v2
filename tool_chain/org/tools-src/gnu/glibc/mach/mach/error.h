/*  err_hurd added by roland@gnu.ai.mit.edu for GNU Hurd.
 *
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988,1987 Carnegie Mellon University
 * All Rights Reserved.
 *
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 *
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 *
 * Carnegie Mellon requests users of this software to return to
 *
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 *
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
 * HISTORY
 * $Log: error.h,v $
 * Revision 1.1.1.1  2010/03/05 07:30:19  reynolds
 * NETGEAR WNR834B v2 router toolchain and freeware.
 *
 * Revision 1.1.1.1  2004/03/29 19:10:14  jsiegel
 * Test Copy
 *
 * Revision 1.1.1.1  2003/02/11 23:28:35  mhuang
 * glibc-2.2.5
 *
 * Revision 1.3  1997/03/16 17:43:08  drepper
 * (mach_error_fn_t): Comment out declaration; it appears to be entirely
 * unused dead code.
 *
 * Revision 1.3  1997/03/14 15:27:35  thomas
 * Wed Mar  5 10:40:05 1997  Thomas Bushnell, n/BSG  <thomas@gnu.ai.mit.edu>
 *
 * 	* mach/mach/mach_traps.h: Include <mach/kern_return.h>.
 *
 * 	* mach/mach/error.h (mach_error_fn_t): Comment out declaration; it
 * 	appears to be entirely unused dead code.
 *
 * Revision 1.2  1996/12/20 01:32:36  drepper
 * Update from main archive 961219
 *
 * Revision 1.2  1996/12/19 20:23:56  drepper
 * Spelling corrections.
 *
 * Revision 1.1  1993/12/17 21:40:28  roland
 * entered into RCS
 *
 * Revision 2.6  93/01/14  17:41:31  danner
 * 	Standardized include symbol name.
 * 	[92/06/10            pds]
 *
 * Revision 2.5  92/03/31  15:18:11  rpd
 * 	Added err_bootstrap for bootstrap errors.
 * 	[92/03/09            rpd]
 *
 * Revision 2.4  91/05/14  16:51:24  mrt
 * 	Correcting copyright
 *
 * Revision 2.3  91/02/05  17:31:48  mrt
 * 	Changed to new Mach copyright
 * 	[91/02/01  17:16:50  mrt]
 *
 * Revision 2.2  90/06/02  14:57:47  rpd
 * 	Added err_mach_ipc for new IPC.
 * 	[90/03/26  22:28:42  rpd]
 *
 * Revision 2.1  89/08/03  16:02:07  rwd
 * Created.
 *
 * Revision 2.4  89/02/25  18:13:18  gm0w
 * 	Changes for cleanup.
 *
 * Revision 2.3  89/02/07  00:51:57  mwyoung
 * Relocated from sys/error.h
 *
 * Revision 2.2  88/10/18  00:37:31  mwyoung
 * 	Added {system,sub and code}_emask
 * 	[88/10/17  17:06:58  mrt]
 *
 *	Added {system,sub and code}_emask
 *
 *  12-May-88 Mary Thompson (mrt) at Carnegie Mellon
 *	Changed mach_error_t from unsigned int to kern_return_t
 *	which is a 32 bit integer regardless of machine type.
 *      unsigned int was incompatible with old usages of mach_error.
 *
 *  10-May-88 Douglas Orr (dorr) at Carnegie-Mellon University
 *	Missing endif replaced
 *
 *   5-May-88 Mary Thompson (mrt) at Carnegie Mellon
 *	Changed typedef of mach_error_t from long to unsigned int
 *	to keep our Camelot users happy. Also moved the nonkernel
 *	function declarations from here to mach_error.h.
 *
 *  10-Feb-88 Douglas Orr (dorr) at Carnegie-Mellon University
 *	Created.
 *
 */
/*
 * File:	mach/error.h
 * Purpose:
 *	error module definitions
 *
 */

#ifndef	_MACH_ERROR_H_
#define _MACH_ERROR_H_
#include <mach/kern_return.h>

/*
 *	error number layout as follows:
 *
 *	hi		 		       lo
 *	| system(6) | subsystem(12) | code(14) |
 */


#define	err_none		(mach_error_t)0
#define ERR_SUCCESS		(mach_error_t)0
#define	ERR_ROUTINE_NIL		(mach_error_fn_t)0


#define	err_system(x)		(((x)&0x3f)<<26)
#define err_sub(x)		(((x)&0xfff)<<14)

#define err_get_system(err)	(((err)>>26)&0x3f)
#define err_get_sub(err)	(((err)>>14)&0xfff)
#define err_get_code(err)	((err)&0x3fff)

#define system_emask		(err_system(0x3f))
#define sub_emask		(err_sub(0xfff))
#define code_emask		(0x3fff)


/*	major error systems	*/
#define	err_kern		err_system(0x0)		/* kernel */
#define	err_us			err_system(0x1)		/* user space library */
#define	err_server		err_system(0x2)		/* user space servers */
#define	err_ipc			err_system(0x3)		/* old ipc errors */
#define err_mach_ipc		err_system(0x4)		/* mach-ipc errors */
#define err_bootstrap		err_system(0x5)		/* bootstrap errors */
#define err_hurd		err_system(0x10) /* GNU Hurd server errors */
#define err_local		err_system(0x3e)	/* user defined errors */
#define	err_ipc_compat		err_system(0x3f)	/* (compatibility) mach-ipc errors */

#define	err_max_system		0x3f


/*	unix errors get lumped into one subsystem  */
#define	unix_err(errno)		(err_kern|err_sub(3)|errno)

typedef	kern_return_t	mach_error_t;
/* typedef mach_error_t	(* mach_error_fn_t)(); */

#endif	/* _MACH_ERROR_H_ */
