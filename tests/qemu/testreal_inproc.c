/*
 * testreal_inproc.c - a REAL-image-class subject for the in-process activation
 * FLIP (vms-db2, docs/design-in-process-activation.md Part II §A.8 remainder
 * gap 2 + the flip). NOT a test suite (its name matches no test_*.c glob, so
 * neither the Dockerfile recipes nor init.sh run it as one). It is the SUBJECT
 * tests/qemu/test_syssvc_imgact_realimg.c activates in DCL's own process.
 *
 * HOW IT DIFFERS FROM TESTIMG.EXE (increment iv's subject). TESTIMG is entered
 * through the OVMX (a0,a1) function-call ABI and imports nothing. TESTREAL is a
 * REAL image in the two senses the design names (§A.8 remainder): it is entered
 * through the SysV auxv `_start` ABI (imgact_activate builds an argc/argv/envp/
 * auxv stack and jumps to _start, which reads it exactly as a kernel-launched
 * static-PIE would), and it reaches the executive ONLY by binding its .vms$imp
 * universal-symbol imports to the ALREADY-RESIDENT producer (imgact_prodreg) --
 * the same shareable DCL holds -- never a private copy. It ends by calling the
 * imported SYS$EXIT (imgact_image_exit), which RETURNS control to DCL in the
 * same process instead of terminating it. The flip is: dcl_activate_image now
 * runs this class IN-PROCESS (same PID, no fork) rather than fork()+execve().
 *
 * WHAT IT DOES (all through resident imports, to make the flows-back genuine):
 *   1. Writes a banner to fd 1 (raw write) so the suite sees it RAN.
 *   2. Reads argc off the constructed auxv stack and returns it, proving the
 *      SysV entry ABI reached _start with a correct stack.
 *   3. Calls the resident vms_kif_setef(REAL_EFN): a process-permanent event
 *      flag set BY the in-process image on the ONE process's executive PCB. DCL
 *      reads it back after rundown -- flows back, which Option B's per-child
 *      event flags never could (design line 144).
 *   4. Calls the resident tls_bump(REAL_TLS_DELTA): touches a __thread datum in
 *      the resident producer through the UNCHANGED thread pointer, proving the
 *      image shares DCL's resident TLS, not a private copy (§A.8 gap 2b: TLS).
 *   5. Calls the resident imgact_image_exit(REAL_EXIT_CODE) -- SYS$EXIT -- which
 *      unwinds to imgact_activate and returns to DCL. It does NOT return here;
 *      the raw exit_group after it is only a backstop if the flip is not wired.
 *
 * Rule 8: ELF/auxv/.vms$imp mechanics are the public System V ABI + OVMX's own
 * symbol-vector format (ovmx_image.h); no VMS byte format is claimed. It carries
 * NO PT_INTERP (in-process activation IS the loader) and NO own PT_TLS (the
 * TLS-bearing-image case stays deferred), and builds with zero dynamic
 * relocations: its GOT cells and .vms$imp use link-time-constant symbol
 * differences (patch_off is `cell - __ehdr_start`, the image-relative offset),
 * and it reaches its own cells / entry PC-relatively.
 */
#include <stdint.h>

#include "imgact_activate.h"   /* IMGACT_NOTE_* / IMGACT_ABI_* */

/* Fixed work parameters (the suite knows these and checks for their effects). */
#define REAL_EFN         40   /* a LOCAL event flag (cluster 1, 32-63): no ascefc */
#define REAL_TLS_DELTA   7L
#define REAL_EXIT_CODE   0
#define REAL_BADABI_CODE 97   /* SYS$EXIT code when argc off the auxv stack is wrong */

/* Import vector indices into the resident producer's .vms$sv (the suite builds
 * the producer with exactly this order). */
#define IMP_EXIT   0
#define IMP_SETEF  1
#define IMP_TLSB   2

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
    register long x8 __asm__("x8") = 94;   /* __NR_exit_group */
    register long x0 __asm__("x0") = code;
    __asm__ volatile("svc #0" : : "r"(x8), "r"(x0) : "memory");
    __builtin_unreachable();
}
#else
static long img_write(long fd, const void *buf, long n){(void)fd;(void)buf;(void)n;return -1;}
static void img_exit_group(long code){(void)code; for(;;){}}
#endif

/*
 * The OVMX in-process marker note. desc = IMGACT_ABI_MARKER | IMGACT_ABI_AUXV:
 * in-process-eligible AND entered through the SysV auxv _start ABI. That desc
 * bit is the whole difference from TESTIMG.EXE's note (desc = 1).
 */
__attribute__((used, aligned(4), section(".note.ovmx")))
static const struct {
    uint32_t n_namesz;
    uint32_t n_descsz;
    uint32_t n_type;
    char     name[8];
    uint32_t desc;
} real_ovmx_note = {
    5, 4, IMGACT_NOTE_TYPE, { 'O', 'V', 'M', 'X', 0, 0, 0, 0 },
    IMGACT_ABI_MARKER | IMGACT_ABI_AUXV
};

