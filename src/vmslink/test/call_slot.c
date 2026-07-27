/*
 * call_slot.c — resolve one universal symbol from an OVMX shareable image by
 * vector index and call it as int(int,int). Prints the result and returns it as
 * the process exit code (mod 256). Used by the LINK.EXE harness to prove that a
 * non-leaf producer (with .rodata + local relocations) links and runs.
 *
 *   usage: call_slot IMAGE INDEX A B
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

#include "ovmx_image.h"
#include "ovmx_symvec.h"

int main(int argc, char **argv)
{
    if (argc != 5) { fprintf(stderr, "usage: %s IMAGE INDEX A B\n", argv[0]); return 2; }
    uint32_t index = (uint32_t)strtoul(argv[2], NULL, 0);
    int a = atoi(argv[3]), b = atoi(argv[4]);

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 2; }
    struct stat st; fstat(fd, &st);
    uint8_t *file = malloc(st.st_size);
    if (read(fd, file, st.st_size) != (ssize_t)st.st_size) { perror("read"); return 2; }

    Elf64_Ehdr *eh = (Elf64_Ehdr *)file;
    Elf64_Phdr *ph = (Elf64_Phdr *)(file + eh->e_phoff);
    uint64_t load_off = 0, load_sz = 0;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) { load_off = ph[i].p_offset; load_sz = ph[i].p_filesz; }
    Elf64_Shdr *sh = (Elf64_Shdr *)(file + eh->e_shoff);
    const char *shstr = (const char *)(file + sh[eh->e_shstrndx].sh_offset);
    uint64_t sv_off = 0;
    for (int i = 0; i < eh->e_shnum; i++)
        if (strcmp(shstr + sh[i].sh_name, OVMX_SV_SECTION) == 0) sv_off = sh[i].sh_offset;
    if (!sv_off) { fprintf(stderr, "no .vms$sv\n"); return 2; }

    void *base = mmap(NULL, load_sz, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, (off_t)load_off);
    if (base == MAP_FAILED) { perror("mmap"); return 2; }
    const struct ovmx_sv_header *svh =
        (const struct ovmx_sv_header *)((uint8_t *)base + sv_off);

    uint64_t addr = ovmx_sv_resolve(svh, index, (uint64_t)base,
                                    svh->gsmatch_major, svh->gsmatch_minor);
    if (!addr) { fprintf(stderr, "resolve failed (index/GSMATCH)\n"); return 3; }

    int r = ((int (*)(int, int))addr)(a, b);
    printf("slot[%u](%d,%d) = %d\n", index, a, b, r);
    return r & 0xff;
}
