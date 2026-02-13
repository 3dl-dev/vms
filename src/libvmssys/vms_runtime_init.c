/*
 * vms_runtime_init.c - Runtime initialization (no glibc)
 *
 * Parses the auxiliary vector, sets up TLS via arch_prctl(ARCH_SET_FS),
 * and initializes subsystems (stdio, etc).
 *
 * Called by crt0.S before main().
 */

#include "vms_runtime_init.h"
#include "vms_syscall.h"
#include "vms_string.h"
#include "vms_stdio.h"

/* ================================================================
 * Global state
 * ================================================================ */

char **vms_environ = NULL;
unsigned long vms_page_size = 4096;

/* Auxv cache */
#define MAX_AUXV 32
static struct {
    unsigned long type;
    unsigned long value;
} auxv_cache[MAX_AUXV];
static int auxv_count = 0;

/* ================================================================
 * TLS block layout
 *
 * GCC's x86_64 TLS model (local-exec) accesses thread-local variables
 * at negative offsets from the FS segment base.  The FS base itself
 * must point to a location that contains a self-pointer (the TCB).
 *
 * Layout:
 *   [TLS variables ...] [padding] [TCB with self-pointer at offset 0]
 *                                  ^--- FS base points here
 *
 * For now, we allocate a simple static TLS block.  A full implementation
 * would compute the TLS size from the PT_TLS program header.
 * ================================================================ */

/* Size of TLS region for thread-local variables */
#define TLS_REGION_SIZE 4096

/* Static TLS block for the main thread */
static char tls_block[TLS_REGION_SIZE + 64] __attribute__((aligned(64)));

/*
 * We find the TLS template via the ELF program headers (PT_TLS).
 * This is more reliable than weak linker symbols which may not
 * be emitted by all linkers.
 */

/* ELF program header type */
#define PT_TLS 7

/* ELF64 program header structure */
struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

/* Find the PT_TLS program header from auxv data */
static void find_tls_phdr(unsigned long *tls_addr, unsigned long *tls_filesz,
                          unsigned long *tls_memsz, unsigned long *tls_align)
{
    *tls_addr = 0;
    *tls_filesz = 0;
    *tls_memsz = 0;
    *tls_align = 0;

    unsigned long phdr_addr = vms_getauxval(VMS_AT_PHDR);
    unsigned long phent = vms_getauxval(VMS_AT_PHENT);
    unsigned long phnum = vms_getauxval(VMS_AT_PHNUM);

    if (!phdr_addr || !phent || !phnum)
        return;

    for (unsigned long i = 0; i < phnum; i++) {
        struct elf64_phdr *ph = (struct elf64_phdr *)(phdr_addr + i * phent);
        if (ph->p_type == PT_TLS) {
            *tls_addr = ph->p_vaddr;
            *tls_filesz = ph->p_filesz;
            *tls_memsz = ph->p_memsz;
            *tls_align = ph->p_align;
            return;
        }
    }
}

