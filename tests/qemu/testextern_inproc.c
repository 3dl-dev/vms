/*
 * testextern_inproc.c - a genuinely EXTERNAL image subject for the in-process
 * activation flip (vms-db2, docs/design-in-process-activation.md Part II §A.8
 * remainder item 2: the PT_INTERP-bearing external-image class). NOT a test
 * suite (its name matches no test_*.c glob, so neither the Dockerfile recipes
 * nor init.sh run it as one). It is the SUBJECT test_syssvc_imgact_extern.c
 * activates in DCL's own process.
 *
 * HOW IT DIFFERS FROM TESTREAL.EXE (the previous flip's subject). TESTREAL is a
 * real image in the two senses the design named -- SysV auxv `_start` entry and
 * resident-bound .vms$imp imports -- but it carries NO PT_INTERP. A genuinely
 * external LINK.EXE executable DOES: LINK.EXE writes
 * "/vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE" into a PT_INTERP so the kernel/fork
 * path activates it by exec'ing IMGACT.EXE (src/vmslink/link.c IMGACT_INTERP,
 * §A.8). Before this flip, imgact_activate rejected ANY PT_INTERP outright and
 * dcl_activate_image forked. TESTEXTERN is TESTREAL plus that PT_INTERP: the
 * defining trait of a real linker-produced executable. The flip is that
 * imgact_activate now accepts a PT_INTERP that names the OVMX loader (in-process
 * activation IS that loader) and runs the image IN DCL's process (same PID, no
 * fork), while a FOREIGN interp still forks (testextern_foreign.c proves that).
 *
 * WHAT IT DOES (identical to TESTREAL, all through resident imports so the
 * flows-back is genuine): writes a banner, reads argc off the constructed auxv
 * stack (proving the SysV entry ABI), sets a process-permanent event flag via
 * the resident vms_kif_setef, bumps a resident __thread datum via tls_bump
 * (proving shared TLS through the unchanged thread pointer), then calls the
 * resident imgact_image_exit (SYS$EXIT), which unwinds to imgact_activate and
 * returns to DCL in the same process.
 *
 * Rule 8: ELF/PT_INTERP/auxv/.vms$imp mechanics are the public System V ABI +
 * OVMX's own symbol-vector format (ovmx_image.h); no VMS byte format is claimed.
 * It carries NO own PT_TLS (that class stays deferred) and builds with zero
 * dynamic relocations (link-time-constant GOT patch_off, PC-relative self-
 * reference), exactly like TESTREAL -- the ONLY structural difference is the
 * added .interp section, which the linker wraps in a PT_INTERP program header.
 */
#include <stdint.h>

#include "imgact_activate.h"   /* IMGACT_NOTE_* / IMGACT_ABI_* */

/* The PT_INTERP string of a real external LINK.EXE executable. Placed in an
 * .interp section; the linker emits a PT_INTERP program header pointing at it
 * (verified: a -shared ET_DYN with an .interp section gets a PT_INTERP). Must
 * match src/vmslink/link.c's IMGACT_INTERP -- what LINK.EXE actually writes. */
__attribute__((used, section(".interp")))
static const char extern_interp[] = "/vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE";

/* Fixed work parameters (the suite knows these and checks for their effects). */
#define EXT_EFN          40   /* a LOCAL event flag (cluster 1, 32-63): no ascefc */
#define EXT_TLS_DELTA    7L
#define EXT_EXIT_CODE    0
#define EXT_BADABI_CODE  97   /* SYS$EXIT code when argc off the auxv stack is wrong */

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
#elif defined(__alpha__)
/* See testreal_inproc.c's identical block for the full rationale. */
static long img_write(long fd, const void *buf, long n)
{
    register long v0 __asm__("$0")  = 4;      /* __NR_write */
    register long a0 __asm__("$16") = fd;
    register long a1 __asm__("$17") = (long)buf;
    register long a2 __asm__("$18") = n;
    register long a3 __asm__("$19");
    __asm__ volatile("callsys"
                     : "+r"(v0), "=r"(a3)
                     : "r"(a0), "r"(a1), "r"(a2)
                     : "$1","$2","$3","$4","$5","$6","$7","$8",
                       "$20","$21","$22","$23","$24","$25","$27","$28","memory");
    return a3 ? -v0 : v0;
}
static void img_exit_group(long code)
{
    register long v0 __asm__("$0")  = 405;    /* __NR_exit_group */
    register long a0 __asm__("$16") = code;
    __asm__ volatile("callsys"
                     :
                     : "r"(v0), "r"(a0)
                     : "$1","$2","$3","$4","$5","$6","$7","$8","$19",
                       "$20","$21","$22","$23","$24","$25","$27","$28","memory");
    __builtin_unreachable();
}
#else
static long img_write(long fd, const void *buf, long n){(void)fd;(void)buf;(void)n;return -1;}
static void img_exit_group(long code){(void)code; for(;;){}}
#endif

/*
 * The OVMX in-process marker note. desc = IMGACT_ABI_MARKER | IMGACT_ABI_AUXV:
 * in-process-eligible AND entered through the SysV auxv _start ABI (like TESTREAL).
 */
