/*
 * sys_imgact.c - in-process image activation library (vms-68f increment iv).
 *
 * imgact_activate() runs an image IN THE CURRENT PROCESS instead of the
 * fork()+execve() model src/vmsdcl/dcl_cmd_process.c has always used: it maps
 * the image into a reserved P0 window (MAP_FIXED), records the extent with the
 * executive (vms_kif_p0_map), protects DCL's critical P1 pages
 * (vms_kif_p1_protect), transitions Supervisor->User (vms_kif_enter_image),
 * enters the image's entry on its own User-mode stack via swapcontext, and on
 * the image's return runs it down (vms_kif_image_rundown / vms_kif_p1_protect
 * restore / vms_kif_p0_unmap) and swapcontexts back to DCL -- same PID
 * throughout. See docs/design-in-process-activation.md Part II §A.2 and
 * src/libvms/include/imgact_activate.h for the model, the eligibility gate and
 * the ceiling of THIS increment.
 *
 * INV-6 (CLAUDE.md Rule 9). Every executive step goes through /dev/vms; with
 * no executive the vms_kif_* calls return SS$_NOSUCHDEV and this function
 * refuses to run the image -- it never silently falls back to a per-process
 * "dlopen and pretend". The eligibility decision (SS$_UNSUPPORTED for any
 * non-in-process image) is made from the ELF alone, BEFORE any executive call,
 * so a real image's fork fallback does not depend on /dev/vms being present.
 *
 * Rule 8: the ELF/PT_LOAD/PT_NOTE/R_*_RELATIVE mechanics are the public System
 * V ABI and OVMX's own in-process marker (imgact_activate.h); no VMS byte
 * format is claimed. Rule 10: what is enforced (P0 teardown at the MMU,
 * critical-P1 mprotect, service-boundary mode) vs. modeled is stated in the
 * design; this file implements the enforced half.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <elf.h>
#include <signal.h>
#include <setjmp.h>
#include <ucontext.h>
#include <sys/mman.h>

#include "ssdef.h"
#include "vms_kif.h"
#include "imgact_activate.h"

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
 * (design §A.2.1). 64 MiB covers any freestanding OVMX image plus its stack. */
#define IMGACT_P0_WINDOW   (64UL * 1024 * 1024)
#define IMGACT_IMG_STACK   (256UL * 1024)
#define IMGACT_PGSZ        4096UL
#define IMGACT_TRUNC(x)    ((x) & ~(IMGACT_PGSZ - 1))
#define IMGACT_ROUND(x)    (((x) + IMGACT_PGSZ - 1) & ~(IMGACT_PGSZ - 1))
#define IMGACT_MAXPHDR     64

typedef long (*imgact_entryfn)(long, long);

/*
 * Activation is not reentrant (VMS runs one image at a time per process), so
 * the context the SIGSEGV recovery and the swapcontext trampoline need is
 * file-scope. A nested in-process activation is out of scope for this
 * increment; the entry gate refuses a second ENTER_IMAGE while one is open.
 */
static ucontext_t     g_return_ctx;
static ucontext_t     g_image_ctx;
static imgact_entryfn g_entry;
static long           g_a0, g_a1;
static long           g_image_ret;
static sigjmp_buf     g_fault_jb;
static volatile sig_atomic_t g_faulted;

static void imgact_trampoline(void)
{
    g_image_ret = g_entry(g_a0, g_a1);
}

/*
 * SIGSEGV while the image runs (e.g. a wild write to a protected critical-P1
 * page) is converted to $EXIT(SS$_ACCVIO)+rundown: siglongjmp back onto DCL's
 * stack, abandoning the image context. Design §A.6.3 (pragmatic first).
 */
static void imgact_segv(int sig)
{
    (void)sig;
    g_faulted = 1;
    siglongjmp(g_fault_jb, 1);
}

/* Scan PT_NOTE segments for the OVMX in-process marker. */
static int imgact_has_marker(unsigned long base, const Elf64_Phdr *ph, int n)
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
                return 1;
            p = desc + ((ds + 3u) & ~3u);
        }
    }
    return 0;
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

