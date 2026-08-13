/*
 * sys_imgact.c - in-process image activation library (vms-68f increment iv).
 *
 * imgact_activate() runs an image IN THE CURRENT PROCESS instead of the
 * fork()+execve() model src/vmsdcl/dcl_cmd_process.c has always used: it maps
 * the image into a reserved P0 window (MAP_FIXED), records the extent with the
 * executive (vms_kif_p0_map), protects DCL's critical P1 pages
 * (vms_kif_p1_protect), transitions Supervisor->User (vms_kif_enter_image),
 * enters the image's entry point, and on the image's return runs it down
 * (vms_kif_image_rundown / vms_kif_p1_protect restore / vms_kif_p0_unmap) and
 * returns to DCL -- same PID throughout. See
 * docs/design-in-process-activation.md Part II §A.2 and
 * src/libvms/include/imgact_activate.h for the model and this increment's
 * ceiling.
 *
 * ENTRY + RETURN MECHANISM, AND WHY NOT swapcontext (increment iv ceiling).
 * The design (§A.6.4) prefers swapcontext onto a separate User-mode stack in
 * the P0 window so Ctrl-Y/CONTINUE re-entry is natural. THAT is deferred to
 * increment v: the ucontext family (makecontext/swapcontext/getcontext) and
 * sigaltstack are NOT universals of DECC$SHR, so an imgact_activate that used
 * them would not link into the VMS-native DCL.EXE (LINK.EXE --executable --use
 * {DECC$SHR,...}, the self-hosting path) -- and the fork replacement must link
 * everywhere DCL does. So iv enters the image with a direct call on the
 * process's own stack and recovers a faulting image with setjmp/longjmp + a
 * SIGSEGV handler (all DECC$SHR universals). The KEY PROPERTY -- the image runs
 * in DCL's process, same PID, no fork -- holds identically; only the separate
 * P0 stack and Ctrl-Y re-entry wait for v. Every libc symbol this file uses
 * (open/lseek/read/close, mmap/mprotect/munmap, memcmp/memset, sigaction/
 * sigemptyset/sigaddset/sigprocmask, setjmp/longjmp) is already a DECC$SHR
 * universal, so adding sys_imgact.c to the shareable (mk_libvms_shr.sh) needs
 * no C-RTL expansion.
 *
 * INV-6 (CLAUDE.md Rule 9). Every executive step goes through /dev/vms; with
 * no executive the vms_kif_* calls return SS$_NOSUCHDEV and this function
 * refuses to run the image -- it never silently falls back to a per-process
 * "load and pretend". The eligibility decision (SS$_UNSUPPORTED for any
 * non-in-process image) is made from the ELF alone, BEFORE any executive call,
 * so a real image's fork fallback does not depend on /dev/vms being present.
 *
 * Rule 8: the ELF/PT_LOAD/PT_NOTE/R_*_RELATIVE mechanics are the public System
 * V ABI and OVMX's own in-process marker (imgact_activate.h); no VMS byte
 * format is claimed. Rule 10: what is enforced (P0 teardown at the MMU,
 * critical-P1 mprotect, service-boundary mode) vs. modeled is stated in the
 * design; this file implements the enforced half.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <elf.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>

#include "ssdef.h"
#include "vms_kif.h"
#include "imgact_activate.h"
#include "ovmx_image.h"       /* .vms$imp / .vms$rel section names + structs */
#include "ovmx_symvec.h"      /* ovmx_sv_entries/names (producer C-RTL fence) */
#include "imgact_prodreg.h"   /* imgact_bind_imports_mapped() (vms-db2)       */

#if defined(__x86_64__)
#  define IMGACT_EM       EM_X86_64
#  define IMGACT_R_RELATIVE R_X86_64_RELATIVE
#elif defined(__aarch64__)
#  define IMGACT_EM       EM_AARCH64
#  define IMGACT_R_RELATIVE R_AARCH64_RELATIVE
#else
#  define IMGACT_EM       0
#  define IMGACT_R_RELATIVE 0
#endif

/* P0 window: a contiguous reservation the image's PT_LOADs map into with
 * MAP_FIXED, so "P0 deleted at rundown" is genuinely enforced at the MMU
 * (design §A.2.1). 64 MiB covers any freestanding OVMX image. */
#define IMGACT_P0_WINDOW   (64UL * 1024 * 1024)
/* User-mode image stack for the auxv ABI, at the top of the P0 window (freed
 * with the whole window at rundown). VMS gives an image a separate User stack;
 * this is its P0-window analogue (design §A.6.4 option (b)). */
#define IMGACT_IMG_STACK   (256UL * 1024)
#define IMGACT_PGSZ        4096UL
#define IMGACT_TRUNC(x)    ((x) & ~(IMGACT_PGSZ - 1))
#define IMGACT_ROUND(x)    (((x) + IMGACT_PGSZ - 1) & ~(IMGACT_PGSZ - 1))
#define IMGACT_MAXPHDR     64

typedef long (*imgact_entryfn)(long, long);

/*
 * Resident TLSDESC static resolver (vms-db2, §A.8 remainder item 4 -- own-PT_TLS
 * in-process). Returns descriptor->arg (word 1, the biased TP-relative offset of
 * the variable) in the return register, PRESERVING every other register per the
 * TLSDESC ABI -- the same contract IMGACT.EXE's __tlsdesc_static keeps
 * (src/imgact/arch, per-arch start.S). entry[0] of every biased .vms$tls descriptor
 * points here and the in-process image's TLSDESC access sequence calls it.
 * Hidden: this is an internal libvms address written into the image's own
 * descriptor cell, NOT a C-RTL universal -- it never enters the .vms$sv symbol
 * vector or the kif SYS_VEC, so adding it triggers no cascade. `mov`/`ldr` set
 * no flags, so the caller's condition codes survive the call too. */
#if defined(__x86_64__) || defined(__aarch64__)
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl ovmx_tlsdesc_static\n"
    ".hidden ovmx_tlsdesc_static\n"
#if defined(__x86_64__)
    ".type ovmx_tlsdesc_static,@function\n"
    "ovmx_tlsdesc_static:\n"
    "    movq 8(%rax), %rax\n"      /* return descriptor->arg (TP-rel offset) */
    "    ret\n"
    ".size ovmx_tlsdesc_static,.-ovmx_tlsdesc_static\n"
#elif defined(__aarch64__)
    ".type ovmx_tlsdesc_static,%function\n"
    "ovmx_tlsdesc_static:\n"
    "    ldr x0, [x0, #8]\n"        /* return descriptor->arg (TP-rel offset) */
    "    ret\n"
    ".size ovmx_tlsdesc_static,.-ovmx_tlsdesc_static\n"