__attribute__((used, aligned(4), section(".note.ovmx")))
static const struct {
    uint32_t n_namesz;
    uint32_t n_descsz;
    uint32_t n_type;
    char     name[8];
    uint32_t desc;
} extern_ovmx_note = {
    5, 4, IMGACT_NOTE_TYPE, { 'O', 'V', 'M', 'X', 0, 0, 0, 0 },
    IMGACT_ABI_MARKER | IMGACT_ABI_AUXV
};

/*
 * The consumer's GOT cells + .vms$imp import table -- identical construction to
 * TESTREAL (see its header): .vms$got pinned at a FIXED vaddr via
 * -Wl,--section-start so the patch_off literals are as-time constants and the
 * image carries ZERO dynamic relocations.
 */
#define TESTEXTERN_GOT_VA 0x200000
__asm__(
    ".pushsection .vms$got,\"aw\",@progbits\n"
    ".balign 8\n"
    "testextern_got0: .quad 0\n"   /* [IMP_EXIT]  imgact_image_exit */
    "testextern_got1: .quad 0\n"   /* [IMP_SETEF] vms_kif_setef     */
    "testextern_got2: .quad 0\n"   /* [IMP_TLSB]  tls_bump          */
    ".popsection\n"

    ".pushsection .vms$imp,\"a\",@progbits\n"
    ".balign 8\n"
    "testextern_imp:\n"
    "  .long 0x31504d49\n"                                     /* OVMX_IMP_MAGIC */
    "  .long 3\n"                                              /* count          */
    "  .long testextern_imp_names - testextern_imp\n"          /* names_off      */
    "  .long testextern_imp_names_end - testextern_imp_names\n"/* names_size     */
    /* entry: producer_off(u32) sv_index(u32) patch_off(u64) req_major(u32) req_minor(u32) */
    "  .long 0\n  .long 0\n  .quad 0x200000\n  .long 1\n  .long 0\n"
    "  .long 0\n  .long 1\n  .quad 0x200008\n  .long 1\n  .long 0\n"
    "  .long 0\n  .long 2\n  .quad 0x200010\n  .long 1\n  .long 0\n"
    "testextern_imp_names:\n"
    "  .asciz \"LIBVMS$SHR.EXE\"\n"
    "testextern_imp_names_end:\n"
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
    __asm__("lea testextern_got0(%%rip), %0" : "=r"(g));
    return g[i];
#elif defined(__aarch64__)
    void **g;
    __asm__("adrp %0, testextern_got0\n\tadd %0, %0, :lo12:testextern_got0" : "=r"(g));
    return g[i];
#elif defined(__alpha__)
    /* See testreal_inproc.c's got_slot for the full rationale. */
    void **g;
    __asm__(
        "ldah $1, testextern_got0($29) !gprelhigh\n\t"
        "lda  $1, testextern_got0($1) !gprellow\n\t"
        "mov  $1, %0\n\t"
        : "=r"(g) :: "$1");
    return g[i];
#else
    (void)i; return 0;
#endif
}

/* The image body. See testreal_inproc.c's testreal_main for the rationale of
 * each step; identical, distinct banner so the suite can tell them apart. */
__attribute__((visibility("hidden"))) long testextern_main(long argc);
__attribute__((visibility("hidden"))) long testextern_main(long argc)
{
    static const char banner[] = "OVMX-EXTERN-RAN\n";
    img_write(1, banner, (long)(sizeof(banner) - 1));

    if (argc != 1)
        ((exitfn)got_slot(IMP_EXIT))(EXT_BADABI_CODE);

    ((seteffn)got_slot(IMP_SETEF))(EXT_EFN);
    ((tlsbfn)got_slot(IMP_TLSB))(EXT_TLS_DELTA);
    ((exitfn)got_slot(IMP_EXIT))(EXT_EXIT_CODE);

    /* Only reached if the flip is NOT wired (imgact_image_exit found no open
     * in-process activation and would have _exit()'d): terminate honestly. */
    img_exit_group(argc);
    return 0;
}

/* _start for the auxv ABI (imgact_enter_auxv jumps here with rsp -> argc). */
__asm__(
#if defined(__x86_64__)
    ".text\n.globl extern_start\n.type extern_start,@function\n"
    "extern_start:\n"
    "  mov (%rsp), %rdi\n"      /* argc */
    "  call testextern_main\n"
    "  ud2\n"
    ".size extern_start,.-extern_start\n"
#elif defined(__aarch64__)
    ".text\n.globl extern_start\n.type extern_start,@function\n"
    "extern_start:\n"
    "  ldr x0, [sp]\n"          /* argc */
    "  bl testextern_main\n"
    "  brk #0\n"
    ".size extern_start,.-extern_start\n"
#elif defined(__alpha__)
    /* See testreal_inproc.c's real_start for the full rationale. */
    ".text\n.globl extern_start\n.type extern_start,@function\n"
    ".ent extern_start\n"
    "extern_start:\n"
    "  .frame $30, 0, $26, 0\n"
    "  .prologue 0\n"
    "  br   $29, 1f\n"
    "1:\n"
    "  ldgp $29, 0($29)\n"
    "  ldq  $16, 0($30)\n"      /* argc = *(sp) */
    "  jsr  $26, testextern_main\n"
    "  call_pal 0x81\n"         /* bpt: must not return */
    ".end extern_start\n"
    ".size extern_start,.-extern_start\n"
#endif
);