/*
 * The consumer's GOT cells + .vms$imp import table. Each patch_off is the GOT
 * cell's IMAGE-RELATIVE offset (imgact_bind_imports_resident writes the resolved
 * RESIDENT producer address to load_base + patch_off before _start runs). A
 * hand-assembled object cannot express "cell's vaddr" as an as-time constant
 * (`cell - __ehdr_start` needs a symbol the linker, not the assembler, defines),
 * so this pins .vms$got at a FIXED vaddr with -Wl,--section-start (see the
 * CMake link options) and records patch_off as the matching literal offsets --
 * a constant, so the image still carries ZERO dynamic relocations. The three
 * cells are at TESTREAL_GOT_VA + {0,8,16} in declaration order (.balign 8). */
#define TESTREAL_GOT_VA 0x200000
__asm__(
    ".pushsection .vms$got,\"aw\",@progbits\n"
    ".balign 8\n"
    "testreal_got0: .quad 0\n"   /* [IMP_EXIT]  imgact_image_exit */
    "testreal_got1: .quad 0\n"   /* [IMP_SETEF] vms_kif_setef     */
    "testreal_got2: .quad 0\n"   /* [IMP_TLSB]  tls_bump          */
    ".popsection\n"

    ".pushsection .vms$imp,\"a\",@progbits\n"
    ".balign 8\n"
    "testreal_imp:\n"
    "  .long 0x31504d49\n"                               /* OVMX_IMP_MAGIC */
    "  .long 3\n"                                        /* count          */
    "  .long testreal_imp_names - testreal_imp\n"        /* names_off      */
    "  .long testreal_imp_names_end - testreal_imp_names\n"/* names_size   */
    /* entry: producer_off(u32) sv_index(u32) patch_off(u64) req_major(u32) req_minor(u32) */
    "  .long 0\n  .long 0\n  .quad 0x200000\n  .long 1\n  .long 0\n"
    "  .long 0\n  .long 1\n  .quad 0x200008\n  .long 1\n  .long 0\n"
    "  .long 0\n  .long 2\n  .quad 0x200010\n  .long 1\n  .long 0\n"
    "testreal_imp_names:\n"
    "  .asciz \"LIBVMS$SHR.EXE\"\n"
    "testreal_imp_names_end:\n"
    ".popsection\n"
);

typedef void     (*exitfn)(int);
typedef uint32_t (*seteffn)(uint32_t);
typedef long     (*tlsbfn)(long);

/* Read a bound GOT cell PC-relatively (no dynamic reloc). */
static void *got_slot(int i)
{
#if defined(__x86_64__)
    void **g;
    __asm__("lea testreal_got0(%%rip), %0" : "=r"(g));
    return g[i];
#elif defined(__aarch64__)
    void **g;
    __asm__("adrp %0, testreal_got0\n\tadd %0, %0, :lo12:testreal_got0" : "=r"(g));
    return g[i];
#else
    (void)i; return 0;
#endif
}

/* The image body. argc comes from the constructed auxv stack (proves the ABI).
 * hidden visibility so real_start's `call testreal_main` is a direct PC-relative
 * call, not a PLT slot -- imgact refuses an image with a PLT (symbolic imports),
 * and this image has none: its only cross-image references are the .vms$imp GOT
 * cells it drives by hand. */
__attribute__((visibility("hidden"))) long testreal_main(long argc);
__attribute__((visibility("hidden"))) long testreal_main(long argc)
{
    static const char banner[] = "OVMX-REALIMG-RAN\n";
    img_write(1, banner, (long)(sizeof(banner) - 1));

    /* 2: the auxv stack must have delivered argc == 1 (the image name). If it
     * did not, imgact did NOT enter us through the SysV auxv ABI correctly --
     * report a distinct SYS$EXIT code and touch NOTHING (no event flag, no TLS),
     * so the suite's flows-back/TLS/exit-code proofs all depend on the stack
     * being built right. This makes the auxv entry load-bearing, not decorative. */
    if (argc != 1)
        ((exitfn)got_slot(IMP_EXIT))(REAL_BADABI_CODE);

    /* 3: a process-permanent event flag set through the resident executive. */
    ((seteffn)got_slot(IMP_SETEF))(REAL_EFN);
    /* 4: touch the resident producer's TLS through the unchanged TP. */
    ((tlsbfn)got_slot(IMP_TLSB))(REAL_TLS_DELTA);
    /* 5: SYS$EXIT -- returns to DCL in-process; does not come back here. */
    ((exitfn)got_slot(IMP_EXIT))(REAL_EXIT_CODE);

    /* Only reached if the flip is NOT wired (imgact_image_exit found no open
     * in-process activation and would have _exit()'d): terminate honestly rather
     * than run off the end. Encode argc so a stack-ABI bug is still visible. */
    img_exit_group(argc);
    return 0;
}

/*
 * _start for the auxv ABI. imgact_enter_auxv jumps here with rsp at the
 * constructed initial stack (rsp -> argc). Read argc, keep rsp 16-aligned for
 * the call, and enter the C body. It never returns (testreal_main exits).
 */
__asm__(
#if defined(__x86_64__)
    ".text\n.globl real_start\n.type real_start,@function\n"
    "real_start:\n"
    "  mov (%rsp), %rdi\n"      /* argc */
    "  call testreal_main\n"    /* rsp was 16-aligned; call -> callee sees +8 */
    "  ud2\n"
    ".size real_start,.-real_start\n"
#elif defined(__aarch64__)
    ".text\n.globl real_start\n.type real_start,@function\n"
    "real_start:\n"
    "  ldr x0, [sp]\n"          /* argc */
    "  bl testreal_main\n"
    "  brk #0\n"
    ".size real_start,.-real_start\n"
#endif
);
