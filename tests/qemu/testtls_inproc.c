/*
 * testtls_inproc.c - an OWN-PT_TLS REAL-image subject for the in-process
 * activation flip (vms-db2, docs/design-in-process-activation.md Part II §A.8
 * remainder item 4 -- the DTV-append / TLS-absorb-over-resident-C-RTL case). NOT
 * a test suite (its name matches no test_*.c glob, so neither the Dockerfile
 * recipes nor init.sh run it as one). It is the SUBJECT
 * tests/qemu/test_syssvc_imgact_tls.c activates in DCL's own process.
 *
 * HOW IT DIFFERS FROM TESTREAL.EXE. TESTREAL is a real image with NO own PT_TLS:
 * it only touches the RESIDENT producer's TLS through DCL's unchanged thread
 * pointer. TESTTLS is TESTREAL plus its OWN PT_TLS -- a __thread datum that is
 * ITS OWN, accessed through the OVMX .vms$tls TLSDESC path the activator biases
 * against DCL's thread pointer (imgact_setup_own_tls). The image must see its own
 * TLS (init value, then a value it writes) WHILE the resident producer's TLS it
 * also touches stays a DISTINCT block, and DCL's own thread pointer + TLS survive
 * untouched. That is the no-clobber crux the whole own-PT_TLS increment turns on.
 *
 * WHAT IT DOES (order matters -- the init check bails BEFORE any write, so the
 * negctl that breaks the TP-bias reddens cleanly without corrupting anything):
 *   1. Writes a banner to fd 1 so the suite sees it RAN.
 *   2. Reads argc off the constructed auxv stack; if != 1 it takes a distinct
 *      bad-ABI SYS$EXIT code and touches nothing (the auxv entry is load-bearing).
 *   3. Reads its OWN __thread datum through the biased .vms$tls TLSDESC sequence
 *      (resident resolver + TP + biased offset). It must equal TLS_INIT_MAGIC --
 *      the activator copied the image's .tdata init image into the image's OWN
 *      block. If the TP-bias is wrong the access lands in DCL's TLS (not the
 *      magic) -> distinct bad-TLS-init exit code, no write (no clobber).
 *   4. Bumps the RESIDENT producer's __thread counter (imported tls_bump) through
 *      the UNCHANGED thread pointer -- proving the resident universals still reach
 *      the RESIDENT TLS, a block distinct from the image's own.
 *   5. WRITES a sentinel into its OWN __thread datum and reads it back; if the
 *      readback is wrong -> distinct bad-TLS-write exit code. Because this write
 *      lands in the image's own block (not DCL's), the suite separately confirms
 *      the resident counter did NOT become the sentinel (distinct blocks).
 *   6. Sets a process-permanent event flag (imported vms_kif_setef) -- flows back.
 *   7. Calls the imported SYS$EXIT(0) -- returns to DCL in-process.
 *
 * Rule 8: ELF/auxv/PT_TLS/TLSDESC/.vms$imp mechanics are the public System V +
 * per-arch ELF TLS ABI + OVMX's own symbol-vector/.vms$tls format (ovmx_image.h);
 * no VMS byte format is claimed. It carries NO PT_INTERP and builds with zero
 * dynamic relocations: the TLSDESC descriptor and GOT cells are filled by the
 * activator (like TESTREAL's GOT), and the .vms$tls literal offset + .vms$imp
 * patch_offs are link-time-constant section-start pins, not relocations.
 */
#include <stdint.h>

#include "imgact_activate.h"   /* IMGACT_NOTE_* / IMGACT_ABI_* */

/* Fixed work parameters (the suite knows these and checks for their effects). */
#define TLS_INIT_MAGIC   0x00ABCDEF01234567L  /* the image's .tdata init datum   */
#define TLS_SENTINEL     0x0BADF00D5EED1234L  /* what the image writes to its TLS */
#define TLS_EFN          41   /* a LOCAL event flag (cluster 1, 32-63): no ascefc */
#define TLS_DELTA        9L   /* how much the image bumps the RESIDENT TLS counter*/
#define TLS_EXIT_CODE    0
#define TLS_BADABI_CODE  97   /* argc off the auxv stack was wrong                */
#define TLS_BADINIT_CODE 91   /* own TLS did not read back the init magic         */
#define TLS_BADWRITE_CODE 92  /* own TLS did not read back the written sentinel   */

/* Import vector indices into the resident producer's .vms$sv (the suite builds
 * the producer with exactly this order -- identical to TESTREAL's). */
#define IMP_EXIT   0
#define IMP_SETEF  1
#define IMP_TLSB   2

/* ---- raw write(2) for the banner + backstop exit (need no import) --------- */
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
 * in-process-eligible AND entered through the SysV auxv _start ABI (same as
 * TESTREAL). The own PT_TLS is detected from the PT_TLS program header, not the
 * marker.
 */
