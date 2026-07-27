/*
 * dump_image.c — OVMXDUMP: inspect an OVMX shareable image's symbol vector.
 *
 * Bead vms-9dd. Reads the `.vms$sv` section (via the ELF section header table)
 * and prints the GSMATCH info + universal-symbol vector. This is the reference
 * reader for the OVMX image format (src/vmslink/include/ovmx_image.h); IMGACT's
 * symbol-vector resolver (bead vms-8d5) parses the same structure.
 *
 * Exit 0 if a well-formed `.vms$sv` is found and printed, nonzero otherwise.
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

static void die(const char *m) { fprintf(stderr, "OVMXDUMP: %s\n", m); exit(2); }

static void *at(uint64_t off, uint64_t sz, const char *w)
{
    if (off + sz > g_size) die(w);
    return g_buf + off;
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s image\n", argv[0]); return 2; }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) die("cannot open image");
    struct stat st;
    if (fstat(fd, &st) < 0) die("fstat");
    g_size = (size_t)st.st_size;
    g_buf = malloc(g_size);
    if (!g_buf || read(fd, g_buf, g_size) != (ssize_t)g_size) die("read");
    close(fd);

    if (g_size < sizeof(Elf64_Ehdr) || memcmp(g_buf, ELFMAG, SELFMAG))
        die("not ELF");
    Elf64_Ehdr *eh = (Elf64_Ehdr *)g_buf;
    if (eh->e_type != ET_DYN) die("not an ET_DYN shareable image");

    Elf64_Shdr *sh = at(eh->e_shoff, (uint64_t)eh->e_shnum * sizeof *sh,
                        "bad shdrs");
    const char *shstr = (const char *)(g_buf + sh[eh->e_shstrndx].sh_offset);

    Elf64_Shdr *svs = NULL;
    for (int i = 0; i < eh->e_shnum; i++)
        if (strcmp(shstr + sh[i].sh_name, OVMX_SV_SECTION) == 0)
            svs = &sh[i];
    if (!svs) die("no " OVMX_SV_SECTION " section (not an OVMX shareable image)");

    struct ovmx_sv_header *h =
        at(svs->sh_offset, sizeof *h, "truncated .vms$sv header");
    if (h->magic != OVMX_SV_MAGIC) die("bad .vms$sv magic");

    const char *gk = h->gsmatch_kind == OVMX_GSMATCH_ALWAYS ? "ALWAYS" :
                     h->gsmatch_kind == OVMX_GSMATCH_EQUAL  ? "EQUAL"  :
                     h->gsmatch_kind == OVMX_GSMATCH_LEQUAL ? "LEQUAL" : "?";
    printf("OVMX shareable image: %s\n", argv[1]);
    printf("  GSMATCH        : %s,%u,%u\n", gk, h->gsmatch_major, h->gsmatch_minor);
    printf("  symbol vector  : %u entr%s\n", h->count, h->count == 1 ? "y" : "ies");

    struct ovmx_sv_entry *e =
        at(svs->sh_offset + sizeof *h, (uint64_t)h->count * sizeof *e,
           "truncated .vms$sv entries");
    const char *names = (const char *)at(svs->sh_offset + h->names_off,
                                         h->names_size, "truncated .vms$sv names");
    for (uint32_t i = 0; i < h->count; i++) {
        const char *kind = e[i].kind == OVMX_SV_PROCEDURE ? "PROCEDURE" :
                           e[i].kind == OVMX_SV_DATA      ? "DATA"      :
                           e[i].kind == OVMX_SV_RETIRED   ? "RETIRED"   : "?";
        printf("    [%3u] %-10s value=0x%016llx  %s\n",
               i, kind, (unsigned long long)e[i].value, names + e[i].name_off);
    }
    free(g_buf);
    return 0;
}