#endif
);
extern void ovmx_tlsdesc_static(void);
#else
/* No resident TLSDESC resolver on this architecture. The own-PT_TLS in-process
 * activation path is x86_64/aarch64-only (IMGACT_EM==0 elsewhere, and the User-
 * mode stack-switch jump above is a no-op); on netbsd-vax image activation is
 * delegated to the platform loader (ld.elf_so, gate activation-netbsd-vax), so
 * the biased .vms$tls descriptor cells that would point here are never
 * constructed and this resolver is never called. A defined, hidden stub keeps
 * libvms linking on every target (elf32-vax, vms-1cb2); it traps if ever
 * reached -- the same "must not happen" contract as __builtin_unreachable()
 * above -- rather than silently returning a bogus TP offset. */
__attribute__((visibility("hidden")))
void ovmx_tlsdesc_static(void) { __builtin_trap(); }
#endif

/* Read the process's ALREADY-PROGRAMMED thread pointer without reprogramming it
 * (the whole point of the own-TLS-over-DCL case). x86_64: musl stores the TCB
 * self-pointer at %fs:0, so the FS base is readable there (no rdfsbase needed),
 * matching IMGACT.EXE's imgact_get_tp. aarch64: TPIDR_EL0. */
static inline unsigned long imgact_read_tp(void)
{
#if defined(__x86_64__)
    unsigned long tp;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(tp));
    return tp;
#elif defined(__aarch64__)
    unsigned long tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    return tp;
#else
    return 0;
#endif
}

/*
 * Activation is not reentrant (VMS runs one image at a time per process), so
 * the recovery state is file-scope. A nested in-process activation is out of
 * scope for this increment; the entry gate refuses a second ENTER_IMAGE while
 * one is open.
 *
 * g_img_jb is the single recovery point set by setjmp() in imgact_activate()
 * before the image is entered, and reached two ways (longjmp value distinguishes
 * them): IMGACT_JB_FAULT from the SIGSEGV handler (a wild write; -> SS$_ACCVIO),
 * IMGACT_JB_EXIT from imgact_image_exit() (the image called SYS$EXIT; a normal
 * return to DCL, exit code in g_exit_code). g_inproc_active gates
 * imgact_image_exit: only while an in-process image is running does SYS$EXIT
 * unwind rather than terminate the process.
 */
#define IMGACT_JB_FAULT 1
#define IMGACT_JB_EXIT  2
static jmp_buf               g_img_jb;
static volatile sig_atomic_t g_faulted;
static volatile sig_atomic_t g_inproc_active;
static volatile int          g_exit_code;

/*
 * SIGSEGV while the image runs (e.g. a wild write to a protected critical-P1
 * page) is converted to $EXIT(SS$_ACCVIO)+rundown: longjmp back onto DCL's
 * frame, abandoning the image. Design §A.6.3 (pragmatic first). The image runs
 * on a separate P0-window stack (auxv ABI) or the process stack ((a0,a1) ABI);
 * either way the fault here is a data write, so the handler's own frame is fine.
 */
static void imgact_segv(int sig)
{
    (void)sig;
    g_faulted = 1;
    longjmp(g_img_jb, IMGACT_JB_FAULT);
}

/*
 * SYS$EXIT for an in-process image (imgact_activate.h). See that header for the
 * model. While an in-process activation is open this UNWINDS to imgact_activate
 * (return to DCL, same process); otherwise it terminates the process, so a
 * forked/normal image that reaches the resident copy still exits correctly.
 */
void imgact_image_exit(int code)
{
    if (g_inproc_active) {
        g_exit_code = code;
        longjmp(g_img_jb, IMGACT_JB_EXIT);
    }
    _exit(code);
}

/*
 * Enter an image at its SysV `_start` with rsp pointing at a constructed initial
 * process stack, on a separate User-mode stack in the P0 window (design §A.6.4
 * option (b): a manual per-arch stack switch built from already-exported
 * primitives, NOT the ucontext family). `entry` is the absolute _start address;
 * `sp` the 16-byte-aligned top of the constructed stack (its first quadword is
 * argc, the SysV ABI). This never returns through here: the image leaves only by
 * calling SYS$EXIT (imgact_image_exit -> longjmp) or by faulting (SIGSEGV ->
 * longjmp), both of which unwind to imgact_activate's setjmp, not to this frame.
 * rdx=0 tells the crt there is no rtld_fini to register (exactly what the kernel
 * hands a static image). Marked noreturn so the compiler keeps no live state
 * across the switch.
 */
__attribute__((noreturn))
static void imgact_enter_auxv(unsigned long entry, unsigned long sp)
{
#if defined(__x86_64__)
    __asm__ volatile(
        "mov %1, %%rsp\n\t"   /* switch to the constructed User-mode stack   */
        "xor %%ebp, %%ebp\n\t"/* clear frame pointer (as the kernel does)    */
        "xor %%edx, %%edx\n\t"/* rdx = 0: no rtld_fini                       */
        "jmp *%0\n\t"         /* jump to the image's _start (no return addr) */
        : : "r"(entry), "r"(sp) : "memory");
#elif defined(__aarch64__)
    __asm__ volatile(
        "mov sp, %1\n\t"      /* switch to the constructed User-mode stack   */
        "mov x29, xzr\n\t"    /* clear frame pointer                         */
        "mov x0, xzr\n\t"     /* x0 = 0: no rtld_fini                        */
        "br %0\n\t"           /* branch to the image's _start                */
        : : "r"(entry), "r"(sp) : "memory");
#else
    (void)entry; (void)sp;
#endif
    __builtin_unreachable();
}

/*
 * Build the SysV initial process stack the auxv ABI hands _start, at the top of
 * a writable region [stk_lo, stk_hi) in the P0 window. Lays argc/argv/envp/auxv
 * per the System V x86-64/AArch64 process-initialization ABI (public, Rule 8 --
 * no VMS format claimed): one arg (the image name), no env, and the auxv entries
 * a static-PIE crt reads (AT_PHDR/PHENT/PHNUM, AT_ENTRY, AT_PAGESZ, AT_RANDOM,
 * AT_NULL). Returns the 16-byte-aligned rsp (points at argc), or 0 if it does
 * not fit. `phdr_va` is the run-time address of the mapped program headers.
 */