__attribute__((used, aligned(4), section(".note.ovmx")))
static const struct {
    uint32_t n_namesz;
    uint32_t n_descsz;
    uint32_t n_type;
    char     name[8];
    uint32_t desc;
} tls_ovmx_note = {
    5, 4, IMGACT_NOTE_TYPE, { 'O', 'V', 'M', 'X', 0, 0, 0, 0 },
    IMGACT_ABI_MARKER | IMGACT_ABI_AUXV
};

/*
 * The image's OWN PT_TLS: an 8-byte __thread datum initialized to TLS_INIT_MAGIC.
 * A .tdata section with the SHF_TLS flag ("T") makes the linker emit a PT_TLS
 * program header whose init image the activator copies into the image's OWN
 * block. img_tls_slot is at module-relative offset 0 (the first/only TLS datum).
 */
__asm__(
    ".pushsection .tdata,\"awT\",@progbits\n"
    ".balign 8\n"
    "img_tls_slot: .quad 0x00ABCDEF01234567\n"   /* == TLS_INIT_MAGIC */
    ".popsection\n"
);

/*
 * The image's TLSDESC descriptor + the .vms$tls table pointing at it. A TLSDESC
 * entry is two quadwords: [0] = resolver (imgact_setup_own_tls fills the resident
 * ovmx_tlsdesc_static), [1] = the variable's MODULE-relative offset (0 here; the
 * activator ADDS the block's TP-relative bias). Both start 0 and carry no dynamic
 * relocation -- the activator writes them, exactly like the .vms$imp GOT cells.
 * .vms$tdesc is pinned at a FIXED vaddr (see CMake --section-start) so the
 * .vms$tls entry can name the descriptor's image-relative offset as a link-time
 * constant literal (a hand-assembled object cannot compute "cell - __ehdr_start"
 * as an as-time constant). .vms$tls itself is read-only (the activator reads it).
 */
#define TESTTLS_TDESC_VA 0x210000
__asm__(
    ".pushsection .vms$tdesc,\"aw\",@progbits\n"
    ".balign 8\n"
    "img_tlsdesc:\n"
    "  .quad 0\n"    /* [0] resolver  (activator fills ovmx_tlsdesc_static)  */
    "  .quad 0\n"    /* [1] module-relative offset (=0); activator += bias   */
    ".popsection\n"

    ".pushsection .vms$tls,\"a\",@progbits\n"
    ".balign 8\n"
    "  .long 0x31534c54\n"      /* OVMX_TLS_MAGIC "TLS1" */
    "  .long 1\n"               /* count                 */
    "  .quad 0x210000\n"        /* == TESTTLS_TDESC_VA: img_tlsdesc image-rel off */
    ".popsection\n"
);

/*
 * The consumer's GOT cells + .vms$imp import table -- identical shape to
 * TESTREAL's (three imports at .vms$got + {0,8,16}, bound to the resident
 * producer before _start). See testreal_inproc.c for the pinning rationale.
 */
#define TESTTLS_GOT_VA 0x200000
__asm__(
    ".pushsection .vms$got,\"aw\",@progbits\n"
    ".balign 8\n"
    "testtls_got0: .quad 0\n"   /* [IMP_EXIT]  imgact_image_exit */
    "testtls_got1: .quad 0\n"   /* [IMP_SETEF] vms_kif_setef     */
    "testtls_got2: .quad 0\n"   /* [IMP_TLSB]  tls_bump          */
    ".popsection\n"

    ".pushsection .vms$imp,\"a\",@progbits\n"
    ".balign 8\n"
    "testtls_imp:\n"
    "  .long 0x31504d49\n"                               /* OVMX_IMP_MAGIC */
    "  .long 3\n"                                        /* count          */
    "  .long testtls_imp_names - testtls_imp\n"          /* names_off      */
    "  .long testtls_imp_names_end - testtls_imp_names\n"/* names_size     */
    /* entry: producer_off(u32) sv_index(u32) patch_off(u64) req_major(u32) req_minor(u32) */
    "  .long 0\n  .long 0\n  .quad 0x200000\n  .long 1\n  .long 0\n"
    "  .long 0\n  .long 1\n  .quad 0x200008\n  .long 1\n  .long 0\n"
    "  .long 0\n  .long 2\n  .quad 0x200010\n  .long 1\n  .long 0\n"
    "testtls_imp_names:\n"
    "  .asciz \"LIBVMS$SHR.EXE\"\n"
    "testtls_imp_names_end:\n"
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
    __asm__("lea testtls_got0(%%rip), %0" : "=r"(g));
    return g[i];
#elif defined(__aarch64__)
    void **g;
    __asm__("adrp %0, testtls_got0\n\tadd %0, %0, :lo12:testtls_got0" : "=r"(g));
    return g[i];
#elif defined(__alpha__)
    /* See testreal_inproc.c's got_slot for the full rationale. */
    void **g;
    __asm__(
        "ldah $1, testtls_got0($29) !gprelhigh\n\t"
        "lda  $1, testtls_got0($1) !gprellow\n\t"
        "mov  $1, %0\n\t"
        : "=r"(g) :: "$1");
    return g[i];
#else
    (void)i; return 0;
#endif
}

