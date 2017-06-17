/* $Id: errtbls.c,v 1.1.1.1 2010/03/05 07:31:15 reynolds Exp $
 * errtbls.c: Error number conversion tables between various syscall
 *            OS semantics.
 *
 * Copyright (C) 1995 David S. Miller (davem@caip.rutgers.edu)
 *
 * Based upon preliminary work which is:
 *
 * Copyright (C) 1995 Adrian M. Rodriguez (adrian@remus.rutgers.edu)
 */

#include <asm/bsderrno.h>        /* NetBSD (bsd4.4) errnos */
#include <asm/solerrno.h>        /* Solaris errnos */

/* Here are tables which convert between Linux/SunOS error number
 * values to the equivalent in other OSs.  Note that since the Linux
 * ones have been set up to match exactly those of SunOS, no
 * translation table is needed for that OS.
 */

int solaris_errno[] = {
	0,
	SOL_EPERM,
	SOL_ENOENT,
	SOL_ESRCH,
	SOL_EINTR,
	SOL_EIO,
	SOL_ENXIO,
	SOL_E2BIG,
	SOL_ENOEXEC,
	SOL_EBADF,
	SOL_ECHILD,
	SOL_EAGAIN,
	SOL_ENOMEM,
	SOL_EACCES,
	SOL_EFAULT,
	SOL_NOTBLK,
	SOL_EBUSY,
	SOL_EEXIST,
	SOL_EXDEV,
	SOL_ENODEV,
	SOL_ENOTDIR,
	SOL_EISDIR,
	SOL_EINVAL,
	SOL_ENFILE,
	SOL_EMFILE,
	SOL_ENOTTY,
	SOL_ETXTBSY,
	SOL_EFBIG,
	SOL_ENOSPC,
	SOL_ESPIPE,
	SOL_EROFS,
	SOL_EMLINK,
	SOL_EPIPE,
	SOL_EDOM,
	SOL_ERANGE,
	SOL_EWOULDBLOCK,
	SOL_EINPROGRESS,
	SOL_EALREADY,
	SOL_ENOTSOCK,
	SOL_EDESTADDRREQ,
	SOL_EMSGSIZE,
	SOL_EPROTOTYPE,
	SOL_ENOPROTOOPT,
	SOL_EPROTONOSUPPORT,
	SOL_ESOCKTNOSUPPORT,
	SOL_EOPNOTSUPP,
	SOL_EPFNOSUPPORT,
	SOL_EAFNOSUPPORT,
	SOL_EADDRINUSE,
	SOL_EADDRNOTAVAIL,
	SOL_ENETDOWN,
	SOL_ENETUNREACH,
	SOL_ENETRESET,
	SOL_ECONNABORTED,
	SOL_ECONNRESET,
	SOL_ENOBUFS,
	SOL_EISCONN,
	SOL_ENOTONN,
	SOL_ESHUTDOWN,
	SOL_ETOOMANYREFS,
	SOL_ETIMEDOUT,
	SOL_ECONNREFUSED,
	SOL_ELOOP,
	SOL_ENAMETOOLONG,
	SOL_EHOSTDOWN,
	SOL_EHOSTUNREACH,
	SOL_ENOTEMPTY,
	SOL_EPROCLIM,
	SOL_EUSERS,
	SOL_EDQUOT,
	SOL_ESTALE,
	SOL_EREMOTE,
	SOL_ENOSTR,
	SOL_ETIME,
	SOL_ENOSR,
	SOL_ENOMSG,
	SOL_EBADMSG,
	SOL_IDRM,
	SOL_EDEADLK,
	SOL_ENOLCK,
	SOL_ENONET,
	SOL_ERREMOTE,
	SOL_ENOLINK,
	SOL_EADV,
	SOL_ESRMNT,
	SOL_ECOMM,
	SOL_EPROTO,
	SOL_EMULTIHOP,
	SOL_EINVAL,    
	SOL_REMCHG,
	SOL_NOSYS,
	SOL_STRPIPE,
	SOL_EOVERFLOW,
	SOL_EBADFD,
	SOL_ECHRNG,
	SOL_EL2NSYNC,
	SOL_EL3HLT,
	SOL_EL3RST,
	SOL_NRNG,
	SOL_EUNATCH,
	SOL_ENOCSI,
	SOL_EL2HLT,
	SOL_EBADE,
	SOL_EBADR,
	SOL_EXFULL,
	SOL_ENOANO,
	SOL_EBADRQC,
	SOL_EBADSLT,
	SOL_EDEADLOCK,
	SOL_EBFONT,
	SOL_ELIBEXEC,
	SOL_ENODATA,
	SOL_ELIBBAD,
	SOL_ENOPKG,
	SOL_ELIBACC,
	SOL_ENOTUNIQ,
	SOL_ERESTART,
	SOL_EUCLEAN,
	SOL_ENOTNAM,
	SOL_ENAVAIL,
	SOL_EISNAM,
	SOL_EREMOTEIO,
	SOL_EILSEQ,
	SOL_ELIBMAX,
	SOL_ELIBSCN,
};

