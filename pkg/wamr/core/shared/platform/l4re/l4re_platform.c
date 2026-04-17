/*
 * L4Re platform implementation for WAMR.
 *
 * Implements the platform_api_vmcore.h interface for the L4Re microkernel OS.
 *
 * Design constraints:
 *   - Single-threaded: mutex operations are no-ops.
 *   - No WASI: file / socket APIs are not implemented.
 *   - Interpreter mode only: os_mmap() is backed by malloc; no executable
 *     memory permissions are required.
 *   - Time: uses the L4 Kernel Interface Page (KIP) clock.
 */

#include "platform_api_vmcore.h"

#include <l4/re/env.h>  /* l4re_kip() */
#include <l4/sys/kip.h> /* l4_kip_clock() */

/* Stack boundary recorded at init time (used by os_thread_get_stack_boundary) */
static uint8 *_stack_low = NULL;

/* ---- Lifecycle ---- */

int
bh_platform_init(void)
{
    volatile uint8 probe;
    /* Estimate stack bottom: 64 KB below current frame */
    _stack_low = (uint8 *)&probe - (64 * 1024);
    return 0;
}

void
bh_platform_destroy(void)
{}

/* ---- I/O ---- */

int
os_printf(const char *format, ...)
{
    int ret;
    va_list ap;
    va_start(ap, format);
    ret = vprintf(format, ap);
    va_end(ap);
    return ret;
}

int
os_vprintf(const char *format, va_list ap)
{
    return vprintf(format, ap);
}

/* ---- Time ---- */

uint64
os_time_get_boot_us(void)
{
    l4_kernel_info_t const *kip = l4re_kip();
    if (!kip)
        return 0;
    /* l4_kip_clock() returns microseconds since boot */
    return (uint64)l4_kip_clock(kip);
}

uint64
os_time_thread_cputime_us(void)
{
    return os_time_get_boot_us();
}

/* ---- Thread identity ---- */

korp_tid
os_self_thread(void)
{
    return (korp_tid)1; /* single-threaded: always "thread 1" */
}

uint8 *
os_thread_get_stack_boundary(void)
{
    return _stack_low;
}

void
os_thread_jit_write_protect_np(bool enabled)
{
    (void)enabled; /* not applicable on L4Re ARM */
}

/* ---- Mutex (single-threaded stubs) ---- */

int
os_mutex_init(korp_mutex *mutex)
{
    mutex->locked = 0;
    return 0;
}

int
os_mutex_destroy(korp_mutex *mutex)
{
    (void)mutex;
    return 0;
}

int
os_mutex_lock(korp_mutex *mutex)
{
    mutex->locked = 1;
    return 0;
}

int
os_mutex_unlock(korp_mutex *mutex)
{
    mutex->locked = 0;
    return 0;
}

/* ---- Memory allocation ---- */

void *
os_malloc(unsigned size)
{
    return malloc(size);
}

void *
os_realloc(void *ptr, unsigned size)
{
    return realloc(ptr, size);
}

void
os_free(void *ptr)
{
    free(ptr);
}

/* ---- Memory mapping ----
 *
 * Interpreter mode never needs executable pages, so we back mmap with
 * malloc.  AOT support (if added later) would require real L4Re dataspace
 * allocation with execute rights.
 */

void *
os_mmap(void *hint, size_t size, int prot, int flags, os_file_handle file)
{
    (void)hint; (void)prot; (void)flags; (void)file;
    return malloc(size);
}

void
os_munmap(void *addr, size_t size)
{
    (void)size;
    free(addr);
}

int
os_mprotect(void *addr, size_t size, int prot)
{
    (void)addr; (void)size; (void)prot;
    return 0; /* no-op: malloc memory is always R/W */
}

void *
os_mremap(void *old_addr, size_t old_size, size_t new_size)
{
    /* Fall back to the generic allocate-copy-free path */
    return os_mremap_slow(old_addr, old_size, new_size);
}

/* ---- Cache maintenance ---- */

void
os_dcache_flush(void)
{}

void
os_icache_flush(void *start, size_t len)
{
    (void)start; (void)len;
    /* For interpreter mode no code is written to executable memory */
}
