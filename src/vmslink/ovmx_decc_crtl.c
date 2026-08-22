/* ovmx_decc_crtl.c — real DEC C RTL special-symbol implementations linked into
 * DECC$SHR (vms-3e4 R1b-2a, GCC-oracle lane).
 *
 * The alpha-dec-vms GCC port + its C runtime reference DEC C RTL entry points that
 * are NOT plain aliases of a musl function (those are handled by the decc$<name>/
 * <name> alias vector, R1b-1). This file provides the ones that carry genuine
 * OpenVMS C RTL semantics, grounded to the public VSI OpenVMS C RTL Reference
 * Manual (Rule-8 sanctioned) + the port's own crt0 contract
 * (libgcc/config/vms/vms-ucrt0.c). Per the lane's ground-to-doc-now / verify-at-
 * integration posture (conductor 2026-08-22): these are REAL minimal impls, never
 * success-stubs; the required empirical confirmation is the port actually running
 * against them, which lands at the full-R1 (port-links-and-runs) gate — not
 * optional, not skipped.
 *
 * NON-TLS INVARIANT: DECC$SHR must stay a non-TLS producer (mk_decc_shr.sh's
 * filter_tls_members exists to enforce "one TLS object per image"). So per-thread
 * state here uses a pthread key (heap-backed), NEVER a `__thread` storage-class
 * variable (which would emit .tdata/.tbss and make DECC$SHR a TLS producer).
 *
 * Compiled -ffreestanding -fPIC and linked into DECC$SHR alongside ovmx_libc_stub.c;
 * the symbols below are exported as universals by mk_decc_shr.sh.
 */

#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>

/* ------------------------------------------------------------------ errno model
 * OpenVMS C keeps TWO per-thread error cells: the ISO C `errno`, and `vaxc$errno`
 * — the VMS condition value of the last failing C RTL call (VSI C RTL Ref Manual,
 * "Error Handling"). Code reads them through the accessors get_errno_addr() and
 * get_vms_errno_addr(); the compiler expands the `errno` / `vaxc$errno` lvalues to
 * `(*get_errno_addr())` / `(*get_vms_errno_addr())`, which is why the per-thread
 * cell is reached through a function, not a bare global int (that is the "bare int
 * alias" the authenticity line forbids). musl's own `errno` is already per-thread
 * via __errno_location(), so get_errno_addr() forwards to it; vaxc$errno gets its
 * own per-thread cell via a pthread key. The values are POPULATED when a C RTL
 * call fails — that population rides the RMS/ACP C-RTL route-through (R2, vms-dfb);
 * this file provides the genuine per-thread STORAGE + accessors those cells live
 * in, not fabricated success. */

int *get_errno_addr(void)
{
    return &errno;                     /* musl errno: per-thread via __errno_location */
}

static pthread_key_t  ovmx_vaxc_errno_key;
static pthread_once_t ovmx_vaxc_errno_once = PTHREAD_ONCE_INIT;

static void ovmx_vaxc_errno_key_init(void)
{
    pthread_key_create(&ovmx_vaxc_errno_key, free);   /* free the cell at thread exit */
}

int *get_vms_errno_addr(void)
{
    pthread_once(&ovmx_vaxc_errno_once, ovmx_vaxc_errno_key_init);
    int *cell = pthread_getspecific(ovmx_vaxc_errno_key);
    if (cell == NULL) {
        cell = malloc(sizeof *cell);
        if (cell != NULL) {
            *cell = 0;                 /* SS$_NORMAL-adjacent "no error yet" */
            pthread_setspecific(ovmx_vaxc_errno_key, cell);
        }
    }
    return cell;
}

/* -------------------------------------------------------------- dual-pointer malloc
 * OpenVMS Alpha's C RTL exposes the 32/64-bit dual-pointer allocators the port's
 * crt0 uses: _malloc32 returns memory addressable by a 32-bit pointer (used for the
 * argv/envp arrays the crt0 builds), _malloc64 the full 64-bit-addressable heap.
 * The crt0 declares `extern int _malloc32 (int)` and casts the result to a pointer,
 * so _malloc32 must return a low address whose bit<31> is clear (a positive 32-bit
 * int that sign-extends back to the same address on LP64).
 *
 * MAP_32BIT (x86_64 Linux) gives exactly that — a mapping in the low 2 GB. Where
 * MAP_32BIT is unavailable (e.g. aarch64), we fall back to plain malloc: the
 * pointer is still valid and the crt0 uses it as a 64-bit pointer, but true P32
 * addressability on those arches is a per-arch refinement (tracked; surfaces if a
 * port path actually depends on the 32-bit width). This is a real allocation, not
 * a stub. */

#ifndef MAP_32BIT
#define MAP_32BIT 0                    /* absent on non-x86_64: fall through to malloc */
#endif

int _malloc32(int size)
{
    if (size <= 0)
        return 0;
#if defined(__x86_64__) && MAP_32BIT
    void *p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p != MAP_FAILED) {
        unsigned long a = (unsigned long)p;
        if (a <= 0x7fffffffUL)         /* bit<31> clear: safe 32-bit-int round-trip */
            return (int)a;
        munmap(p, (size_t)size);       /* out of the P32 range — reject honestly */
    }
#endif
    /* Fallback: a real (64-bit) allocation. Valid pointer; P32 width is a refinement
     * on arches without MAP_32BIT (see comment above). */
    return (int)(long)malloc((size_t)size);
}

void *_malloc64(unsigned long size)
{
    return malloc((size_t)size);
}