int netbsd_errno[] = {
	0,
	BSD_EPERM,
	BSD_ENOENT,
	BSD_ESRCH,
	BSD_EINTR,
	BSD_EIO,
	BSD_ENXIO,
	BSD_E2BIG,
	BSD_ENOEXEC,
	BSD_EBADF,
	BSD_ECHILD,
	BSD_EAGAIN,
	BSD_ENOMEM,
	BSD_EACCES,
	BSD_EFAULT,
	BSD_NOTBLK,
	BSD_EBUSY,
	BSD_EEXIST,
	BSD_EXDEV,
	BSD_ENODEV,
	BSD_ENOTDIR,
	BSD_EISDIR,
	BSD_EINVAL,
	BSD_ENFILE,
	BSD_EMFILE,
	BSD_ENOTTY,
	BSD_ETXTBSY,
	BSD_EFBIG,
	BSD_ENOSPC,
	BSD_ESPIPE,
	BSD_EROFS,
	BSD_EMLINK,
	BSD_EPIPE,
	BSD_EDOM,
	BSD_ERANGE,
	BSD_EWOULDBLOCK,
	BSD_EINPROGRESS,
	BSD_EALREADY,
	BSD_ENOTSOCK,
	BSD_EDESTADDRREQ,
	BSD_EMSGSIZE,
	BSD_EPROTOTYPE,
	BSD_ENOPROTOOPT,
	BSD_EPROTONOSUPPORT,
	BSD_ESOCKTNOSUPPORT,
	BSD_EOPNOTSUPP,
	BSD_EPFNOSUPPORT,
	BSD_EAFNOSUPPORT,
	BSD_EADDRINUSE,
	BSD_EADDRNOTAVAIL,
	BSD_ENETDOWN,
	BSD_ENETUNREACH,
	BSD_ENETRESET,
	BSD_ECONNABORTED,
	BSD_ECONNRESET,
	BSD_ENOBUFS,
	BSD_EISCONN,
	BSD_ENOTONN,
	BSD_ESHUTDOWN,
	BSD_ETOOMANYREFS,
	BSD_ETIMEDOUT,
	BSD_ECONNREFUSED,
	BSD_ELOOP,
	BSD_ENAMETOOLONG,
	BSD_EHOSTDOWN,
	BSD_EHOSTUNREACH,
	BSD_ENOTEMPTY,
	BSD_EPROCLIM,
	BSD_EUSERS,
	BSD_EDQUOT,
	BSD_ESTALE,
	BSD_EREMOTE,
	BSD_ENOSTR,
	BSD_ETIME,
	BSD_ENOSR,
	BSD_ENOMSG,
	BSD_EBADMSG,
	BSD_IDRM,
	BSD_EDEADLK,
	BSD_ENOLCK,
	BSD_ENONET,
	BSD_ERREMOTE,
	BSD_ENOLINK,
	BSD_EADV,
	BSD_ESRMNT,
	BSD_ECOMM,
	BSD_EPROTO,
	BSD_EMULTIHOP,
	BSD_EINVAL,    
	BSD_REMCHG,
	BSD_NOSYS,
	BSD_STRPIPE,
	BSD_EOVERFLOW,
	BSD_EBADFD,
	BSD_ECHRNG,
	BSD_EL2NSYNC,
	BSD_EL3HLT,
	BSD_EL3RST,
	BSD_NRNG,
	BSD_EUNATCH,
	BSD_ENOCSI,
	BSD_EL2HLT,
	BSD_EBADE,
	BSD_EBADR,
	BSD_EXFULL,
	BSD_ENOANO,
	BSD_EBADRQC,
	BSD_EBADSLT,
	BSD_EDEADLOCK,
	BSD_EBFONT,
	BSD_ELIBEXEC,
	BSD_ENODATA,
	BSD_ELIBBAD,
	BSD_ENOPKG,
	BSD_ELIBACC,
	BSD_ENOTUNIQ,
	BSD_ERESTART,
	BSD_EUCLEAN,
	BSD_ENOTNAM,
	BSD_ENAVAIL,
	BSD_EISNAM,
	BSD_EREMOTEIO,
	BSD_EILSEQ,
	BSD_ELIBMAX,
	BSD_ELIBSCN,
};
