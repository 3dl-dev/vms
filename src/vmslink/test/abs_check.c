/*
 * abs_check.c — verify an ABS64 (.rela.data) pointer initializer in an OVMX
 * shareable image resolved to a real in-image address and was recorded in
 * .vms$rel for load-bias at activation. (vms-004)
 *
 *   usage: abs_check IMAGE PTRSYM OFFSET TARGETSYM
 *
 * Resolves the DATA universals PTRSYM and TARGETSYM (by name, via .vms$sv),
 * reads the 8-byte word at (PTRSYM_value + OFFSET) — file offset == image
 * vaddr, single identity-mapped PT_LOAD — and asserts it equals TARGETSYM_value
 * (the pointer was relocated to the target's image-relative address). Also
 * asserts (PTRSYM_value + OFFSET) is present in .vms$rel. Exit 0 on success.
 */
#define _GNU_SOURCE
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "ovmx_image.h"

static uint8_t *g_buf;
static size_t   g_size;

static void die(const char *m) { fprintf(stderr, "abs_check: %s\n", m); exit(2); }

/* Find a named universal's image-relative value in .vms$sv. */
static int univ_value(const char *name, uint64_t *out)
{
    Elf64_Ehdr *eh = (Elf64_Ehdr *)g_buf;
    Elf64_Shdr *sh = (Elf64_Shdr *)(g_buf + eh->e_shoff);
    const char *shstr = (const char *)(g_buf + sh[eh->e_shstrndx].sh_offset);
    Elf64_Shdr *sv = NULL;
    for (int i = 0; i < eh->e_shnum; i++)
        if (strcmp(shstr + sh[i].sh_name, OVMX_SV_SECTION) == 0) sv = &sh[i];
    if (!sv) die("no .vms$sv");
    struct ovmx_sv_header *h = (struct ovmx_sv_header *)(g_buf + sv->sh_offset);
    struct ovmx_sv_entry *e = (struct ovmx_sv_entry *)(g_buf + sv->sh_offset + sizeof *h);
    const char *names = (const char *)(g_buf + sv->sh_offset + h->names_off);
    for (uint32_t i = 0; i < h->count; i++)
        if (strcmp(names + e[i].name_off, name) == 0) { *out = e[i].value; return 1; }
    return 0;
}

/* Is `off` present in the image's .vms$rel offset list? */
static int in_rel(uint64_t off)
{
    Elf64_Ehdr *eh = (Elf64_Ehdr *)g_buf;
    Elf64_Shdr *sh = (Elf64_Shdr *)(g_buf + eh->e_shoff);
    const char *shstr = (const char *)(g_buf + sh[eh->e_shstrndx].sh_offset);
    for (int i = 0; i < eh->e_shnum; i++)
        if (strcmp(shstr + sh[i].sh_name, OVMX_REL_SECTION) == 0) {
            struct ovmx_rel_header *rh =
                (struct ovmx_rel_header *)(g_buf + sh[i].sh_offset);
            uint64_t *o = (uint64_t *)(g_buf + sh[i].sh_offset + sizeof *rh);
            for (uint32_t k = 0; k < rh->count; k++) if (o[k] == off) return 1;
        }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 5) { fprintf(stderr, "usage: %s IMAGE PTRSYM OFFSET TARGETSYM\n", argv[0]); return 2; }
    uint64_t addend = strtoull(argv[3], NULL, 0);

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) die("open");
    struct stat st; fstat(fd, &st);
    g_size = (size_t)st.st_size;
    g_buf = malloc(g_size);
    if (read(fd, g_buf, g_size) != (ssize_t)g_size) die("read");
    close(fd);

    uint64_t pv, tv;
    if (!univ_value(argv[2], &pv)) die("PTRSYM not an exported universal");
    if (!univ_value(argv[4], &tv)) die("TARGETSYM not an exported universal");

    uint64_t slot = pv + addend;
    if (slot + 8 > g_size) die("pointer slot past end of image");
    uint64_t word = *(uint64_t *)(g_buf + slot);   /* offset == vaddr */

    printf("ptr %s+%llu @0x%llx = 0x%llx ; target %s = 0x%llx ; in .vms$rel=%d\n",
           argv[2], (unsigned long long)addend, (unsigned long long)slot,
           (unsigned long long)word, argv[4], (unsigned long long)tv, in_rel(slot));

    if (word != tv) { fprintf(stderr, "FAIL: pointer != target address\n"); return 1; }
    if (tv == 0)    { fprintf(stderr, "FAIL: target resolved to 0\n"); return 1; }
    if (!in_rel(slot)) { fprintf(stderr, "FAIL: slot not recorded in .vms$rel\n"); return 1; }
    return 0;
}
