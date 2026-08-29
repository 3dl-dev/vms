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

/* vms-430 PC discriminator (DIAGNOSTIC, not for merge): the SIGSEGV handler
 * below reads the Alpha faulting PC from the named sigcontext field, which the
 * alpha-dec-vms musl signal.h only exposes under a feature macro (the mcontext_t
 * with sc_pc is gated on _GNU_SOURCE||_BSD_SOURCE). Additive; keep it first. */
#define _GNU_SOURCE 1

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

/* ALPHA (vms-864): unlike _malloc32 (no musl equivalent -- genuinely needed,
 * see the wrapper note below), a plain `_malloc64` DEFINITION here is ALSO
 * auto-decorated by the alpha-dec-vms cc1 to `decc$_malloc64` (the same
 * crtlmap "_X64 -> X" rule) -- and musl-alpha's OWN malloc.c, compiled
 * -mpointer-size=64, decorates its plain `malloc` definition to that EXACT
 * same name (the "malloc 64 MALLOC" pointer-size rule). Whole-archiving both
 * this object and libc.a into DECC$SHR then gives `decc$_malloc64` TWO
 * definitions across two archive members -- confirmed empirically: LINK.EXE's
 * whole-archive resolver does not flag it (a real link.c gap, not exercised
 * here), and the requested --symbol-vector simply lists the name twice
 * (harmless, but a genuine duplicate-definition hazard, not a clean state).
 * musl-alpha's decc$_malloc64 is the correct, sufficient producer (this
 * function is a bare `return malloc(size)` forward -- literally the same
 * call musl's own malloc already IS), so on alpha this definition is
 * unneeded as well as duplicating; __VMS__ selects it out. x86_64/aarch64
 * (no musl decc$ auto-decoration) still need it, exported as the plain
 * `_malloc64` universal (mk_decc_shr.sh generic tail) -- unchanged. */
#ifndef __VMS__
void *_malloc64(unsigned long size)
{
    return malloc((size_t)size);
}
#endif /* !__VMS__ */

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

/* ALPHA (vms-864): the alpha-dec-vms cc1 auto-decorates the CRTL surface at
 * codegen (gcc/config/vms/vms.cc's crtlmap) -- transparently, at every
 * definition AND call site, keyed off the plain SOURCE identifier. Confirmed
 * by direct compile: a definition literally named `_malloc32` (above) is
 * ALREADY emitted as `decc$malloc` (the crtlmap "_X32 -> X" rule), and
 * `_malloc64` as `decc$_malloc64` ("_X64 -> X"). Defining ovmx_decc_malloc /
 * ovmx_decc_malloc64 as SEPARATE functions asm-relabeled to those same
 * decorated names, on THAT compiler, is a genuine duplicate definition of the
 * same linker symbol within one translation unit (confirmed: GAS rejects the
 * emitted .s with "symbol 'decc$malloc..en' is already defined") -- not a
 * theoretical conflict, an actual assembler error. So on alpha these wrapper
 * functions are unneeded AND unbuildable; __VMS__ (predefined only by the
 * alpha-dec-vms port compiler, never by a host x86_64/aarch64 gcc/musl-gcc)
 * selects them out. This does not omit functionality (Rule: adapt minimally,
 * never #ifdef away something needed) -- decc$malloc/decc$_malloc64 are still
 * produced, by the port's own auto-decoration of the SAME _malloc32/_malloc64
 * definitions above, just without a redundant second copy. On x86_64/aarch64
 * (no such auto-decoration exists) these wrappers remain the only source of
 * decc$malloc/decc$_malloc64, unchanged. */
#ifndef __VMS__
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
#endif /* !__VMS__ */

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
 * Reference Manual ("Floating-Point Support", the g/t/x model prefixes), Rule 8.
 *
 * ALPHA (vms-864): the SAME crtlmap "printf FLOAT64" rule that motivates this
 * wrapper on x86_64/aarch64 fires INSIDE the alpha-dec-vms cc1 itself when it
 * compiles musl-alpha's own printf.c -- musl's `printf` definition is already
 * auto-decorated straight to `decc$tprintf` (confirmed in the plain libc.a +
 * libgcc.a decc$ enumeration, mk_decc_shr.sh's ALPHA branch: sv#... decc$tprintf,
 * no ovmx_decc_crtl.c involvement). Defining ovmx_decc_tprintf here TOO would be
 * a second, cross-object definition of the same decc$tprintf universal (libc.a's
 * printf.o vs. this file) once both are whole-archived into DECC$SHR -- guarded
 * out under __VMS__ for the same reason as decc$malloc/decc$_malloc64 above: not
 * omitted functionality, the port's own codegen already produces it from musl's
 * real printf. */
#ifndef __VMS__
int ovmx_decc_tprintf(const char *fmt, ...) __asm__("decc$tprintf");
int ovmx_decc_tprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);          /* real formatted output to stdout          */
    va_end(ap);
    return r;                          /* chars written (or <0 on error)           */
}
#endif /* !__VMS__ */

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

