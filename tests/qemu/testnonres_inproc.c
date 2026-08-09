/*
 * testnonres_inproc.c - a REAL-image consumer whose .vms$imp names a
 * NON-RESIDENT producer (TESTNONRES.EXE), the subject
 * tests/qemu/test_syssvc_imgact_nonres.c activates in DCL's own process
 * (vms-db2, docs/design-in-process-activation.md Part II §A.8 remainder item 1
 * -- the non-resident-producer sub-step + its flip). NOT a test suite (its name
 * matches no test_*.c glob, so init.sh never runs it as one).
 *
 * HOW IT DIFFERS FROM TESTREAL.EXE. TESTREAL imports only from the ALREADY-
 * RESIDENT producer (LIBVMS$SHR the process holds). TESTNONRES additionally
 * imports prod_bump from TESTPROD.EXE -- a producer DCL does NOT hold resident.
 * The activator therefore MAPS TESTPROD into DCL's process (imgact_map_producer)
 * and binds prod_bump to that mapped instance -- proving the design's §A.2.2
 * "map a shareable the process does not already hold, bind to the one instance"
 * for a case where the producer is genuinely absent, not merely resident.
 *
 * WHAT IT DOES (entered via the SysV auxv _start ABI, IMGACT_ABI_AUXV):
 *   1. Writes a banner to fd 1 so the suite sees it RAN.
 *   2. argc off the constructed auxv stack must be 1 (proves the entry ABI); if
 *      not, it SYS$EXITs a distinct bad-ABI code and touches the producer NOT at
 *      all, so the auxv entry is load-bearing.
 *   3. Calls prod_bump() through its bound GOT cell: reaches the MAPPED TESTPROD
 *      producer, increments its shared counter, and gets the new value back.
 *   4. SYS$EXIT(that value): returns to DCL in-process with the bump result as
 *      the image's exit code, so the suite reads WHICH counter value this run
 *      observed -- the whole shared-instance proof (a first run sees 1; after the
 *      suite mutates the mapped counter to 100, a second run sees 101, proving
 *      the SAME instance, not a per-consumer private copy).
 *
 * SYS$EXIT (imgact_image_exit) binds to the RESIDENT LIBVMS$SHR the suite
 * publishes; prod_bump binds to the NON-RESIDENT TESTPROD the activator maps.
 * Rule 8: ELF/auxv/.vms$imp/.vms$sv mechanics only; no VMS byte format claimed.
 * No PT_INTERP, no own PT_TLS, zero dynamic relocations (its GOT cells are pinned
 * with -Wl,--section-start and its patch_off literals are constants).
 */
#include <stdint.h>

#include "imgact_activate.h"   /* IMGACT_NOTE_* / IMGACT_ABI_* */

#define NONRES_BADABI_CODE 97  /* SYS$EXIT code if argc off the auxv stack wrong */

/* Import vector indices. */
#define IMP_EXIT   0           /* LIBVMS$SHR.EXE  .vms$sv[0] imgact_image_exit */
#define IMP_BUMP   1           /* TESTPROD.EXE    .vms$sv[0] prod_bump         */

/* ---- raw write(2) for the banner (needs no import) --------------------- */
#if defined(__x86_64__)
static long img_write(long fd, const void *buf, long n)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(1L), "D"(fd), "S"(buf), "d"(n)
                     : "rcx", "r11", "memory");
    return r;
}
static void img_exit_group(long code)
{
    __asm__ volatile("syscall" : : "a"(231L), "D"(code) : "memory");
    __builtin_unreachable();
}
#elif defined(__aarch64__)
static long img_write(long fd, const void *buf, long n)
{
    register long x8 __asm__("x8") = 64;
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = n;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}
static void img_exit_group(long code)
{
    register long x8 __asm__("x8") = 94;
    register long x0 __asm__("x0") = code;
    __asm__ volatile("svc #0" : : "r"(x8), "r"(x0) : "memory");
    __builtin_unreachable();
}
#else
static long img_write(long fd, const void *buf, long n){(void)fd;(void)buf;(void)n;return -1;}
static void img_exit_group(long code){(void)code; for(;;){}}
#endif

/* The OVMX in-process marker note: in-process-eligible AND SysV auxv _start. */
__attribute__((used, aligned(4), section(".note.ovmx")))
static const struct {
    uint32_t n_namesz;
    uint32_t n_descsz;
    uint32_t n_type;
    char     name[8];
    uint32_t desc;
} nonres_ovmx_note = {
    5, 4, IMGACT_NOTE_TYPE, { 'O', 'V', 'M', 'X', 0, 0, 0, 0 },
    IMGACT_ABI_MARKER | IMGACT_ABI_AUXV
};

