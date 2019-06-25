/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 Stanislav Sedov <stas@FreeBSD.org>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 */

#ifndef CEPH_COMPAT_H
#define CEPH_COMPAT_H

#include "acconfig.h"

#if defined(__linux__)
#define PROCPREFIX
#endif

#if defined(__FreeBSD__)

// FreeBSD supports Linux procfs with its compatibility module
// And all compatibility stuff is standard mounted on this 
#define PROCPREFIX "/compat/linux"

/* Make sure that ENODATA is defined in the correct way */
#ifndef ENODATA
#define	ENODATA	ENOATTR
#else
#if (ENODATA == 9919)
// #warning ENODATA already defined to be 9919, redefining to fix
// Silencing this warning because it fires at all files where compat.h
// is included after boost files.
//
// This value stems from the definition in the boost library
// And when this case occurs it is due to the fact that boost files
// are included before this file. Redefinition might not help in this
// case since already parsed code has evaluated to the wrong value.
// This would warrrant for d definition that would actually be evaluated
// at the location of usage and report a possible confict.
// This is left up to a future improvement
#elif (ENODATA != 87)
#warning ENODATA already defined to a value different from 87 (ENOATRR), refining to fix
#endif
#undef ENODATA
#define ENODATA ENOATTR
#endif
#ifndef MSG_MORE
#define	MSG_MORE 0
#endif

#ifndef O_DSYNC
#define O_DSYNC O_SYNC
#endif

// Fix clock accuracy
#if !defined(CLOCK_MONOTONIC_COARSE)
#if defined(CLOCK_MONOTONIC_FAST)
#define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC_FAST
#else
#define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC
#endif
#endif
#if !defined(CLOCK_REALTIME_COARSE)
#if defined(CLOCK_REALTIME_FAST)
#define CLOCK_REALTIME_COARSE CLOCK_REALTIME_FAST
#else
#define CLOCK_REALTIME_COARSE CLOCK_REALTIME
#endif
#endif

/* And include the extra required include file */
#include <pthread_np.h>

#endif /* !__FreeBSD__ */

#if defined(__APPLE__) || defined(__FreeBSD__)
/* get PATH_MAX */
#include <limits.h>

#ifndef EREMOTEIO
#define EREMOTEIO 121
#endif

#ifndef HOST_NAME_MAX
#ifdef MAXHOSTNAMELEN 
#define HOST_NAME_MAX MAXHOSTNAMELEN 
#else
#define HOST_NAME_MAX 255
#endif
#endif

#endif /* __APPLE__ */

/* O_LARGEFILE is not defined/required on OSX/FreeBSD */
#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

/* Could be relevant for other platforms */
#ifndef ERESTART
#define ERESTART EINTR
#endif

#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(expression) ({     \
  __typeof(expression) __result;              \
  do {                                        \
    __result = (expression);                  \
  } while (__result == -1 && errno == EINTR); \
  __result; })
#endif

#ifdef __cplusplus
# define VOID_TEMP_FAILURE_RETRY(expression) \
   static_cast<void>(TEMP_FAILURE_RETRY(expression))
#else
# define VOID_TEMP_FAILURE_RETRY(expression) \
   do { (void)TEMP_FAILURE_RETRY(expression); } while (0)
#endif

#if defined(__FreeBSD__) || defined(__APPLE__)
#define lseek64(fd, offset, whence) lseek(fd, offset, whence)
#endif

#if defined(__sun) || defined(_AIX)
#define LOG_AUTHPRIV    (10<<3)
#define LOG_FTP         (11<<3)
#define __STRING(x)     "x"
#define IFTODT(mode)   (((mode) & 0170000) >> 12)
#endif

#if defined(_AIX)
#define MSG_DONTWAIT MSG_NONBLOCK
#endif

#if defined(HAVE_PTHREAD_SETNAME_NP)
  #if defined(__APPLE__)
    #define ceph_pthread_setname(thread, name) ({ \
      int __result = 0;                         \
      if (thread == pthread_self())             \
        __result = pthread_setname_np(name);    \
      __result; })
  #else
    #define ceph_pthread_setname pthread_setname_np
  #endif
#elif defined(HAVE_PTHREAD_SET_NAME_NP)
  /* Fix a small name diff */
  #define ceph_pthread_setname pthread_set_name_np
#else
  /* compiler warning free success noop */
  #define ceph_pthread_setname(thread, name) ({ \
    int __i = 0;                              \
    __i; })
#endif

#if defined(HAVE_PTHREAD_GETNAME_NP)
  #define ceph_pthread_getname pthread_getname_np
#else
  /* compiler warning free success noop */
  #define ceph_pthread_getname(thread, name, len) ({ \
    if (name != NULL)                              \
      *name = '\0';                                \
    0; })
#endif

#endif /* !CEPH_COMPAT_H */
