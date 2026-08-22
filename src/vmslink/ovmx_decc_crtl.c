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

/* VMS string descriptor (dsc$descriptor_s) — public, VMS-authentic layout. */
struct ovmx_dsc_s {
    unsigned short dsc_w_length;    /* string length in bytes                    */
    unsigned char  dsc_b_dtype;     /* DSC$K_DTYPE_T                             */
    unsigned char  dsc_b_class;     /* DSC$K_CLASS_S                            */
    char          *dsc_a_pointer;   /* -> the (non-NUL-terminated) string        */
};

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
        const struct ovmx_dsc_s *d = image_file_desc;
        if (d->dsc_a_pointer) { name = d->dsc_a_pointer; nlen = d->dsc_w_length; }
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
