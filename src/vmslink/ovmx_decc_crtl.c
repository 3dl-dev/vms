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
#include <stdarg.h>
#include <stdio.h>

/* Canonical VMS image-activation context — single source of truth (INV-LEDGER):
 * dsc$descriptor_s (image_file_desc, arg 4), plus ovmx_imghdr (arg 3) and
 * ovmx_cli_util (arg 2). Defined once in the freestanding IMGACT include (pulls
 * only <stdint.h>); IMGACT and decc$main must agree on these, so decc$main binds
 * the same header rather than a private copy. -Isrc/imgact/include is added to
 * the CRTL compile in mk_decc_shr.sh and run_decc_shr.sh. (vms-8c8) */
#include "ovmx_activation.h"

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
 * so _malloc32 MUST return a low address whose bit<31> is clear (a positive 32-bit
 * int that round-trips back to the same address on LP64). decc$main (below) DEPENDS
 * on this: it stores the argv/envp element pointers as 32-bit ints, so a truncated
 * 64-bit pointer would corrupt them — the value must be genuinely <4 GB.
 *
 * Two real ways to a <4 GB mapping:
 *   - x86_64: MAP_32BIT gives a low-2 GB mapping directly (per call).
 *   - any arch (incl. aarch64, where MAP_32BIT does not exist): reserve one low
 *     arena via mmap MAP_FIXED_NOREPLACE at a low hint and bump-allocate from it.
 *     A PLAIN low hint is ignored by the kernel (it returns a high address), so
 *     FIXED_NOREPLACE is required; we try a few hints in case one is occupied.
 * If neither yields a <4 GB region, _malloc32 returns 0 (honest failure) rather
 * than a truncated 64-bit pointer — the former is a clean NULL the caller can
 * check, the latter is silent corruption. (vms-954 closes the R1b-2a arm64 gap
 * that decc$main would otherwise inherit.) */

#ifndef MAP_32BIT
#define MAP_32BIT 0                    /* absent on non-x86_64                    */
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000   /* Linux 4.17+ value; alpine musl defines it */
#endif

/* One-time-reserved low (<4 GB) arena for the non-MAP_32BIT path, bump-allocated
 * under a lock. First-light sized; a port path that outgrows it grows the arena
 * (a real allocator refinement, not a correctness gap — the addresses it returns
 * are genuine <4 GB pointers today). */
static pthread_mutex_t p32_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char  *p32_base;
static unsigned long   p32_off, p32_cap;

static void *p32_arena_alloc(unsigned long size)
{
    pthread_mutex_lock(&p32_lock);
    if (!p32_base) {
        static const unsigned long hints[] = {
            0x40000000UL, 0x50000000UL, 0x60000000UL, 0x30000000UL
        };
        unsigned long cap = 4UL * 1024 * 1024;   /* 4 MiB first-light arena */
        for (unsigned i = 0; i < sizeof hints / sizeof hints[0] && !p32_base; i++) {
            void *p = mmap((void *)hints[i], (size_t)cap, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p == MAP_FAILED)
                continue;                          /* hint occupied: try the next */
            if ((unsigned long)p < 0x100000000UL) {
                p32_base = p; p32_cap = cap; p32_off = 0;
            } else {
                munmap(p, (size_t)cap);            /* honored elsewhere, not <4 GB */
            }
        }
    }
    void *r = 0;
    if (p32_base) {
        unsigned long a = (p32_off + 15UL) & ~15UL;   /* 16-byte align */
        if (a + size <= p32_cap) { r = p32_base + a; p32_off = a + size; }
    }
    pthread_mutex_unlock(&p32_lock);
    return r;
}

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
    void *q = p32_arena_alloc((unsigned long)size);
    if (q) {
        unsigned long a = (unsigned long)q;   /* arena is at 0x4000_0000+: bit<31> clear */
        if (a <= 0x7fffffffUL)
            return (int)a;
    }
    return 0;   /* honest failure: no <4 GB region (NOT a truncated 64-bit pointer) */
}

void *_malloc64(unsigned long size)
{
    return malloc((size_t)size);
}