static void setup_tls(void)
{
    unsigned long tls_addr, tls_filesz, tls_memsz, tls_align;
    find_tls_phdr(&tls_addr, &tls_filesz, &tls_memsz, &tls_align);

#if defined(__x86_64__)
    /*
     * x86_64 TLS variant II: FS base points to TCB, thread-local
     * variables at negative offsets from FS base.
     *
     * Layout: [TLS data] [TCB with self-pointer at offset 0]
     *                      ^--- FS base
     */
    unsigned long tls_size = tls_memsz;
    if (tls_size == 0)
        tls_size = 64;

    /* Align tls_size up */
    if (tls_align > 0)
        tls_size = (tls_size + tls_align - 1) & ~(tls_align - 1);

    /* Place TCB after TLS data in our block */
    unsigned long fs_base = (unsigned long)&tls_block[0] + tls_size;
    fs_base = (fs_base + 15) & ~15UL;

    /* Self-pointer at offset 0 of TCB */
    *(unsigned long *)fs_base = fs_base;

    /* Copy .tdata template (initialized TLS data) */
    if (tls_filesz > 0 && tls_addr) {
        vms_memcpy((char *)(fs_base - tls_size), (void *)tls_addr, tls_filesz);
    }
    /* .tbss portion is already zero (tls_block is BSS) */

    vms_sys_arch_prctl(VMS_ARCH_SET_FS, fs_base);

#elif defined(__aarch64__)
    /*
     * AArch64 TLS variant I: TPIDR_EL0 points to TCB,
     * TLS data at positive offsets (TP + 16 + offset).
     *
     * The compiler generates: mrs x0, tpidr_el0; add x0, x0, #16; ldr ...
     */
    unsigned long tp = (unsigned long)&tls_block[0];
    tp = (tp + 15) & ~15UL;

    /* Zero the TCB area (first 16 bytes) */
    vms_memset((void *)tp, 0, 16);

    /* Copy .tdata template to TP + 16 */
    if (tls_filesz > 0 && tls_addr) {
        vms_memcpy((void *)(tp + 16), (void *)tls_addr, tls_filesz);
    }
    /* .tbss is zero (tls_block is BSS) */

    __asm__ volatile("msr tpidr_el0, %0" : : "r"(tp));
#endif
    (void)tls_block;
}

/* ================================================================
 * Auxiliary vector parsing
 * ================================================================ */

static void parse_auxv(char **envp)
{
    /* Walk past the environment to find auxv */
    char **p = envp;
    while (*p)
        p++;
    p++; /* skip NULL terminator */

    /* p now points to the auxv entries (pairs of unsigned long) */
    unsigned long *auxv = (unsigned long *)p;

    auxv_count = 0;
    while (auxv[0] != VMS_AT_NULL && auxv_count < MAX_AUXV) {
        auxv_cache[auxv_count].type = auxv[0];
        auxv_cache[auxv_count].value = auxv[1];
        auxv_count++;

        if (auxv[0] == VMS_AT_PAGESZ)
            vms_page_size = auxv[1];

        auxv += 2;
    }
}

unsigned long vms_getauxval(unsigned long type)
{
    for (int i = 0; i < auxv_count; i++) {
        if (auxv_cache[i].type == type)
            return auxv_cache[i].value;
    }
    return 0;
}

/* ================================================================
 * Environment access
 * ================================================================ */

char *vms_getenv(const char *name)
{
    if (!vms_environ || !name)
        return NULL;

    vms_size_t namelen = vms_strlen(name);
    for (char **p = vms_environ; *p; p++) {
        if (vms_strncmp(*p, name, namelen) == 0 && (*p)[namelen] == '=')
            return &(*p)[namelen + 1];
    }
    return NULL;
}

/* Simple setenv: modifies existing entry or does nothing if not found.
 * Full setenv (with allocation) requires the zone allocator (Phase 2). */
int vms_setenv(const char *name, const char *value)
{
    if (!vms_environ || !name || !value)
        return -1;

    vms_size_t namelen = vms_strlen(name);
    for (char **p = vms_environ; *p; p++) {
        if (vms_strncmp(*p, name, namelen) == 0 && (*p)[namelen] == '=') {
            /* Overwrite value in-place if it fits */
            vms_size_t oldvallen = vms_strlen(&(*p)[namelen + 1]);
            vms_size_t newvallen = vms_strlen(value);
            if (newvallen <= oldvallen) {
                vms_strcpy(&(*p)[namelen + 1], value);
                return 0;
            }
            /* Doesn't fit -- would need malloc; skip for now */
            return -1;
        }
    }
    return -1;
}

/* ================================================================
 * Main init entry point
 * ================================================================ */

void __vms_runtime_init(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;

    /* Save environment pointer */
    vms_environ = envp;

    /* Parse auxiliary vector */
    parse_auxv(envp);

    /* Set up TLS for __thread variables */
    setup_tls();

    /* Set thread ID for futex usage */
    vms_sys_set_tid_address(NULL);

    /* Initialize standard I/O streams */
    vms_stdio_init();
}
