/*
 * resolve_call.c — proof of OVMX symbol-vector resolution (bead vms-8d5).
 *
 * Exercises the exact resolver IMGACT uses (ovmx_symvec.h): maps a LINK.EXE-
 * produced OVMX shareable image the way SYS$IMGACT maps a PT_LOAD, resolves a
 * universal symbol by VECTOR POSITION (not by name), adds the load bias, and
 * CALLS it. Then checks GSMATCH accept/reject cases.
 *
 * The image under test is built by src/vmslink/test/run_test.sh:
 *   LIBMATH$SHR.EXE, GSMATCH=LEQUAL,1,1000, vector: [0]=myadd [1]=mymul.
 *
 * Exit 0 iff every check passes.
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

typedef int (*binop)(int, int);

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s image\n", argv[0]); return 2; }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 2; }
    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); return 2; }
    size_t fsize = (size_t)st.st_size;

    /* Read the file to parse ELF headers (section table trails the PT_LOAD). */
    uint8_t *file = malloc(fsize);
    if (!file || read(fd, file, fsize) != (ssize_t)fsize) { perror("read"); return 2; }

    Elf64_Ehdr *eh = (Elf64_Ehdr *)file;
    if (eh->e_type != ET_DYN) { fprintf(stderr, "not ET_DYN\n"); return 2; }

    /* Locate the single PT_LOAD and the .vms$sv section. */
    Elf64_Phdr *ph = (Elf64_Phdr *)(file + eh->e_phoff);
    uint64_t load_off = 0, load_sz = 0;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) { load_off = ph[i].p_offset; load_sz = ph[i].p_filesz; }
    if (!load_sz) { fprintf(stderr, "no PT_LOAD\n"); return 2; }

    Elf64_Shdr *sh = (Elf64_Shdr *)(file + eh->e_shoff);
    const char *shstr = (const char *)(file + sh[eh->e_shstrndx].sh_offset);
    uint64_t sv_off = 0;
    for (int i = 0; i < eh->e_shnum; i++)
        if (strcmp(shstr + sh[i].sh_name, OVMX_SV_SECTION) == 0)
            sv_off = sh[i].sh_offset;
    if (!sv_off) { fprintf(stderr, "no .vms$sv\n"); return 2; }

    /* Map the PT_LOAD as SYS$IMGACT would: identity offset==vaddr, R+X. */
    void *base = mmap(NULL, load_sz, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, (off_t)load_off);
    if (base == MAP_FAILED) { perror("mmap"); return 2; }
    uint64_t bias = (uint64_t)base;              /* p_vaddr == 0 in our image */

    /* The .vms$sv header lives inside the PT_LOAD, so it is in the mapping. */
    const struct ovmx_sv_header *svh =
        (const struct ovmx_sv_header *)((uint8_t *)base + sv_off);
    if (svh->magic != OVMX_SV_MAGIC) { fprintf(stderr, "bad magic\n"); return 2; }

    int fails = 0;
    #define CHECK(cond, msg) do { \
        if (cond) { printf("  ok   %s\n", msg); } \
        else { printf("  FAIL %s\n", msg); fails++; } } while (0)

    printf("resolve+call via symbol vector (bias=%p):\n", base);

    /* Resolve slot 0 (myadd) and slot 1 (mymul) by POSITION, GSMATCH LEQUAL 1,1000. */
    uint64_t a0 = ovmx_sv_resolve(svh, 0, bias, 1, 1000);
    uint64_t a1 = ovmx_sv_resolve(svh, 1, bias, 1, 1000);
    CHECK(a0 != 0, "slot 0 resolves");
    CHECK(a1 != 0, "slot 1 resolves");
    if (a0) { int r = ((binop)a0)(2, 3); printf("    myadd(2,3) = %d\n", r); CHECK(r == 5, "myadd(2,3)==5"); }
    if (a1) { int r = ((binop)a1)(4, 5); printf("    mymul(4,5) = %d\n", r); CHECK(r == 20, "mymul(4,5)==20"); }

    /* Out-of-range index -> no binding (VMS bad-symbol-vector-index). */
    CHECK(ovmx_sv_resolve(svh, 99, bias, 1, 1000) == 0, "index 99 rejected (out of range)");

    /* GSMATCH matrix against image GSMATCH=LEQUAL,1,1000. */
    CHECK(ovmx_gsmatch_ok(svh, 1, 1000) == 1, "GSMATCH LEQUAL: req 1,1000 accepted");
    CHECK(ovmx_gsmatch_ok(svh, 1,  999) == 1, "GSMATCH LEQUAL: req 1,999 accepted (image newer)");
    CHECK(ovmx_gsmatch_ok(svh, 1, 1001) == 0, "GSMATCH LEQUAL: req 1,1001 rejected (image older)");
    CHECK(ovmx_gsmatch_ok(svh, 2, 1000) == 0, "GSMATCH: major mismatch rejected");
    CHECK(ovmx_sv_resolve(svh, 0, bias, 1, 1001) == 0, "resolve fails on GSMATCH reject");

    printf(fails ? "\nRESOLVE/GSMATCH: %d FAILURE(S)\n" : "\nALL RESOLVE/GSMATCH CHECKS PASSED\n", fails);
    munmap(base, load_sz);
    free(file);
    close(fd);
    return fails ? 1 : 0;
}