/* -------------------------------------------------- decc$malloc / decc$_malloc64
 * (vms-981) The alpha-dec-vms GCC port maps the source `malloc` through the CRTL
 * name table (gcc/config/vms/vms-crtlmap.map: "malloc  64 MALLOC"; the decoration
 * in gcc/config/vms/vms.cc) to one of a POINTER-SIZE-selected pair, and the crt0
 * itself (libgcc/config/vms/vms-ucrt0.c) references BOTH:
 *
 *   decc$malloc     - the 32-bit-pointer malloc: the port emits it as the
 *                     translation of the RTL-internal `_malloc32` name (vms.cc,
 *                     the "_X32 -> X" rule), so its result MUST be addressable by
 *                     a 32-bit pointer (< 4 GB). The port crt0 builds the widened
 *                     argv/envp ARRAY here (a small allocation living in the low
 *                     4 GB, exactly as DEC C's default-heap malloc places it).
 *   decc$_malloc64  - the 64-bit-pointer malloc: in -mpointer-size=64 the port
 *                     maps the source `malloc` to this (vms.cc, "X -> _X64 if
 *                     long"), the full 64-bit-addressable heap. app.c's plain
 *                     malloc(3) lowers to this.
 *
 * These are the DECORATED malloc-family entries the R1b-1 alias vector
 * deliberately did NOT generate (they are not plain decc$<name>/<name> musl
 * aliases; decc$malloc's < 4 GB contract is not something musl's malloc honors).
 * Real bridges onto the already-genuine OVMX allocators, correct void* signatures
 * (NOT the bare-int _malloc32 ABI): decc$malloc -> _malloc32 (the < 4 GB arena /
 * MAP_32BIT path above), decc$_malloc64 -> _malloc64 (musl malloc). Grounded to
 * the GPL port source + the VSI OpenVMS C RTL Reference Manual (Rule 8). */

void *ovmx_decc_malloc(size_t n)   __asm__("decc$malloc");
void *ovmx_decc_malloc(size_t n)
{
    if (n > 0x7fffffffUL)              /* _malloc32 takes a (positive) int size   */
        return NULL;                   /* honest failure, never a truncated size  */
    int a = _malloc32((int)n);
    if (a <= 0)                        /* no < 4 GB region (or n==0): honest NULL  */
        return NULL;
    return (void *)(unsigned long)(unsigned int)a;   /* genuine < 4 GB pointer     */
}

void *ovmx_decc_malloc64(size_t n) __asm__("decc$_malloc64");
void *ovmx_decc_malloc64(size_t n)
{
    return _malloc64((unsigned long)n);   /* full 64-bit-addressable heap (malloc) */
}

/* ------------------------------------------------------------------ decc$tprintf
 * (vms-981) The port maps the source `printf` (vms-crtlmap.map: "printf  FLOAT64
 * FLOAT128") through vms.cc's float-model decoration: FLOAT64 prepends 't',
 * FLOAT128 prepends 'x' ONLY when LONG_DOUBLE_TYPE_SIZE == 128. The port emits
 * `decc$tprintf` (a 't', no 'x'), so this target's long double is 64-bit and the
 * name selects the DEC C RTL's IEEE T_FLOAT variant of printf-to-stdout: %f/%g/%e
 * arguments are IEEE-754 binary64 (as opposed to the VAX D/G-float variants
 * decc$printf / decc$gprintf select). OVMX/musl doubles ARE IEEE-754 binary64, so
 * the T_FLOAT model is exactly musl's native printf model — a faithful forward to
 * vprintf (== vfprintf(stdout, ...)), NOT a reinterpretation. Real formatted
 * output to stdout; varargs via va_list; returns the char count musl's vprintf
 * returns (negative on output error), the C-standard printf return real OpenVMS
 * DECC$SHR also yields. Grounded to the GPL port source + the VSI OpenVMS C RTL
 * Reference Manual ("Floating-Point Support", the g/t/x model prefixes), Rule 8. */

int ovmx_decc_tprintf(const char *fmt, ...) __asm__("decc$tprintf");
int ovmx_decc_tprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);          /* real formatted output to stdout          */
    va_end(ap);
    return r;                          /* chars written (or <0 on error)           */
}

