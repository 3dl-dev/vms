/*
 * ovmx_activate.c — reference SYS$IMGACT activation for the symbol-vector path
 * (bead vms-142). This is the activation logic IMGACT.EXE performs, expressed
 * in hosted C for the e2e proof: it maps a LINK.EXE-produced executable image
 * and its producer shareable images, resolves every .vms$imp import through the
 * producer's symbol vector + GSMATCH (ovmx_symvec.h — byte-identical to what
 * IMGACT uses), patches the consumer's GOT cells, and transfers control.
 *
 *   usage: ovmx_activate CONSUMER.EXE PRODUCER1$SHR.EXE [PRODUCER2$SHR.EXE ...]
 *
 * Runs the consumer in a forked child (it exits via a syscall); the parent
 * returns the child's exit status, so the harness can assert the cross-image
 * call produced the right value. A GSMATCH/index failure exits nonzero with a
 * %IMGACT-F- diagnostic, never running the consumer.
 */
#define _GNU_SOURCE
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "ovmx_image.h"
#include "ovmx_symvec.h"

struct mapped {
    char     soname[256];
    uint8_t *base;          /* mmap base of the PT_LOAD (== vaddr 0)          */
    uint64_t load_sz;
    struct ovmx_sv_header *sv;   /* for producers */
    struct ovmx_imp_header *imp; /* for the consumer */
    uint64_t entry;              /* for the consumer */
};

static void fail(const char *code, const char *msg)
{
    fprintf(stderr, "%%IMGACT-F-%s, %s\n", code, msg);
    _exit(44);
}

/* Read+parse an image, mmap its PT_LOAD, and index .vms$sv / .vms$imp / entry. */
static void map_image(const char *path, struct mapped *m, int writable)
{
    memset(m, 0, sizeof *m);
    const char *b = strrchr(path, '/');
    snprintf(m->soname, sizeof m->soname, "%s", b ? b + 1 : path);

    int fd = open(path, O_RDONLY);
    if (fd < 0) fail("IMGNOTFND", path);
    struct stat st; fstat(fd, &st);
    uint8_t *file = malloc(st.st_size);
    if (read(fd, file, st.st_size) != (ssize_t)st.st_size) fail("READERR", path);

    Elf64_Ehdr *eh = (Elf64_Ehdr *)file;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_type != ET_DYN)
        fail("NOTOVMX", "not an OVMX image (ET_DYN)");
    m->entry = eh->e_entry;

    Elf64_Phdr *ph = (Elf64_Phdr *)(file + eh->e_phoff);
    uint64_t load_off = 0;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) { load_off = ph[i].p_offset; m->load_sz = ph[i].p_filesz; }
    if (!m->load_sz) fail("NOLOAD", path);

    int prot = PROT_READ | PROT_EXEC | (writable ? PROT_WRITE : 0);
    m->base = mmap(NULL, m->load_sz, prot, MAP_PRIVATE, fd, (off_t)load_off);
    if (m->base == MAP_FAILED) fail("MAPFAIL", path);
    close(fd);

    Elf64_Shdr *sh = (Elf64_Shdr *)(file + eh->e_shoff);
    const char *shstr = (const char *)(file + sh[eh->e_shstrndx].sh_offset);
    for (int i = 0; i < eh->e_shnum; i++) {
        const char *nm = shstr + sh[i].sh_name;
        if (strcmp(nm, OVMX_SV_SECTION) == 0)
            m->sv = (struct ovmx_sv_header *)(m->base + sh[i].sh_offset);
        else if (strcmp(nm, OVMX_IMP_SECTION) == 0)
            m->imp = (struct ovmx_imp_header *)(m->base + sh[i].sh_offset);
    }
    free(file);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s CONSUMER PRODUCER...\n", argv[0]); return 2; }

    struct mapped consumer;
    map_image(argv[1], &consumer, 1 /*writable: GOT patched*/);
    if (!consumer.imp) fail("NOIMP", "consumer has no .vms$imp");

    struct mapped prod[64];
    int np = argc - 2;
    for (int i = 0; i < np; i++)
        map_image(argv[2 + i], &prod[i], 0);

    /* Resolve each import via the producer's symbol vector + GSMATCH. */
    struct ovmx_imp_header *ih = consumer.imp;
    struct ovmx_imp_entry *ie =
        (struct ovmx_imp_entry *)((uint8_t *)ih + sizeof *ih);
    const char *inames = (const char *)((uint8_t *)ih + ih->names_off);

    for (uint32_t k = 0; k < ih->count; k++) {
        const char *want = inames + ie[k].producer_off;
        struct mapped *p = NULL;
        for (int j = 0; j < np; j++)
            if (strcmp(prod[j].soname, want) == 0) p = &prod[j];
        if (!p) fail("IMGNOTFND", want);

        uint64_t addr = ovmx_sv_resolve(p->sv, ie[k].sv_index,
                                        (uint64_t)p->base,
                                        ie[k].req_major, ie[k].req_minor);
        if (!addr) {
            fprintf(stderr, "%%IMGACT-F-GSMATCH, %s: symbol-vector index %u "
                    "not bindable (GSMATCH or bad index)\n", want, ie[k].sv_index);
            return 44;
        }
        *(uint64_t *)(consumer.base + ie[k].patch_off) = addr;
        fprintf(stderr, "%%IMGACT-I-BOUND, %s[%u] -> 0x%016llx\n",
                want, ie[k].sv_index, (unsigned long long)addr);
    }

    /* Transfer control in a child (the consumer exits via a syscall). */
    pid_t pid = fork();
    if (pid == 0) {
        void (*entry)(void) = (void (*)(void))(consumer.base + consumer.entry);
        entry();
        _exit(99);   /* consumer should not return */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        fprintf(stderr, "%%IMGACT-I-EXIT, consumer exited %d\n", WEXITSTATUS(status));
        return WEXITSTATUS(status);
    }
    fprintf(stderr, "%%IMGACT-F-ABORT, consumer did not exit normally\n");
    return 44;
}
