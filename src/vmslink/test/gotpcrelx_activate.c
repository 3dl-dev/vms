/*
 * gotpcrelx_activate.c — reference SYS$IMGACT activation for an INTRA-image GOT
 * reference (vms-e5d). ovmx_activate.c (vms-142) proves the CROSS-image
 * .vms$imp path; call_slot.c proves a direct (non-GOT) symbol-vector call.
 * Neither exercises the case here: a shareable whose own code reaches a
 * SIBLING-defined function through a GOT-indirect call (R_X86_64_GOTPCREL /
 * GOTPCRELX / REX_GOTPCRELX), which requires the run-time activator to add the
 * image's load bias to every slot .vms$rel lists (the VMS-native analog of an
 * ELF R_*_RELATIVE fixup — see ovmx_image.h) BEFORE the indirect call reads it.
 * A harness that maps the image at vaddr 0 (like a naive readelf-style tool)
 * never exposes a wrong bias; mmap(NULL, ...) picks a real, non-zero, ASLR'd
 * address, so applying .vms$rel here is load-bearing, not decorative.
 *
 *   usage: gotpcrelx_activate IMAGE ENTRY_SYMBOL A B
 *
 * Maps the image PT_LOAD writable+executable (real ASLR base, not 0), adds
 * that base to every .vms$rel-listed slot, resolves ENTRY_SYMBOL via .vms$sv,
 * and calls it as int(int,int) IN THE MAPPED IMAGE — a real load, not a
 * readelf/byte inspection. Exit code is the call's return value.
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

static void die(const char *m) { fprintf(stderr, "gotpcrelx_activate: %s\n", m); exit(2); }

int main(int argc, char **argv)
{
    if (argc != 5) { fprintf(stderr, "usage: %s IMAGE ENTRY_SYMBOL A B\n", argv[0]); return 2; }
    const char *entry_name = argv[2];
    int a = atoi(argv[3]), b = atoi(argv[4]);

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) die("open");
    struct stat st; fstat(fd, &st);
    uint8_t *file = malloc((size_t)st.st_size);
    if (read(fd, file, st.st_size) != (ssize_t)st.st_size) die("read");

    Elf64_Ehdr *eh = (Elf64_Ehdr *)file;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_type != ET_DYN)
        die("not an OVMX shareable (ET_DYN)");

    Elf64_Phdr *ph = (Elf64_Phdr *)(file + eh->e_phoff);
    uint64_t load_off = 0, load_sz = 0;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) { load_off = ph[i].p_offset; load_sz = ph[i].p_filesz; }
    if (!load_sz) die("no PT_LOAD");

    Elf64_Shdr *sh = (Elf64_Shdr *)(file + eh->e_shoff);
    const char *shstr = (const char *)(file + sh[eh->e_shstrndx].sh_offset);
    Elf64_Shdr *svsh = NULL, *relsh = NULL;
    for (int i = 0; i < eh->e_shnum; i++) {
        const char *nm = shstr + sh[i].sh_name;
        if (strcmp(nm, OVMX_SV_SECTION) == 0)  svsh  = &sh[i];
        if (strcmp(nm, OVMX_REL_SECTION) == 0) relsh = &sh[i];
    }
    if (!svsh) die("no .vms$sv");

    /* Real load: PROT_WRITE too (the .vms$rel fixup writes into the GOT cells;
     * a real ELF loader would put .got in a writable PT_LOAD segment -- this
     * single-PT_LOAD harness mirrors run_test_x86_64.sh's ACTIVATE by mapping
     * the whole segment RWX for simplicity, same as ovmx_activate.c does for
     * its writable consumer). mmap(NULL, ...) is a REAL, non-zero, kernel-
     * chosen base -- not vaddr 0. */
    uint8_t *base = mmap(NULL, load_sz, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE, fd, (off_t)load_off);
    if (base == MAP_FAILED) die("mmap");
    close(fd);

    /* Apply .vms$rel: add the real load bias to every listed image-relative
     * slot. This IS the load-time step abs_check.c's own comments say a byte-
     * level readelf check cannot substitute for -- it is exercised here, not
     * skipped. A shareable with a GOT (this one) always carries .vms$rel; an
     * image with none doesn't need this step (e.g. no .rel data at all). */
    if (relsh) {
        struct ovmx_rel_header *rh = (struct ovmx_rel_header *)(base + relsh->sh_offset);
        if (rh->magic != OVMX_REL_MAGIC) die(".vms$rel bad magic");
        uint64_t *off = (uint64_t *)((uint8_t *)rh + sizeof *rh);
        for (uint32_t i = 0; i < rh->count; i++) {
            uint64_t *slot = (uint64_t *)(base + off[i]);
            *slot += (uint64_t)base;   /* image-relative -> real run-time address */
        }
    }

    /* Resolve ENTRY_SYMBOL by name via .vms$sv (index-based binding is the
     * cross-image contract; a same-process direct call can look up by name --
     * this harness only needs to find where to jump in, not bind an import). */
    struct ovmx_sv_header *svh = (struct ovmx_sv_header *)(base + svsh->sh_offset);
    if (svh->magic != OVMX_SV_MAGIC) die(".vms$sv bad magic");
    struct ovmx_sv_entry *sve = (struct ovmx_sv_entry *)((uint8_t *)svh + sizeof *svh);
    const char *names = (const char *)svh + svh->names_off;
    uint64_t entry_val = 0; int found = 0;
    for (uint32_t i = 0; i < svh->count; i++)
        if (strcmp(names + sve[i].name_off, entry_name) == 0) { entry_val = sve[i].value; found = 1; break; }
    if (!found) die("entry symbol not in .vms$sv");

    int (*fn)(int, int) = (int (*)(int, int))(base + entry_val);
    int r = fn(a, b);
    printf("%s(%d,%d) = %d\n", entry_name, a, b, r);
    return r & 0xff;
}