/*
 * Read / write the image's OWN __thread datum through the genuine TLSDESC access
 * sequence: load the descriptor address, CALL its resolver word (the resident
 * ovmx_tlsdesc_static, which returns the biased TP-relative offset preserving
 * every other register), then dereference thread-pointer + offset. This exercises
 * BOTH descriptor words the activator filled -- the resolver ([0]) and the biased
 * offset ([1]) -- so a broken bias is observable here, not papered over.
 */
static long img_tls_read(void)
{
#if defined(__x86_64__)
    long v;
    __asm__ volatile(
        "lea img_tlsdesc(%%rip), %%rax\n\t"
        "call *(%%rax)\n\t"              /* resolver -> biased offset in rax     */
        "mov %%fs:(%%rax), %0\n\t"       /* *(TP + offset): the image's OWN slot */
        : "=r"(v) : : "rax", "memory");
    return v;
#elif defined(__aarch64__)
    long v;
    __asm__ volatile(
        "adrp x0, img_tlsdesc\n\t"
        "add x0, x0, :lo12:img_tlsdesc\n\t"
        "ldr x1, [x0]\n\t"              /* x1 = descriptor resolver             */
        "blr x1\n\t"                    /* -> biased offset in x0               */
        "mrs x1, tpidr_el0\n\t"
        "ldr %0, [x1, x0]\n\t"          /* *(TP + offset)                       */
        : "=r"(v) : : "x0", "x1", "memory");
    return v;
#elif defined(__alpha__)
    /* Alpha has no PC-relative lea/adrp (see got_slot above) and no hardware
     * TLSDESC (src/imgact/arch/alpha/start.S's __tlsdesc_static header
     * comment) -- but this is OVMX's OWN descriptor convention (a plain
     * function-pointer-plus-offset pair we call by hand), not the ELF
     * TLSDESC ABI, so it ports cleanly: load &img_tlsdesc GP-relatively,
     * indirect-call its resolver word with the descriptor address in $16
     * (a0) per __tlsdesc_static's documented contract ("arg in $16, returns
     * in $0"), then add the Alpha thread pointer (rduniq, call_pal 0x9e --
     * same primitive src/imgact/arch/alpha/start.S's imgact_get_tp uses) to
     * the returned bias and dereference. The resolver call is a genuine
     * indirect call (target loaded into $27 = pv, then jsr $26,($27)), so
     * the callee can validly re-derive its own gp if it needs to -- same
     * ABI contract as got_slot's callees elsewhere in this file. $9 (a
     * callee-saved register) carries the resolver's result across the
     * call_pal so it is not lost; declared as a clobber so GCC saves/
     * restores it around this asm as needed. Verified end-to-end under
     * qemu-alpha (a standalone harness driving this exact resolver-call +
     * rduniq sequence through a hand-built descriptor matched the expected
     * TP+bias address). */
    long v;
    __asm__ volatile(
        "ldah $16, img_tlsdesc($29) !gprelhigh\n\t"
        "lda  $16, img_tlsdesc($16) !gprellow\n\t"
        "ldq  $27, 0($16)\n\t"          /* resolver = descriptor[0]              */
        "jsr  $26, ($27)\n\t"           /* resolver(a0=&descriptor) -> $0 = bias */
        "mov  $0, $9\n\t"
        "call_pal 0x9e\n\t"             /* rduniq -> $0 = TP                     */
        "addq $0, $9, $0\n\t"
        "ldq  %0, 0($0)\n\t"            /* *(TP + bias): the image's OWN slot    */
        : "=r"(v)
        : : "$16", "$27", "$26", "$0", "$9", "memory");
    return v;
#else
    return 0;
#endif
}