/*
 * GOT cells + .vms$imp table. Two imports from TWO producers:
 *   [IMP_EXIT] imgact_image_exit  from "LIBVMS$SHR.EXE" (RESIDENT, published)
 *   [IMP_BUMP] prod_bump          from "TESTPROD.EXE"   (NON-RESIDENT, mapped)
 * .vms$got is pinned at 0x200000 (cells at +0,+8) so patch_off is a literal.
 */
#define NONRES_GOT_VA 0x200000
__asm__(
    ".pushsection .vms$got,\"aw\",@progbits\n"
    ".balign 8\n"
    "nonres_got0: .quad 0\n"   /* [IMP_EXIT] imgact_image_exit */
    "nonres_got1: .quad 0\n"   /* [IMP_BUMP] prod_bump         */
    ".popsection\n"

    ".pushsection .vms$imp,\"a\",@progbits\n"
    ".balign 8\n"
    "nonres_imp:\n"
    "  .long 0x31504d49\n"                                 /* OVMX_IMP_MAGIC */
    "  .long 2\n"                                          /* count          */
    "  .long nonres_imp_names - nonres_imp\n"              /* names_off      */
    "  .long nonres_imp_names_end - nonres_imp_names\n"    /* names_size     */
    /* entry: producer_off(u32) sv_index(u32) patch_off(u64) req_major req_minor */
    /* [IMP_EXIT] producer "LIBVMS$SHR.EXE" (off 0), sv idx 0, cell 0x200000 */
    "  .long 0\n  .long 0\n  .quad 0x200000\n  .long 1\n  .long 0\n"
    /* [IMP_BUMP] producer "TESTPROD.EXE" (off 15), sv idx 0, cell 0x200008 */
    "  .long 15\n  .long 0\n  .quad 0x200008\n  .long 1\n  .long 0\n"
    "nonres_imp_names:\n"
    "  .asciz \"LIBVMS$SHR.EXE\"\n"     /* offset 0  (15 bytes incl NUL) */
    "  .asciz \"TESTPROD.EXE\"\n"       /* offset 15                     */
    "nonres_imp_names_end:\n"
    ".popsection\n"
);

typedef void (*exitfn)(int);
typedef long (*bumpfn)(void);

/* Read a bound GOT cell PC-relatively (no dynamic reloc). */
static void *got_slot(int i)
{
#if defined(__x86_64__)
    void **g;
    __asm__("lea nonres_got0(%%rip), %0" : "=r"(g));
    return g[i];
#elif defined(__aarch64__)
    void **g;
    __asm__("adrp %0, nonres_got0\n\tadd %0, %0, :lo12:nonres_got0" : "=r"(g));
    return g[i];
#else
    (void)i; return 0;
#endif
}

__attribute__((visibility("hidden"))) long nonres_main(long argc);
__attribute__((visibility("hidden"))) long nonres_main(long argc)
{
    static const char banner[] = "OVMX-NONRES-RAN\n";
    img_write(1, banner, (long)(sizeof(banner) - 1));

    /* The auxv stack must have delivered argc == 1. If not, the SysV entry ABI
     * was mis-built: report a distinct SYS$EXIT code and touch the producer NOT
     * at all, so the shared-counter proof depends on a correct entry. */
    if (argc != 1)
        ((exitfn)got_slot(IMP_EXIT))(NONRES_BADABI_CODE);

    /* Reach the MAPPED non-resident producer through the bound GOT cell: bump
     * its shared counter and carry the new value out as the exit code. */
    long v = ((bumpfn)got_slot(IMP_BUMP))();
    ((exitfn)got_slot(IMP_EXIT))((int)v);

    /* Only reached if the flip is NOT wired (imgact_image_exit found no open
     * in-process activation): terminate honestly rather than run off the end. */
    img_exit_group(v);
    return 0;
}

/* _start for the auxv ABI. imgact_enter_auxv jumps here with rsp at the
 * constructed initial stack (rsp -> argc). */
__asm__(
#if defined(__x86_64__)
    ".text\n.globl nonres_start\n.type nonres_start,@function\n"
    "nonres_start:\n"
    "  mov (%rsp), %rdi\n"      /* argc */
    "  call nonres_main\n"
    "  ud2\n"
    ".size nonres_start,.-nonres_start\n"
#elif defined(__aarch64__)
    ".text\n.globl nonres_start\n.type nonres_start,@function\n"
    "nonres_start:\n"
    "  ldr x0, [sp]\n"          /* argc */
    "  bl nonres_main\n"
    "  brk #0\n"
    ".size nonres_start,.-nonres_start\n"
#endif
);