static unsigned long imgact_build_auxv_stack(unsigned long stk_lo,
                                             unsigned long stk_hi,
                                             unsigned long entry,
                                             unsigned long phdr_va,
                                             unsigned long phent,
                                             unsigned long phnum)
{
    /* String/AT_RANDOM area at the very top; then the aligned control block
     * below it. All writes stay within [stk_lo, stk_hi). */
    static const char argv0[] = "OVMX-IMAGE";
    unsigned long top = stk_hi;

    /* 16 random bytes for AT_RANDOM (crt stack-canary seed). */
    top -= 16;
    unsigned long rand_va = top;
    for (int i = 0; i < 16; i++)
        ((unsigned char *)rand_va)[i] = (unsigned char)(0x5A ^ i);

    top -= sizeof(argv0);
    unsigned long argv0_va = top;
    memcpy((void *)argv0_va, argv0, sizeof(argv0));

    /* Control block: argc, argv[0], NULL, envp NULL, then 6 auxv pairs + AT_NULL
     * pair. 4 + 12 + 2 = 18 quadwords; round the base DOWN to 16-byte alignment
     * so rsp (at argc) satisfies the ABI. */
    unsigned long words = 4 + 12 + 2;   /* see below */
    unsigned long sp = top - words * 8;
    sp &= ~0xFUL;
    if (sp < stk_lo)
        return 0;

    unsigned long *w = (unsigned long *)sp;
    unsigned long k = 0;
    w[k++] = 1;                 /* argc */
    w[k++] = argv0_va;          /* argv[0] */
    w[k++] = 0;                 /* argv terminator */
    w[k++] = 0;                 /* envp terminator (no env) */
    w[k++] = 6;   w[k++] = 4096;        /* AT_PAGESZ */
    w[k++] = 3;   w[k++] = phdr_va;     /* AT_PHDR   */
    w[k++] = 4;   w[k++] = phent;       /* AT_PHENT  */
    w[k++] = 5;   w[k++] = phnum;       /* AT_PHNUM  */
    w[k++] = 9;   w[k++] = entry;       /* AT_ENTRY  */
    w[k++] = 25;  w[k++] = rand_va;     /* AT_RANDOM */
    w[k++] = 0;   w[k++] = 0;           /* AT_NULL   */
    return sp;
}

/* Read exactly n bytes at file offset off (pread is not a DECC$SHR universal;
 * lseek+read are). Returns 0 on success, -1 otherwise. */
static int imgact_readat(int fd, void *buf, unsigned long n, long off)
{
    unsigned char *p = buf;
    if (lseek(fd, off, SEEK_SET) != off)
        return -1;
    while (n) {
        long r = read(fd, p, n);
        if (r <= 0)
            return -1;
        p += r;
        n -= (unsigned long)r;
    }
    return 0;
}

/*
 * PT_INTERP acceptance for the EXTERNAL-image flip (vms-db2, §A.8 remainder
 * item 2 -- the PT_INTERP-bearing class). A genuinely external LINK.EXE image
 * names its loader in PT_INTERP: LINK.EXE emits
 * "/vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE" (src/vmslink/link.c IMGACT_INTERP), so
 * the kernel/fork path activates it by exec'ing IMGACT.EXE. In-process
 * activation IS that loader, so an interp naming the OVMX loader (basename
 * IMGACT.EXE) does NOT disqualify the image -- imgact_activate does the
 * interpreter's job in DCL's own process. An interp naming ANYTHING else (a
 * foreign ld.so, a #! shell) is not ours to run in-process and is refused
 * (SS$_UNSUPPORTED) so the caller forks. Reading the interp string and matching
 * our own loader's name is an OVMX convention (Rule 8 -- no VMS byte format is
 * claimed; the ELF/PT_INTERP mechanics are the public System V ABI).
 *
 * Returns 1 if the interp at file offset `off` (length `sz`, NUL-terminated per
 * the ABI) has basename IMGACT_LOADER_BASENAME, 0 otherwise (including any read
 * error or an implausibly long interp). Read from the still-open image fd via
 * imgact_readat (lseek+read; DECC$SHR universals) before any mmap or executive
 * call, so a foreign-interp image forks with no executive interaction.
 */
#define IMGACT_LOADER_BASENAME "IMGACT.EXE"
static int imgact_interp_is_ours(int fd, unsigned long off, unsigned long sz)
{
    char buf[256];
    if (sz == 0 || sz > sizeof buf)
        return 0;
    if (imgact_readat(fd, buf, sz, (long)off) != 0)
        return 0;
    buf[sz - 1] = '\0';   /* the PT_INTERP string is NUL-terminated (p_filesz) */

    /* basename = the tail after the last '/' (or the whole string if none). */
    const char *bn = buf;
    for (const char *p = buf; *p; p++)
        if (*p == '/')
            bn = p + 1;

    const char *want = IMGACT_LOADER_BASENAME;
    unsigned long j = 0;
    while (want[j] && bn[j] == want[j])
        j++;
    return want[j] == '\0' && bn[j] == '\0';
}

/* Scan PT_NOTE segments for the OVMX in-process marker, returning its 4-byte
 * descriptor (the entry-ABI flag word, IMGACT_ABI_*) or -1 if no marker note is
 * present. A descriptor shorter than 4 bytes reads as IMGACT_ABI_MARKER (the
 * increment-iv (a0,a1) default) so an older marker image keeps its ABI. */
static long imgact_marker_desc(unsigned long base, const Elf64_Phdr *ph, int n)
{
    for (int i = 0; i < n; i++) {
        if (ph[i].p_type != PT_NOTE)
            continue;
        unsigned char *p   = (unsigned char *)(base + ph[i].p_vaddr);
        unsigned char *end = p + ph[i].p_memsz;
        while (p + 12 <= end) {
            uint32_t ns = *(uint32_t *)p;
            uint32_t ds = *(uint32_t *)(p + 4);
            uint32_t ty = *(uint32_t *)(p + 8);
            unsigned char *name = p + 12;
            unsigned char *desc = name + ((ns + 3u) & ~3u);
            if (ns == sizeof(IMGACT_NOTE_OWNER) && ty == IMGACT_NOTE_TYPE &&
                memcmp(name, IMGACT_NOTE_OWNER, 4) == 0)
                return (ds >= 4 && desc + 4 <= end)
                           ? (long)(*(uint32_t *)desc)
                           : (long)IMGACT_ABI_MARKER;
            p = desc + ((ds + 3u) & ~3u);
        }
    }
    return -1;
}

/*
 * Apply R_*_RELATIVE relocations from PT_DYNAMIC. Returns 0 on success, -1 if
 * the image carries any non-RELATIVE relocation (a symbolic import) -- which
 * makes it NOT in-process-eligible under this increment.
 */
static int imgact_relocate(unsigned long base, const Elf64_Phdr *ph, int n)
{
    const Elf64_Dyn *dyn = NULL;
    for (int i = 0; i < n; i++)
        if (ph[i].p_type == PT_DYNAMIC)
            dyn = (const Elf64_Dyn *)(base + ph[i].p_vaddr);
    if (!dyn)
        return 0;   /* no dynamic section: nothing to relocate */

    Elf64_Addr rela = 0, jmprel = 0;
    Elf64_Xword relasz = 0, relaent = sizeof(Elf64_Rela), pltrelsz = 0;
    for (const Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_RELA:     rela     = d->d_un.d_ptr; break;
        case DT_RELASZ:   relasz   = d->d_un.d_val; break;
        case DT_RELAENT:  relaent  = d->d_un.d_val; break;
        case DT_JMPREL:   jmprel   = d->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltrelsz = d->d_un.d_val; break;
        case DT_REL: case DT_RELSZ:
            return -1;  /* 32-bit REL form: not an OVMX x86_64/aarch64 image */
        default: break;
        }
    }
    if (jmprel || pltrelsz)
        return -1;      /* a PLT means symbolic imports -> not eligible */
    if (!rela || !relasz || !relaent)
        return 0;
    for (Elf64_Xword off = 0; off < relasz; off += relaent) {
        const Elf64_Rela *r = (const Elf64_Rela *)(base + rela + off);
        if (ELF64_R_TYPE(r->r_info) != (uint32_t)IMGACT_R_RELATIVE)
            return -1;  /* any symbolic reloc -> not in-process-eligible */
        *(uint64_t *)(base + r->r_offset) = base + (uint64_t)r->r_addend;
    }
    return 0;
}

