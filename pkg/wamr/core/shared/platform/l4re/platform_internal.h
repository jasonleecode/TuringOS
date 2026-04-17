/*
 * L4Re platform internal definitions for WAMR.
 *
 * Single-threaded port: mutex/cond/sem are stub types.
 * No WASI, no JIT, no shared memory.
 */

#ifndef _PLATFORM_INTERNAL_H
#define _PLATFORM_INTERNAL_H

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BH_PLATFORM_L4RE

/* ---- Thread / sync types (stubs for single-threaded operation) ---- */

typedef unsigned long korp_tid;

typedef struct { int locked; } korp_mutex;
typedef struct { int waiters; } korp_cond;
typedef korp_tid korp_thread;
typedef struct { int count; } korp_rwlock;
typedef struct { int count; } korp_sem;

#define OS_THREAD_MUTEX_INITIALIZER { 0 }

/* __thread attribute — not needed in single-threaded mode, define as empty */
#define os_thread_local_attribute

/* ---- Stack / thread sizing ---- */

/* Native stack reserved for WASM applet threads */
#define BH_APPLET_PRESERVED_STACK_SIZE (8 * 1024)

/* Default thread priority (unused) */
#define BH_THREAD_DEFAULT_PRIORITY 0

/* ---- File / socket handle types (WASI disabled, stubs only) ---- */

typedef int os_file_handle;
typedef void *os_dir_stream;
typedef int os_raw_file_handle;

/* poll / select stubs (no socket / WASI support) */
typedef struct {
    int   fd;
    short events;
    short revents;
} os_poll_file_handle;

typedef unsigned int os_nfds_t;

typedef struct {
    long tv_sec;
    long tv_nsec;
} os_timespec;

static inline os_file_handle
os_get_invalid_handle(void)
{
    return -1;
}

/* ---- Page size ---- */

/* L4Re on ARMv7 uses 4 KB pages */
#define os_getpagesize() 4096

#ifdef __cplusplus
}
#endif

#endif /* _PLATFORM_INTERNAL_H */