/* ============================ vms-430 PC discriminator =====================
 * DIAGNOSTIC (NOT for merge). A SIGSEGV/SIGBUS handler that pins the faulting
 * PC + si_addr of the alpha joint-e2e CRTL/RMS crash and then _exit(42) so the
 * fault cannot recurse. Installed at the TOP of decc$main (below) — which is
 * proven-reached on the rail (the DECCMAIN-DISC marker) and runs BEFORE the
 * crt0->main handoff and before main's body, so it covers BOTH sub-hypotheses:
 * a handoff crash (jsr main) AND an in-main CRTL-body crash. Lands in
 * DECC$SHR.EXE. write()/_exit() here are INTRA-DECC$SHR musl calls (resolved
 * inside the shareable, never imports), so the handler is self-sufficient no
 * matter what the joint image links against.
 *
 * Alpha faulting-PC field: on alpha-dec-vms musl, mcontext_t IS struct
 * sigcontext and the trap PC is uc_mcontext.sc_pc (a `long`). Cited source:
 * tools/cross-alpha-vms/musl-arch/arch/alpha-dec-vms/bits/signal.h. On that
 * same header SA_SIGINFO is 0x40 (the Alpha/OSF value, NOT the generic 0x04) —
 * using the <signal.h> macro (not a literal) picks up the right bit. Guarded
 * __alpha__-only so x86_64/aarch64 DECC$SHR builds are byte-unchanged. */
#if defined(__alpha__)
#include <signal.h>
#include <unistd.h>

/* Async-signal-safe 16-digit hex, MSB first, into p[0..15]. */
static void ovmx_disc_puthex(char *p, unsigned long v)
{
    for (int i = 15; i >= 0; i--) {
        unsigned d = (unsigned)(v & 0xfUL);
        p[i] = (char)(d < 10 ? ('0' + d) : ('a' + (d - 10)));
        v >>= 4;
    }
}

/* Async-signal-safe pointer-append builders (no libc), for the SIGILL-DISC
 * line whose signo/word_at_pc fields are variable width. */
static char *ovmx_disc_apphex(char *p, unsigned long v, int ndig)
{
    for (int i = ndig - 1; i >= 0; i--) {
        unsigned d = (unsigned)((v >> (i * 4)) & 0xfUL);
        *p++ = (char)(d < 10 ? ('0' + d) : ('a' + (d - 10)));
    }
    return p;
}
static char *ovmx_disc_appstr(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *ovmx_disc_appdec(char *p, unsigned long v)
{
    char t[24]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) *p++ = t[--n];
    return p;
}

static void ovmx_disc_segv(int sig, siginfo_t *info, void *uctx)
{
    unsigned long addr = info ? (unsigned long)info->si_addr : 0UL;
    unsigned long pc = 0UL;
    if (uctx)
        pc = (unsigned long)((ucontext_t *)uctx)->uc_mcontext.sc_pc;

    /* Legacy SIGSEGV-DISC line preserved for SIGSEGV/SIGBUS (cross-ref against
     * readelf on JOINT_E2E.EXE / DECC$SHR.EXE). Fixed-width overwrite template:
     *  idx 24 = first si_addr digit; idx 46 = first pc digit. */
    if (sig == SIGSEGV || sig == SIGBUS) {
        char line[] =
            "SIGSEGV-DISC: si_addr=0x0000000000000000 pc=0x0000000000000000\n";
        ovmx_disc_puthex(line + 24, addr);
        ovmx_disc_puthex(line + 46, pc);
        (void)write(2, line, sizeof(line) - 1);
    }

    /* SIGILL/SIGABRT/SIGTRAP (and, self-identifying via signo, every signal):
     * additionally read the OPCODE WORD AT THE FAULTING PC. word_at_pc == 0
     * -> jumped into a zero region (bad linkage cell / descriptor); a real
     * opcode -> genuine illegal instruction at that address. pc <= a page is
     * treated as unreadable (word stays 0) so a null pc cannot re-fault here. */
    {
        char b[128];
        char *p = b;
        unsigned int word = 0;
        if (pc > 4096UL)
            word = *(volatile unsigned int *)pc;
        p = ovmx_disc_appstr(p, "SIGILL-DISC: signo=");
        p = ovmx_disc_appdec(p, (unsigned long)sig);
        p = ovmx_disc_appstr(p, " si_addr=0x");
        p = ovmx_disc_apphex(p, addr, 16);
        p = ovmx_disc_appstr(p, " pc=0x");
        p = ovmx_disc_apphex(p, pc, 16);
        p = ovmx_disc_appstr(p, " word_at_pc=0x");
        p = ovmx_disc_apphex(p, (unsigned long)word, 8);
        *p++ = '\n';
        (void)write(2, b, (unsigned long)(p - b));
    }
    _exit(42);
}

static void ovmx_disc_install_segv(void)
{
    struct sigaction sa;
    char *z = (char *)&sa;                 /* zero without a memset dependency */
    for (unsigned i = 0; i < sizeof sa; i++)
        z[i] = 0;
    sa.sa_sigaction = ovmx_disc_segv;
    sa.sa_flags = SA_SIGINFO;              /* alpha 0x40, from bits/signal.h    */
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS,  &sa, 0);
    sigaction(SIGILL,  &sa, 0);            /* the CRTL/RMS decc$_memset64 fault  */
    sigaction(SIGABRT, &sa, 0);
    sigaction(SIGTRAP, &sa, 0);
}
#endif /* __alpha__ */

/* decc$main's emitted symbol carries the VMS `$`; the C name is ovmx_decc_main. */
void ovmx_decc_main(void *progxfer, void *cli_util, void *imghdr,
                    void *image_file_desc, unsigned int linkflag,
                    unsigned int cliflag, int *argc, int *argv, int *envp)
                    __asm__("decc$main");

void ovmx_decc_main(void *progxfer, void *cli_util, void *imghdr,
                    void *image_file_desc, unsigned int linkflag,
                    unsigned int cliflag, int *argc, int *argv, int *envp)
{
#if defined(__alpha__)
    ovmx_disc_install_segv();   /* vms-430 PC discriminator (diagnostic) */
#endif
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