/*
 * Locate an OVMX symbol-vector section (.vms$imp / .vms$rel) by name via the
 * image's ELF section-header table, returning its image-relative vaddr + size.
 * Returns 1 on hit (fills addr and size), 0 otherwise. Re-homed from imgact.c's
 * ovmx_find_section but over lseek/read (DECC$SHR universals) instead of
 * imgact's freestanding sys_pread. Bounded section/strtab counts keep it
 * allocation-free. (vms-db2)
 */
static int imgact_find_section(int fd, const char *want,
                               unsigned long *addr, unsigned long *size)
{
    Elf64_Ehdr eh;
    Elf64_Shdr sh[64];
    static char strtab[4096];   /* file-scope-sized; activation is serial */
    unsigned long ssz, stsz;

    if (imgact_readat(fd, &eh, sizeof eh, 0) != 0)
        return 0;
    if (eh.e_shnum == 0 || eh.e_shnum > 64 || eh.e_shentsize != sizeof(Elf64_Shdr) ||
        eh.e_shstrndx >= eh.e_shnum)
        return 0;
    ssz = (unsigned long)eh.e_shnum * sizeof(Elf64_Shdr);
    if (imgact_readat(fd, sh, ssz, (long)eh.e_shoff) != 0)
        return 0;
    stsz = sh[eh.e_shstrndx].sh_size;
    if (stsz == 0 || stsz > sizeof strtab)
        return 0;
    if (imgact_readat(fd, strtab, stsz, (long)sh[eh.e_shstrndx].sh_offset) != 0)
        return 0;
    for (int i = 0; i < eh.e_shnum; i++) {
        if (sh[i].sh_name >= stsz)
            continue;
        const char *nm = strtab + sh[i].sh_name;
        unsigned long j = 0;
        while (want[j] && nm[j] == want[j])
            j++;
        if (want[j] == '\0' && nm[j] == '\0') {
            *addr = sh[i].sh_addr;
            *size = sh[i].sh_size;
            return 1;
        }
    }
    return 0;
}

/*
 * Apply an OVMX .vms$rel self-relative fixup table: add the load bias to every
 * image-relative 8-byte slot LINK.EXE recorded (the VMS-native equivalent of
 * R_*_RELATIVE without a PT_DYNAMIC). No-op when the image has no .vms$rel. The
 * target pages must be writable (they are: activation reprotects to final perms
 * AFTER this). (vms-db2, re-homed from imgact.c apply_vms_rel.)
 */
static void imgact_apply_vms_rel(unsigned long base,
                                 unsigned long rel_addr, unsigned long rel_size)
{
    const struct ovmx_rel_header *rh =
        (const struct ovmx_rel_header *)(base + rel_addr);
    if (rel_size < sizeof *rh || rh->magic != OVMX_REL_MAGIC)
        return;
    const uint64_t *off = (const uint64_t *)((const char *)rh + sizeof *rh);
    for (uint32_t k = 0; k < rh->count; k++)
        *(uint64_t *)(uintptr_t)(base + off[k]) += base;
}

/*
 * Give an in-process image with its OWN PT_TLS a DISTINCT TLS block whose
 * __thread data is its own, WITHOUT reprogramming the process thread pointer
 * (vms-db2, docs/design-in-process-activation.md §A.8 remainder item 4 -- the
 * DTV-append / TLS-absorb-over-resident-C-RTL case). Reprogramming TP is exactly
 * what IMGACT.EXE does for a FRESH-process image (setup_tls); doing it here would
 * clobber DCL's own TLS and every resident universal's TLS -- the LARP risk #236
 * fenced. This re-homes IMGACT.EXE's absorb_tls_over_crtl (src/imgact/imgact.c,
 * bead vms-616) as an in-process library routine:
 *
 *   1. Allocate the image its OWN per-thread block [.tdata init image | .tbss 0].
 *   2. Read DCL's already-programmed TP; delta = block - TP (signed, wraps).
 *   3. Bias each OVMX .vms$tls TLSDESC entry: word[0] = the resident resolver
 *      (ovmx_tlsdesc_static), word[1] += delta (LINK pre-fills word[1] with the
 *      variable's MODULE-relative offset). The standard TLSDESC access
 *      (TP + resolver()) then computes TP + (block - TP) + module_off =
 *      block + module_off -- INSIDE the image's own block -- while TP, DCL's TLS
 *      and the resident universals' TLS are untouched.
 *
 * This is the SAME arithmetic on both TLS variants (Drepper I aarch64 / II
 * x86_64): the block's real address minus TP absorbs the variant's sign, so no
 * per-variant math is needed here (IMGACT.EXE's variant math is only for the
 * fresh-TP layout this routine deliberately avoids).
 *
 * FENCED (INV-6, no half-done TLS): only the OVMX .vms$tls carrier -- a
 * TLSDESC-model own-TLS image, which LINK.EXE emits with NO PT_DYNAMIC -- is
 * handled. A PT_TLS image with NO .vms$tls (a fixed local-exec/initial-exec
 * TP-offset model, whose accesses bake a TP offset we cannot place beside DCL's
 * live TLS) is refused SS$_UNSUPPORTED so the caller forks. Single image module
 * only: a producer in the graph carrying its OWN PT_TLS stays deferred
 * (imgact_map_producer already refuses that class). The block is process-local
 * mmap (like absorb_tls_over_crtl's), freed by the caller at rundown.
 *
 * Returns SS$_NORMAL (block set in out_blk / out_len), SS$_UNSUPPORTED (deferred
 * class -> fork), or SS$_INSFMEM. Pure userspace -- no /dev/vms.
 */