/* ------------------------------------------------------------------- decc$main
 * The DEC C RTL image-startup routine. The alpha-dec-vms crt0
 * (libgcc/config/vms/vms-ucrt0.c) transfers to __main, which forwards the six-arg
 * VMS image-activation context verbatim and expects decc$main to PRODUCE
 * argc/argv/envp:
 *
 *   decc$main(progxfer, cli_util, imghdr, image_file_desc,
 *             linkflag, cliflag, &argc, &argv, &envp);
 *
 * argc/argv/envp are 32-bit `int` out-params (vms-ucrt0.c declares them `int`):
 *   *argc = the argument count
 *   *argv = a 32-bit-addressable pointer to a NULL-terminated array of 32-bit
 *           string pointers
 *   *envp = likewise for the environment
 * The crt0 casts *argv and *envp back up to 64-bit on LP64, so every pointer
 * they hold must be genuinely <4 GB — _malloc32 for the arrays AND the strings.
 *
 * Grounded to the crt0 contract + the IMGACT activation-context design
 * (docs/design-imgact-vms-activation-context.md, vms-f60d) co-designed with the
 * conductor. cli_util / imghdr are OVMX-original structs whose minimal shapes the
 * two halves agreed (§4b): cli_util = { version; get_command_line callback },
 * imghdr = { version; flags; image_base }.
 *
 * cliflag == 0 (non-CLI, the RUN path — FIRST-LIGHT SCOPE): argc = 1, argv[0] =
 * the image file spec from image_file_desc (a VMS string descriptor), envp = empty
 * (a single NULL). A VMS non-CLI image has no Unix environment, so an empty envp
 * is the faithful result — IMGACT does not supply one (envp is not among the six
 * args; decc$main produces it).
 *
 * cliflag != 0 (CLI): the command line is fetched via cli_util->get_command_line
 * and split into argv. The SPLITTING RULES are DEC C's (quoting/whitespace/case),
 * an empirical VMS behavior to be grounded against lab-Alpha before it is
 * authoritative — so it is a SEPARATE grounded rung (vms-954 follow-up), NOT
 * invented here. Until then a CLI activation is handled by the non-CLI production
 * (argv[0] = image name, no split args): a valid argv is still produced (never a
 * fake or a crash), just not yet the split command line. This is documented, not
 * silent. */

/* The VMS string descriptor (dsc$descriptor_s) now comes from ovmx_activation.h
 * (included above) — the canonical definition IMGACT and decc$main share. */

/* Cast a _malloc32 return (a non-negative 32-bit address) back to a pointer. */
static inline void *p32_to_ptr(int a32)
{
    return (void *)(unsigned long)(unsigned int)a32;
}

/* decc$main's emitted symbol carries the VMS `$`; the C name is ovmx_decc_main. */
void ovmx_decc_main(void *progxfer, void *cli_util, void *imghdr,
                    void *image_file_desc, unsigned int linkflag,
                    unsigned int cliflag, int *argc, int *argv, int *envp)
                    __asm__("decc$main");

void ovmx_decc_main(void *progxfer, void *cli_util, void *imghdr,
                    void *image_file_desc, unsigned int linkflag,
                    unsigned int cliflag, int *argc, int *argv, int *envp)
{
    (void)progxfer;   /* the transfer address — not consumed here      */
    (void)imghdr;     /* {version,flags,image_base}; flags unused first-light */
    (void)linkflag;   /* passed 0; decc$main does not branch on it (§4b.8) */
    (void)cli_util;   /* CLI path is a separate grounded rung (see header) */
    (void)cliflag;

    /* --- Non-CLI production: argv = [ image-file-spec ], envp = [] -------------
     * argv[0] is the image file spec, taken from the image_file_desc descriptor. */
    const char  *name = "";
    unsigned int nlen = 0;
    if (image_file_desc) {
        const struct dsc$descriptor_s *d = image_file_desc;
        if (d->dsc$a_pointer) { name = d->dsc$a_pointer; nlen = d->dsc$w_length; }
    }

    /* Copy the (length-counted, non-NUL-terminated) name into a <4 GB, NUL-
     * terminated buffer so the 32-bit argv[0] pointer addresses it. */
    int  name32 = _malloc32((int)(nlen + 1));
    if (name32) {
        char *ns = p32_to_ptr(name32);
        for (unsigned i = 0; i < nlen; i++) ns[i] = name[i];
        ns[nlen] = '\0';
    }

    /* argv: [ name32, 0 ] as 32-bit pointers. */
    int  av32 = _malloc32((int)(sizeof(int) * 2));
    if (av32) {
        int *av = p32_to_ptr(av32);
        av[0] = name32;
        av[1] = 0;
    }

    /* envp: empty (single NULL terminator). */
    int  ev32 = _malloc32((int)(sizeof(int) * 1));
    if (ev32) {
        int *ev = p32_to_ptr(ev32);
        ev[0] = 0;
    }

    if (argc) *argc = 1;
    if (argv) *argv = av32;
    if (envp) *envp = ev32;
}