static void img_tls_write(long val)
{
#if defined(__x86_64__)
    __asm__ volatile(
        "lea img_tlsdesc(%%rip), %%rax\n\t"
        "call *(%%rax)\n\t"
        "mov %0, %%fs:(%%rax)\n\t"
        : : "r"(val) : "rax", "memory");
#elif defined(__aarch64__)
    __asm__ volatile(
        "adrp x0, img_tlsdesc\n\t"
        "add x0, x0, :lo12:img_tlsdesc\n\t"
        "ldr x1, [x0]\n\t"
        "blr x1\n\t"
        "mrs x1, tpidr_el0\n\t"
        "str %0, [x1, x0]\n\t"
        : : "r"(val) : "x0", "x1", "memory");
#elif defined(__alpha__)
    /* See img_tls_read's identical descriptor-call + rduniq sequence above
     * for the full rationale; this is the store form. %0 (val) is left to
     * GCC's own register choice, which will not collide with the clobbered
     * scratch set below. */
    __asm__ volatile(
        "ldah $16, img_tlsdesc($29) !gprelhigh\n\t"
        "lda  $16, img_tlsdesc($16) !gprellow\n\t"
        "ldq  $27, 0($16)\n\t"          /* resolver = descriptor[0]              */
        "jsr  $26, ($27)\n\t"           /* resolver(a0=&descriptor) -> $0 = bias */
        "mov  $0, $9\n\t"
        "call_pal 0x9e\n\t"             /* rduniq -> $0 = TP                     */
        "addq $0, $9, $0\n\t"
        "stq  %0, 0($0)\n\t"            /* *(TP + bias) = val                    */
        : : "r"(val)
        : "$16", "$27", "$26", "$0", "$9", "memory");
#else
    (void)val;
#endif
}

/* The image body. hidden visibility so tls_start's call is a direct PC-relative
 * call, not a PLT slot (imgact refuses an image with a PLT). */
__attribute__((visibility("hidden"))) long testtls_main(long argc);
__attribute__((visibility("hidden"))) long testtls_main(long argc)
{
    static const char banner[] = "OVMX-TLSIMG-RAN\n";
    img_write(1, banner, (long)(sizeof(banner) - 1));

    /* 2: the auxv stack must have delivered argc == 1. */
    if (argc != 1)
        ((exitfn)got_slot(IMP_EXIT))(TLS_BADABI_CODE);

    /* 3: the image's OWN __thread datum must read back its init magic. If the
     * TP-bias is wrong the access lands in DCL's TLS instead -> bail with a
     * distinct code and touch NOTHING (no write, no resident bump, no event
     * flag), so every downstream proof depends on the bias being right. */
    if (img_tls_read() != TLS_INIT_MAGIC)
        ((exitfn)got_slot(IMP_EXIT))(TLS_BADINIT_CODE);

    /* 4: the RESIDENT producer's TLS, through the unchanged TP -- a DISTINCT
     * block from the image's own (the suite checks this counter moved by exactly
     * TLS_DELTA and did NOT become the sentinel below). */
    ((tlsbfn)got_slot(IMP_TLSB))(TLS_DELTA);

    /* 5: WRITE the image's own datum and read it back -- proving the own block is
     * writable and distinct. This write lands in the image's block, not DCL's. */
    img_tls_write(TLS_SENTINEL);
    if (img_tls_read() != TLS_SENTINEL)
        ((exitfn)got_slot(IMP_EXIT))(TLS_BADWRITE_CODE);

    /* 6: a process-permanent event flag set through the resident executive. */
    ((seteffn)got_slot(IMP_SETEF))(TLS_EFN);
    /* 7: SYS$EXIT -- returns to DCL in-process; does not come back here. */
    ((exitfn)got_slot(IMP_EXIT))(TLS_EXIT_CODE);

    /* Only reached if the flip is NOT wired: terminate honestly. */
    img_exit_group(argc);
    return 0;
}

/*
 * _start for the auxv ABI. imgact_enter_auxv jumps here with rsp at the
 * constructed initial stack (rsp -> argc). Read argc and enter the C body.
 */
__asm__(
#if defined(__x86_64__)
    ".text\n.globl tls_start\n.type tls_start,@function\n"
    "tls_start:\n"
    "  mov (%rsp), %rdi\n"      /* argc */
    "  call testtls_main\n"
    "  ud2\n"
    ".size tls_start,.-tls_start\n"
#elif defined(__aarch64__)
    ".text\n.globl tls_start\n.type tls_start,@function\n"
    "tls_start:\n"
    "  ldr x0, [sp]\n"          /* argc */
    "  bl testtls_main\n"
    "  brk #0\n"
    ".size tls_start,.-tls_start\n"
#elif defined(__alpha__)
    /* See testreal_inproc.c's real_start for the full rationale. */
    ".text\n.globl tls_start\n.type tls_start,@function\n"
    ".ent tls_start\n"
    "tls_start:\n"
    "  .frame $30, 0, $26, 0\n"
    "  .prologue 0\n"
    "  br   $29, 1f\n"
    "1:\n"
    "  ldgp $29, 0($29)\n"
    "  ldq  $16, 0($30)\n"      /* argc = *(sp) */
    "  jsr  $26, testtls_main\n"
    "  call_pal 0x81\n"         /* bpt: must not return */
    ".end tls_start\n"
    ".size tls_start,.-tls_start\n"
#endif
);