static uint32_t imgact_setup_own_tls(unsigned long base,
                                     unsigned long tls_image,
                                     unsigned long tls_filesz,
                                     unsigned long tls_memsz,
                                     unsigned long tls_align,
                                     unsigned long tls_sec_addr,
                                     unsigned long tls_sec_size,
                                     void **out_blk, unsigned long *out_len)
{
    /* The .vms$tls carrier is REQUIRED (the TLSDESC-model own-TLS class). A
     * PT_TLS image lacking it uses a fixed TP-offset model we cannot place
     * beside DCL's TLS -> refuse (fork). */
    if (!tls_sec_addr || tls_sec_size < sizeof(struct ovmx_tls_header))
        return SS$_UNSUPPORTED;
    const struct ovmx_tls_header *th =
        (const struct ovmx_tls_header *)(base + tls_sec_addr);
    if (th->magic != OVMX_TLS_MAGIC || th->count == 0 || th->count > 4096)
        return SS$_UNSUPPORTED;
    /* The count entry_off[] must fit inside the located section. */
    if (tls_sec_size < sizeof *th + (unsigned long)th->count * sizeof(uint64_t))
        return SS$_UNSUPPORTED;
    /* A per-thread block is a page-aligned mmap; alignment beyond a page is not
     * supported (matches IMGACT.EXE's absorb_tls_over_crtl overalignment fence). */
    if (tls_align > IMGACT_PGSZ)
        return SS$_UNSUPPORTED;

    unsigned long len = IMGACT_ROUND(tls_memsz ? tls_memsz : 1);
    void *blk = mmap(NULL, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (blk == MAP_FAILED)
        return SS$_INSFMEM;
    if (tls_filesz)
        memcpy(blk, (void *)tls_image, tls_filesz);
    /* .tbss tail stays zero (anonymous mapping). */

    /* delta: signed distance from DCL's TP to this block's base. TLSDESC returns
     * module_off + delta; TP + that == block + module_off. */
    unsigned long delta = (unsigned long)blk - imgact_read_tp();
    const uint64_t *eo = (const uint64_t *)(base + tls_sec_addr + sizeof *th);
    for (uint32_t k = 0; k < th->count; k++) {
        unsigned long *desc = (unsigned long *)(base + (unsigned long)eo[k]);
        desc[0] = (unsigned long)&ovmx_tlsdesc_static;   /* resident resolver   */
        desc[1] += delta;   /* module-relative -> TP-relative bias (vms-db2)    */
    }
    *out_blk = blk;
    *out_len = len;
    return SS$_NORMAL;
}

/*
 * SYS$SHARE search directory for a producer named only by soname -- the same
 * fallback SYSLIB IMGACT.EXE uses (src/imgact/imgact.c IMGACT_FALLBACK_SYSLIB).
 * A producer whose .vms$imp soname is an absolute/relative path is opened as
 * given first.
 */
#define IMGACT_SYSSHARE "/vms/SYS0/SYSCOMMON/SYSLIB"

static int imgact_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

/* Join dir + "/" + name into buf (bounded); returns 0 on success, -1 if it does
 * not fit. Freestanding-safe (no strlen/strcpy universal needed here). */
static int imgact_pathjoin(char *buf, unsigned long bufsz,
                           const char *dir, const char *name)
{
    unsigned long i = 0;
    for (const char *p = dir; *p; p++) {
        if (i + 1 >= bufsz) return -1;
        buf[i++] = *p;
    }
    if (i + 1 >= bufsz) return -1;
    buf[i++] = '/';
    for (const char *p = name; *p; p++) {
        if (i + 1 >= bufsz) return -1;
        buf[i++] = *p;
    }
    buf[i] = '\0';
    return 0;
}

/*
 * Does a mapped producer's .vms$sv export musl's C-RTL bootstrap __init_libc?
 * That marks it a C-RTL producer (DECC$SHR) whose activation would drive
 * musl's __init_libc and REPROGRAM the process thread pointer -- fatal to run
 * in DCL's own process (it would clobber DCL's TLS). Such a producer is refused
 * in-process (the deferred remainder); the consumer forks. Reached by NAME
 * (the .vms$sv name blob exists for diagnostics + exactly this), not by index.
 */
static int imgact_producer_needs_crtl(const struct ovmx_sv_header *sv)
{
    if (!sv || sv->magic != OVMX_SV_MAGIC)
        return 0;
    const struct ovmx_sv_entry *e = ovmx_sv_entries(sv);
    const char *names = ovmx_sv_names(sv);
    for (uint32_t k = 0; k < sv->count; k++) {
        if (e[k].kind == OVMX_SV_RETIRED)
            continue;
        if (imgact_streq(names + e[k].name_off, "__init_libc"))
            return 1;
    }
    return 0;
}

/*
 * Map a NON-RESIDENT producer shareable into DCL's process and register it, so
 * a consumer's .vms$imp import can bind to it (vms-db2, §A.8 remainder item 1 --
 * the non-resident producer sub-step). This re-homes src/imgact/imgact.c's
 * load_ovmx_producer() as an in-process library routine: it maps the producer's
 * PT_LOADs with a PLAIN mmap (NOT the P0 window -- a shareable is process-
 * permanent on VMS, surviving image rundown, which is also what lets a second
 * consumer bind to the SAME instance), applies its .vms$rel self-relative
 * fixups, locates its .vms$sv, registers it (imgact_register_producer), and
 * recursively binds its OWN .vms$imp against the registry (transitive graph).
 *
 * FENCED to the class the design proves here (the rest stays deferred/forks,
 * SS$_UNSUPPORTED): a producer carrying its OWN PT_TLS, a PT_INTERP, a
 * PT_DYNAMIC (symbolic relocs), or a C-RTL bootstrap (__init_libc -- it would
 * reprogram the thread pointer, §A.8 item 4) is refused. A pure leaf/
 * computational OVMX symbol-vector shareable maps safely in-process.
 *
 * Returns SS$_NORMAL if the producer is now resident; SS$_UNSUPPORTED if it is
 * not found, malformed, or of a deferred class (the caller forks); SS$_INSFMEM
 * on an mmap failure. Pure userspace -- no /dev/vms (the executive-mediated P0/
 * mode/rundown that wrap the consumer's own activation stay in imgact_activate).
 */
static uint32_t imgact_map_producer(const char *soname)
{
    Elf64_Ehdr eh;
    Elf64_Phdr ph[IMGACT_MAXPHDR];
    char path[256];
    int fd = -1, i;

    if (!soname || soname[0] == '\0')
        return SS$_BADPARAM;
    /* Already resident (a re-entrant find; also dedups a diamond dependency). */
    if (imgact_find_producer(soname, 0, 0))
        return SS$_NORMAL;

    /* Search: the name as given (absolute/relative), then SYS$SHARE (SYSLIB). */
    fd = open(soname, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (imgact_pathjoin(path, sizeof path, IMGACT_SYSSHARE, soname) == 0)
            fd = open(path, O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0)
        return SS$_UNSUPPORTED;   /* not found -> consumer forks */

    if (imgact_readat(fd, &eh, sizeof eh, 0) != 0 ||
        memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_type != ET_DYN || eh.e_machine != IMGACT_EM ||
        eh.e_phnum == 0 || eh.e_phnum > IMGACT_MAXPHDR) {
        close(fd);
        return SS$_UNSUPPORTED;
    }
    if (imgact_readat(fd, ph, sizeof(Elf64_Phdr) * eh.e_phnum,
                      (long)eh.e_phoff) != 0) {
        close(fd);
        return SS$_UNSUPPORTED;
    }

    /* Fence: an OWN PT_TLS or a PT_INTERP is a deferred class -> fork. (A
     * PT_DYNAMIC is NOT rejected here: every -shared ET_DYN carries one, but a
     * pure OVMX symbol-vector producer has no symbolic relocations through it --
     * imgact_relocate below rejects any that are symbolic, applying only
     * R_*_RELATIVE, exactly as the consumer path does.) */
    unsigned long lo = ~0UL, hi = 0;
    for (i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type == PT_TLS || ph[i].p_type == PT_INTERP) {
            close(fd);
            return SS$_UNSUPPORTED;
        }
        if (ph[i].p_type == PT_LOAD) {
            unsigned long a = IMGACT_TRUNC(ph[i].p_vaddr);
            unsigned long z = IMGACT_ROUND(ph[i].p_vaddr + ph[i].p_memsz);
            if (a < lo) lo = a;
            if (z > hi) hi = z;
        }
    }
    if (lo == ~0UL || hi <= lo) {
        close(fd);
        return SS$_UNSUPPORTED;
    }

    /* Locate the OVMX symbol-vector sections while the file is open (image-
     * relative addrs; applied against the mapped image below). */
    unsigned long sv_addr = 0, sv_size = 0, imp_addr = 0, imp_size = 0;
    unsigned long rel_addr = 0, rel_size = 0;
    if (!imgact_find_section(fd, OVMX_SV_SECTION, &sv_addr, &sv_size)) {
        close(fd);
        return SS$_UNSUPPORTED;   /* not an OVMX producer -> fork */
    }
    int have_imp = imgact_find_section(fd, OVMX_IMP_SECTION, &imp_addr, &imp_size);
    int have_rel = imgact_find_section(fd, OVMX_REL_SECTION, &rel_addr, &rel_size);

    /* Map the producer's span with a PLAIN mmap (process-permanent, NOT the P0
     * window): reserve [lo,hi), read each PT_LOAD, then reprotect to final. */
    unsigned long span = hi - lo;
    void *map = mmap(NULL, span, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return SS$_INSFMEM;
    }
    unsigned long base = (unsigned long)map - lo;   /* load bias */
    for (i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_filesz == 0)
            continue;
        if (imgact_readat(fd, (void *)(base + ph[i].p_vaddr),
                          ph[i].p_filesz, (long)ph[i].p_offset) != 0) {
            munmap(map, span);
            close(fd);
            return SS$_UNSUPPORTED;
        }
    }
    close(fd);

    const struct ovmx_sv_header *sv =
        (const struct ovmx_sv_header *)(base + sv_addr);
    if (sv->magic != OVMX_SV_MAGIC || imgact_producer_needs_crtl(sv)) {
        munmap(map, span);
        return SS$_UNSUPPORTED;   /* C-RTL producer -> deferred, fork */
    }

    /* Apply any R_*_RELATIVE relocations through PT_DYNAMIC and refuse a
     * symbolic PLT/reloc (a producer needing symbolic ELF binding is the
     * deferred class -> fork). Pages are still writable here. Then bias the
     * producer's own .vms$rel self-relative slots. Both no-ops for a pure PIC
     * symbol-vector producer that carries neither. */
    if (imgact_relocate(base, ph, eh.e_phnum) != 0) {
        munmap(map, span);
        return SS$_UNSUPPORTED;
    }
    if (have_rel)
        imgact_apply_vms_rel(base, rel_addr, rel_size);

    /* Reprotect each PT_LOAD to its final permissions. */
    for (i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        unsigned long a = IMGACT_TRUNC(ph[i].p_vaddr);
        unsigned long z = IMGACT_ROUND(ph[i].p_vaddr + ph[i].p_memsz);
        int pr = 0;
        if (ph[i].p_flags & PF_R) pr |= PROT_READ;
        if (ph[i].p_flags & PF_W) pr |= PROT_WRITE;
        if (ph[i].p_flags & PF_X) pr |= PROT_EXEC;
        mprotect((void *)(base + a), z - a, pr);
    }

    /* Register it resident BEFORE resolving its own imports, so a dependency
     * cycle dedups through imgact_find_producer (imgact.c's ordering). */
    uint32_t status = imgact_register_producer(soname, (uint64_t)base, sv);
    if (!(status & 1)) {
        munmap(map, span);
        return status;
    }

    /* Transitively bind the producer's OWN .vms$imp against the registry (a lib
     * shareable importing from another producer), mapping further non-resident
     * producers recursively. If a dependency cannot be bound the whole producer
     * is unusable in-process -> refuse so the consumer forks. */
    if (have_imp) {
        status = imgact_bind_imports_mapped(
            (uint64_t)base,
            (const struct ovmx_imp_header *)(base + imp_addr),
            imgact_map_producer);
        if (!(status & 1))
            return status;   /* left registered; harmless (it is genuinely
                              * mapped, only its deps failed -- a later attempt
                              * re-finds it resident and re-checks). */
    }
    return SS$_NORMAL;
}