uint32_t imgact_activate(const char *path, long a0, long a1,
                         const struct imgact_critp1 *critp1,
                         int *image_rc)
{
    Elf64_Ehdr eh;
    Elf64_Phdr ph[IMGACT_MAXPHDR];
    int fd, i;
    unsigned long base, span_lo = ~0UL, span_hi = 0;
    void *win = MAP_FAILED, *stk = MAP_FAILED;
    uint32_t status;

    if (image_rc)
        *image_rc = -1;
    if (IMGACT_EM == 0)
        return SS$_UNSUPPORTED;   /* unsupported host arch */

    /* ---- 1. Parse + eligibility, WITHOUT touching the executive -------- */
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return SS$_NOSUCHFILE;
    if (pread(fd, &eh, sizeof eh, 0) != (long)sizeof eh ||
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
    if (pread(fd, ph, sizeof(Elf64_Phdr) * eh.e_phnum, eh.e_phoff) !=
        (long)(sizeof(Elf64_Phdr) * eh.e_phnum)) {
        close(fd);
        return SS$_BADPARAM;
    }
    for (i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type == PT_INTERP) {   /* wants the loader -> fork path */
            close(fd);
            return SS$_UNSUPPORTED;
        }
        if (ph[i].p_type == PT_LOAD) {
            if (ph[i].p_vaddr < span_lo) span_lo = ph[i].p_vaddr;
            if (ph[i].p_vaddr + ph[i].p_memsz > span_hi)
                span_hi = ph[i].p_vaddr + ph[i].p_memsz;
        }
    }
    if (span_hi == 0 || IMGACT_ROUND(span_hi) + IMGACT_IMG_STACK > IMGACT_P0_WINDOW) {
        close(fd);
        return SS$_UNSUPPORTED;
    }

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
            pread(fd, (void *)(base + ph[i].p_vaddr), ph[i].p_filesz,
                  ph[i].p_offset) != (long)ph[i].p_filesz) {
            status = SS$_BADPARAM; goto out_unmap;
        }
    }
    close(fd);
    fd = -1;

    /* Positive in-process marker: nothing that lacks it takes this path. */
    if (!imgact_has_marker(base, ph, eh.e_phnum)) {
        status = SS$_UNSUPPORTED;
        goto out_unmap;
    }
    if (imgact_relocate(base, ph, eh.e_phnum) != 0) {
        status = SS$_UNSUPPORTED;
        goto out_unmap;
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

    /* Image's own User-mode stack, inside the P0 window's tail. */
    stk = mmap((void *)(base + IMGACT_P0_WINDOW - IMGACT_IMG_STACK),
               IMGACT_IMG_STACK, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (stk == MAP_FAILED) { status = SS$_INSFMEM; goto out_p0; }

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
        stack_t     ss;
        struct sigaction sa, old;
        /* Fixed size, not SIGSTKSZ: glibc >= 2.34 makes SIGSTKSZ a sysconf()
         * call, not a compile-time constant usable as an array dimension. */
        static char altstk[32768];

        ss.ss_sp = altstk; ss.ss_size = sizeof altstk; ss.ss_flags = 0;
        sigaltstack(&ss, NULL);
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = imgact_segv;
        sa.sa_flags = SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &old);

        g_entry = (imgact_entryfn)(base + eh.e_entry);
        g_a0 = a0; g_a1 = a1; g_image_ret = -1; g_faulted = 0;

        getcontext(&g_image_ctx);
        g_image_ctx.uc_stack.ss_sp   = stk;
        g_image_ctx.uc_stack.ss_size = IMGACT_IMG_STACK;
        g_image_ctx.uc_link          = &g_return_ctx;
        makecontext(&g_image_ctx, imgact_trampoline, 0);

        if (sigsetjmp(g_fault_jb, 1) == 0)
            swapcontext(&g_return_ctx, &g_image_ctx);   /* runs the image */
        /* control is back in DCL's process, on DCL's stack, same PID */

        sigaction(SIGSEGV, &old, NULL);
    }

    /* ---- 5. Rundown: restore Supervisor, unprotect P1, delete P0 ------ */
    vms_kif_image_rundown(NULL, NULL);
    if (critp1 && critp1->limit > critp1->base)
        vms_kif_p1_protect(critp1->base, critp1->limit, 1);

    status = g_faulted ? SS$_ACCVIO : SS$_NORMAL;
    if (!g_faulted && image_rc)
        *image_rc = (int)g_image_ret;

out_p0:
    {
        uint64_t ob = 0, ol = 0;
        vms_kif_p0_unmap(&ob, &ol);   /* executive marks the process image-less */
    }
out_unmap:
    if (fd >= 0)
        close(fd);
    if (win != MAP_FAILED)
        munmap(win, IMGACT_P0_WINDOW);   /* collapse the P0 window (stk is inside it) */
    return status;
}