uint32_t imgact_activate(const char *path, long a0, long a1,
                         const struct imgact_critp1 *critp1,
                         int *image_rc)
{
    Elf64_Ehdr eh;
    Elf64_Phdr ph[IMGACT_MAXPHDR];
    int fd, i;
    unsigned long base, span_hi = 0;
    /* OVMX symbol-vector cross-image import + self-relative fixup sections
     * (vms-db2). Located from the file while fd is open; applied after mapping. */
    unsigned long imp_addr = 0, imp_size = 0, rel_addr = 0, rel_size = 0;
    int have_imp = 0, have_rel = 0;
    /* Own-PT_TLS geometry (vms-db2, §A.8 item 4). Captured from the PT_TLS phdr;
     * the per-image TLS block is allocated and its .vms$tls TLSDESC entries
     * biased against DCL's TP before the image is entered, and the block is freed
     * at rundown (tls_blk volatile: set before the enter/setjmp, freed in the
     * common teardown after a possible longjmp). */
    int have_own_tls = 0;
    unsigned long tls_vaddr = 0, tls_filesz = 0, tls_memsz = 0, tls_align = 1;
    unsigned long tls_sec_addr = 0, tls_sec_size = 0;
    void * volatile tls_blk = MAP_FAILED;
    volatile unsigned long tls_blk_len = 0;
    /* volatile: read/used after a possible longjmp out of the image (a fault),
     * so their values must not be left indeterminate by the non-local jump. */
    void * volatile win = MAP_FAILED;
    volatile long image_ret = -1;
    uint32_t status;

    if (image_rc)
        *image_rc = -1;
    if (IMGACT_EM == 0)
        return SS$_UNSUPPORTED;   /* unsupported host arch */

    /* ---- 1. Parse + eligibility, WITHOUT touching the executive -------- */
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return SS$_NOSUCHFILE;
    if (imgact_readat(fd, &eh, sizeof eh, 0) != 0 ||
        memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_ident[EI_CLASS] != ELFCLASS64) {
        close(fd);
        return SS$_BADPARAM;
    }
    /* ET_DYN (position-independent), our machine, sane phnum. */
    if (eh.e_type != ET_DYN || eh.e_machine != IMGACT_EM ||
        eh.e_phnum == 0 || eh.e_phnum > IMGACT_MAXPHDR) {
        close(fd);
        return SS$_UNSUPPORTED;
    }
    if (imgact_readat(fd, ph, sizeof(Elf64_Phdr) * eh.e_phnum,
                      (long)eh.e_phoff) != 0) {
        close(fd);
        return SS$_BADPARAM;
    }
    unsigned long interp_off = 0, interp_sz = 0;
    for (i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type == PT_INTERP) {
            /* An external LINK.EXE image names its loader here. In-process
             * activation IS the loader, so we DON'T reject on sight (as this
             * did before the external-image flip); capture the interp string's
             * location and validate below that it names the OVMX loader. An
             * interp naming a foreign loader still forks (imgact_interp_is_ours
             * -> SS$_UNSUPPORTED). (vms-db2, §A.8 remainder item 2) */
            interp_off = ph[i].p_offset;
            interp_sz  = ph[i].p_filesz;
            continue;
        }
        /* Own PT_TLS (vms-db2, §A.8 item 4): capture the geometry; the image
         * gets its OWN TLS block, biased against DCL's TP (imgact_setup_own_tls
         * below), rather than forking. Handled only for the OVMX .vms$tls
         * TLSDESC carrier -- a PT_TLS image without .vms$tls is refused there and
         * forks (no half-done TLS). .tbss is a per-thread template, not mapped
         * memory, so PT_TLS does NOT extend span_hi (the PT_LOAD holding .tdata
         * does). */
        if (ph[i].p_type == PT_TLS) {
            have_own_tls = 1;
            tls_vaddr  = ph[i].p_vaddr;
            tls_filesz = ph[i].p_filesz;
            tls_memsz  = ph[i].p_memsz;
            tls_align  = ph[i].p_align ? ph[i].p_align : 1;
            continue;
        }
        if (ph[i].p_type == PT_LOAD &&
            ph[i].p_vaddr + ph[i].p_memsz > span_hi)
            span_hi = ph[i].p_vaddr + ph[i].p_memsz;
    }
    /* A PT_INTERP that does not name the OVMX loader is a foreign interpreter
     * (a real ld.so, a shebang) we must not run in-process -- fork it. Checked
     * from the still-open fd, before any mmap or executive call. */
    if (interp_sz && !imgact_interp_is_ours(fd, interp_off, interp_sz)) {
        close(fd);
        return SS$_UNSUPPORTED;
    }
    if (span_hi == 0 || IMGACT_ROUND(span_hi) > IMGACT_P0_WINDOW) {
        close(fd);
        return SS$_UNSUPPORTED;
    }

    /* Locate the OVMX cross-image import table (.vms$imp) and self-relative
     * fixup table (.vms$rel), if any, while the file is still open. A zero-
     * import image (e.g. the freestanding TESTIMG.EXE) has neither, so this is
     * a no-op and its activation path is byte-for-byte what increment iv did. */
    have_imp = imgact_find_section(fd, OVMX_IMP_SECTION, &imp_addr, &imp_size);
    have_rel = imgact_find_section(fd, OVMX_REL_SECTION, &rel_addr, &rel_size);
    /* .vms$tls: the OVMX TLSDESC-entry carrier for an own-PT_TLS image (vms-db2).
     * Located while the file is open; the image's TLSDESC descriptors it points
     * at are biased below (imgact_setup_own_tls), after the image is mapped. */
    if (have_own_tls)
        (void)imgact_find_section(fd, OVMX_TLS_SECTION, &tls_sec_addr, &tls_sec_size);

    /* ---- 2. Reserve the P0 window and map the image into it ----------- */
    win = mmap(NULL, IMGACT_P0_WINDOW, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (win == MAP_FAILED) {
        close(fd);
        return SS$_INSFMEM;
    }
    base = (unsigned long)win;   /* ET_DYN load bias (linked at 0) */

    for (i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        unsigned long va  = IMGACT_TRUNC(ph[i].p_vaddr);
        unsigned long end = IMGACT_ROUND(ph[i].p_vaddr + ph[i].p_memsz);
        void *m = mmap((void *)(base + va), end - va,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (m == MAP_FAILED) { status = SS$_INSFMEM; goto out_unmap; }
        if (ph[i].p_filesz &&
            imgact_readat(fd, (void *)(base + ph[i].p_vaddr),
                          ph[i].p_filesz, (long)ph[i].p_offset) != 0) {
            status = SS$_BADPARAM; goto out_unmap;
        }
    }
    close(fd);
    fd = -1;

    /* Positive in-process marker: nothing that lacks it takes this path. Its
     * descriptor selects the entry ABI -- IMGACT_ABI_AUXV (a real image entered
     * through SysV _start) vs. the increment-iv (a0,a1) function-call ABI. */
    long abi = imgact_marker_desc(base, ph, eh.e_phnum);
    if (abi < 0) {
        status = SS$_UNSUPPORTED;
        goto out_unmap;
    }
    if (imgact_relocate(base, ph, eh.e_phnum) != 0) {
        status = SS$_UNSUPPORTED;
        goto out_unmap;
    }

    /* OVMX symbol-vector cross-image binding (vms-db2). Segments are still RW
     * here (reprotect is below), so both the .vms$rel self-relative slots and
     * the .vms$imp GOT cells are writable. Apply .vms$rel first (bias the
     * image's own self-relative slots), THEN bind imports so a .vms$sv value a
     * producer exports has already been biased before a consumer reads it --
     * the same order imgact.c uses. Each import binds to an ALREADY-RESIDENT
     * producer (imgact_prodreg); an image naming a producer that is not
     * resident cannot be activated in-process and returns SS$_UNSUPPORTED, so
     * dcl_activate_image forks it (no LARP: never a private producer copy). */
    if (have_rel)
        imgact_apply_vms_rel(base, rel_addr, rel_size);
    if (have_imp) {
        /* Bind each import to an ALREADY-RESIDENT producer; a named producer
         * that is NOT resident is mapped in-process (imgact_map_producer, the
         * NON-RESIDENT producer sub-step, vms-db2) and bound to. A producer we
         * cannot safely map in-process (PT_TLS / C-RTL bootstrap -- deferred)
         * leaves the import unbound -> SS$_UNSUPPORTED -> the caller forks. No
         * LARP: never a private producer copy. */
        status = imgact_bind_imports_mapped(
            (uint64_t)base,
            (const struct ovmx_imp_header *)(base + imp_addr),
            imgact_map_producer);
        if (!(status & 1))
            goto out_unmap;   /* unbound import -> caller keeps the fork path */
    }

    /* Own-PT_TLS: give the image its own TLS block, biased against DCL's TP, so
     * its __thread data is ITS OWN while DCL's TP/TLS and the resident universals'
     * TLS are untouched (vms-db2, §A.8 item 4). Done while segments are still RW
     * (the .vms$tls descriptors it writes live in a writable image section) and
     * BEFORE any executive call, so a deferred-class TLS image (no .vms$tls) forks
     * with no /dev/vms interaction (INV-6). The block is freed at rundown. */
    if (have_own_tls) {
        void *blk = NULL;
        unsigned long blen = 0;
        status = imgact_setup_own_tls(base, base + tls_vaddr, tls_filesz,
                                      tls_memsz, tls_align,
                                      tls_sec_addr, tls_sec_size, &blk, &blen);
        if (!(status & 1))
            goto out_unmap;   /* deferred TLS class / OOM -> caller forks */
        tls_blk = blk;
        tls_blk_len = blen;
    }

    /* Reprotect each segment to its final PT_LOAD permissions. */
    for (i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        unsigned long va  = IMGACT_TRUNC(ph[i].p_vaddr);
        unsigned long end = IMGACT_ROUND(ph[i].p_vaddr + ph[i].p_memsz);
        int pr = 0;
        if (ph[i].p_flags & PF_R) pr |= PROT_READ;
        if (ph[i].p_flags & PF_W) pr |= PROT_WRITE;
        if (ph[i].p_flags & PF_X) pr |= PROT_EXEC;
        mprotect((void *)(base + va), end - va, pr);
    }

    /* ---- 3. Register the P0 extent with the executive (INV-6) --------- */
    status = vms_kif_p0_map((uint64_t)base, (uint64_t)(base + IMGACT_ROUND(span_hi)));
    if (!(status & 1))
        goto out_unmap;   /* SS$_NOSUCHDEV etc. -- image NOT run */

    /* ---- 4. Protect critical P1, descend to User, enter the image ----- */
    if (critp1 && critp1->limit > critp1->base) {
        status = vms_kif_p1_protect(critp1->base, critp1->limit, 0);
        if (!(status & 1))
            goto out_p0;
    }

    status = vms_kif_enter_image(NULL, NULL);   /* Supervisor -> User */
    if (!(status & 1)) {
        if (critp1 && critp1->limit > critp1->base)
            vms_kif_p1_protect(critp1->base, critp1->limit, 1);
        goto out_p0;
    }

    {
        struct sigaction sa, old;
        unsigned long entry_va = base + eh.e_entry;
        /* For the auxv ABI, build the User-mode stack up front (before the mode
         * descent / setjmp) so a failure here is a clean refusal, not a mid-run
         * abort. The stack lives at the top of the P0 window and is freed with
         * the whole window at rundown. */
        unsigned long sp = 0;
        if (abi & IMGACT_ABI_AUXV) {
            unsigned long stk_hi = base + IMGACT_P0_WINDOW;
            unsigned long stk_lo = stk_hi - IMGACT_IMG_STACK;
            /* Never let the image span and its User stack overlap. */
            if (stk_lo < base + IMGACT_ROUND(span_hi)) {
                status = SS$_UNSUPPORTED;
                goto out_enter_fail;
            }
            if (mmap((void *)stk_lo, IMGACT_IMG_STACK, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
                status = SS$_INSFMEM;
                goto out_enter_fail;
            }
            sp = imgact_build_auxv_stack(stk_lo, stk_hi, entry_va,
                                         base + eh.e_phoff, eh.e_phentsize,
                                         eh.e_phnum);
            if (sp == 0) {
                status = SS$_INSFMEM;
                goto out_enter_fail;
            }
        }

        memset(&sa, 0, sizeof sa);
        sa.sa_handler = imgact_segv;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGSEGV, &sa, &old);

        g_faulted = 0;
        g_exit_code = 0;
        g_inproc_active = 1;   /* SYS$EXIT now unwinds instead of terminating */
        int jv = setjmp(g_img_jb);
        if (jv == 0) {
            if (abi & IMGACT_ABI_AUXV) {
                /* Enter the real image at _start on its own P0 stack. It leaves
                 * ONLY by calling SYS$EXIT (imgact_image_exit -> longjmp EXIT) or
                 * by faulting (SIGSEGV -> longjmp FAULT); imgact_enter_auxv never
                 * returns here. */
                imgact_enter_auxv(entry_va, sp);
            } else {
                imgact_entryfn entry = (imgact_entryfn)entry_va;
                image_ret = entry(a0, a1);   /* (a0,a1) ABI: returns normally */
            }
        } else if (jv == IMGACT_JB_EXIT) {
            image_ret = g_exit_code;         /* SYS$EXIT returned to DCL */
        } else {
            /* Recovered from a fault. longjmp (not siglongjmp) left SIGSEGV
             * blocked in the process mask; unblock it explicitly. */
            sigset_t unb;
            sigemptyset(&unb);
            sigaddset(&unb, SIGSEGV);
            sigprocmask(SIG_UNBLOCK, &unb, NULL);
        }
        g_inproc_active = 0;

        sigaction(SIGSEGV, &old, NULL);
    }

    /* ---- 5. Rundown: restore Supervisor, unprotect P1, delete P0 ------ */
    vms_kif_image_rundown(NULL, NULL);
    if (critp1 && critp1->limit > critp1->base)
        vms_kif_p1_protect(critp1->base, critp1->limit, 1);

    status = g_faulted ? SS$_ACCVIO : SS$_NORMAL;
    if (!g_faulted && image_rc)
        *image_rc = (int)image_ret;
    goto out_p0;

out_enter_fail:
    /* Failed to prepare the User-mode image stack after descending to User mode:
     * run the image down (restores Supervisor) and unprotect P1, preserving the
     * failure `status` (do NOT overwrite it with the normal-run verdict). */
    g_inproc_active = 0;
    vms_kif_image_rundown(NULL, NULL);
    if (critp1 && critp1->limit > critp1->base)
        vms_kif_p1_protect(critp1->base, critp1->limit, 1);

out_p0:
    {
        uint64_t ob = 0, ol = 0;
        vms_kif_p0_unmap(&ob, &ol);   /* executive marks the process image-less */
    }
out_unmap:
    if (fd >= 0)
        close(fd);
    if (tls_blk != MAP_FAILED)
        munmap((void *)tls_blk, tls_blk_len);   /* free the per-image TLS block */
    if (win != MAP_FAILED)
        munmap(win, IMGACT_P0_WINDOW);   /* collapse the P0 window */
    return status;
}
