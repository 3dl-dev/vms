/*
 * link.c — LINK.EXE, the OVMX/VMS linker (MVP, bead vms-9dd, pillar vms-ade).
 *
 * Operator ruling "no unix/linux, all VMS": LINK.EXE replaces `ld` for OVMX
 * shareable images. It reads gcc's ELF relocatable objects and emits an OVMX
 * shareable image — an ELF ET_DYN that carries a `.vms$sv` SYMBOL VECTOR of
 * universal symbols (see src/vmslink/include/ovmx_image.h and
 * docs/design-link-native-toolchain.md). IMGACT (SYS$IMGACT) later binds
 * consumers to these universal symbols by vector POSITION, not by ELF hash.
 *
 * Producer side. LINK.EXE merges N relocatable ELF objects (and/or whole `ar`
 * archives — parsed in-process, no `ld -r`) into a mappable ET_DYN image: it
 * lays out text/rodata/data/bss by ELF flags, applies PC-relative relocations
 * (local + cross-object), synthesizes a GOT for GOT-indirect references and a
 * TLSDESC table for thread-local access, resolves .rela.data ABS64 pointer
 * initializers, records every image-relative slot in `.vms$rel` for load-bias,
 * and emits `.vms$sv` (declared universals) + GSMATCH. Input arrays and the
 * per-object section tables are dynamically sized, so the whole musl libc.a
 * (1345 members / 3600+ sections) ingests without a static cap. With
 * --allow-undefined, references not yet defined (compiler runtime / crt) are
 * recorded as deferred imports for the C RTL to satisfy at activation (vms-61f).
 *
 * This program is built by the normal (bootstrap) toolchain to make VMS images.
 * With -DOVMX_RMS_IO (bead vms-b5a) it is ALSO compiled freestanding-musl and
 * self-linked into an OVMX image (mk_link.sh): its input-object read and its
 * output-image write are then routed through OVMX's RMS system services
 * (ovmx_link_rms_io.c) instead of raw open()/read()/write(), so LINK.EXE runs
 * AS a VMS-native image under IMGACT — self-host S3.1. The four #ifdef
 * OVMX_RMS_IO seams below (slurp/output-write/file_is_archive) are the only
 * behavioral difference; without the define this stays the plain host tool.
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
#include "ovmx_symvec.h"
#include "evax_read.h"      /* Alpha/VMS (EVAX) object front end (bead vms-cbe) */

#ifdef OVMX_RMS_IO
#include "ovmx_link_rms_io.h"   /* vms-b5a: RMS-backed object read + image write */
#endif

/* ABS64 pointer-initializer relocation (S+A written as a 64-bit word — used in
 * .rela.data for pointer tables like stdio FILE structs). Guarded. (vms-004) */
#ifndef R_AARCH64_ABS64
#define R_AARCH64_ABS64           257
#endif

/* 32-bit PC-relative data reference (S+A-P written as a 32-bit word). Emitted
 * for relative/switch tables and some -fPIC data-relative constants. Guarded.
 * (vms-ba1) */
#ifndef R_AARCH64_PREL32
#define R_AARCH64_PREL32          261
#endif

/* Additional PC-relative .text relocation types musl-scale code emits. Guarded:
 * older <elf.h> may lack them. (vms-004) */
#ifndef R_AARCH64_LD_PREL_LO19
#define R_AARCH64_LD_PREL_LO19    273
#endif
#ifndef R_AARCH64_ADR_PREL_LO21
#define R_AARCH64_ADR_PREL_LO21   274
#endif
#ifndef R_AARCH64_TSTBR14
#define R_AARCH64_TSTBR14         279
#endif
#ifndef R_AARCH64_CONDBR19
#define R_AARCH64_CONDBR19        280
#endif

/* GOT-relative relocation types (aarch64). Guarded: older <elf.h> may lack them. */
#ifndef R_AARCH64_ADR_GOT_PAGE
#define R_AARCH64_ADR_GOT_PAGE    311
#endif
#ifndef R_AARCH64_LD64_GOT_LO12_NC
#define R_AARCH64_LD64_GOT_LO12_NC 312
#endif

/* TLSDESC (general-dynamic TLS) relocation types (aarch64). */
#ifndef R_AARCH64_TLSDESC_ADR_PAGE21
#define R_AARCH64_TLSDESC_ADR_PAGE21 562
#endif
#ifndef R_AARCH64_TLSDESC_LD64_LO12
#define R_AARCH64_TLSDESC_LD64_LO12  563
#endif
#ifndef R_AARCH64_TLSDESC_ADD_LO12
#define R_AARCH64_TLSDESC_ADD_LO12   564
#endif
#ifndef R_AARCH64_TLSDESC_CALL
#define R_AARCH64_TLSDESC_CALL       569
#endif

/* x86_64 "simple" static relocations (vms-8f5, grounded by
 * docs/design-link-x86_64-relocs.md): straight little-endian word/dword
 * writes at the relocation offset — NOT bitfield-packed into an instruction
 * encoding like the AARCH64 cases above. GOT/TLSDESC-class x86_64 types
 * (GOTPCREL, REX_GOTPCRELX, GOTPC32_TLSDESC, TLSDESC_CALL, DTPOFF32) are a
 * later bead (vms-cd1/vms-2e4) — out of scope here. Guarded: older <elf.h>
 * may lack them. */
#ifndef R_X86_64_64
#define R_X86_64_64    1   /* direct 64-bit: S+A, absolute pointer initializer */
#endif
#ifndef R_X86_64_PC32
#define R_X86_64_PC32  2   /* 32-bit PC-relative: S+A-P */
#endif
#ifndef R_X86_64_PLT32
#define R_X86_64_PLT32 4   /* 32-bit PC-relative via PLT: S+A-P (same value when
                             * the callee is defined in this link — no real PLT
                             * stub needed for an intra-link reference) */
#endif

/* x86_64 GOT-relative relocations (vms-cd1, grounded by
 * docs/design-link-x86_64-relocs.md): `mov sym@GOTPCREL(%rip), reg` computes
 * the GOT cell's address in ONE relocation — GOT_entry_addr+A-P written as a
 * flat 32-bit disp32, unlike aarch64's ADR_GOT_PAGE/LD64_GOT_LO12_NC PAIR
 * (page-hi21 + lo12 split across two instructions). REX_GOTPCRELX is GNU as's
 * "relaxable" GOTPCREL variant (mov->lea when the linker can prove the GOT
 * slot is unneeded) — LINK.EXE does not perform that relaxation (out of
 * scope: correctness over the optimization), so it is handled identically to
 * plain GOTPCREL: always synthesize the GOT slot and patch the load. Guarded:
 * older <elf.h> may lack them. */
#ifndef R_X86_64_GOTPCREL
#define R_X86_64_GOTPCREL      9   /* mov sym@GOTPCREL(%rip), reg : S(got)+A-P */
#endif
#ifndef R_X86_64_REX_GOTPCRELX
#define R_X86_64_REX_GOTPCRELX 42  /* REX-prefixed relaxable GOTPCREL variant */
#endif
#ifndef R_X86_64_GOTPCRELX
#define R_X86_64_GOTPCRELX     41  /* non-REX relaxable GOTPCREL variant (vms-e5d,
                                     * grounded by docs/design-link-x86_64-relocs.md
                                     * and readelf -r on real musl .o files: e.g.
                                     * `cmp sym@GOTPCREL(%rip), reg` / other non-REX
                                     * encodings gas can relax). Same disp32-write
                                     * codegen as GOTPCREL/REX_GOTPCRELX — LINK.EXE
                                     * does not implement the relaxation either
                                     * way, so all three are handled identically. */
#endif

/* x86_64 TLSDESC ("gnu2" TLS dialect) relocations (vms-2e4). The type set is
 * grounded by docs/design-link-x86_64-relocs.md; the CODEGEN below was
 * re-derived from real `gcc -fPIC -O2 -mtls-dialect=gnu2` objects rather than
 * assumed from the aarch64 shape, because the two are NOT the same mechanism:
 *
 *   aarch64 (4 relocs, bit-patched into an ADRP+LDR+ADD+BLR quartet):
 *       TLSDESC_ADR_PAGE21 / TLSDESC_LD64_LO12 / TLSDESC_ADD_LO12 / TLSDESC_CALL
 *
 *   x86_64 (2 relocs, a flat disp32 + a pure marker) — observed byte-exactly:
 *       48 8d 05 <disp32>   lea  sym@TLSDESC(%rip), %rax
 *                              -> R_X86_64_GOTPC32_TLSDESC, addend -4
 *       ff 10               call *(%rax)
 *                              -> R_X86_64_TLSDESC_CALL, addend 0
 *       64 03 38            add  %fs:(%rax), %edi      (TP + returned offset)
 *
 * So GOTPC32_TLSDESC is written EXACTLY like GOTPCREL — descriptor_addr+A-P as
 * a flat little-endian 32-bit word, with the -4 being a FIELD addend (the site
 * is the 4-byte disp32 field, 4 bytes before the instruction end the CPU uses
 * as %rip) and NOT a symbol offset. TLSDESC_CALL is a marker on the 2-byte
 * `call *(%rax)`: it must be left ALONE (its site is only two bytes wide — a
 * 32-bit write there would corrupt the following instruction).
 *
 * R_X86_64_DTPOFF32 is the local-dynamic half. For `static _Thread_local`
 * variables (exactly what src/libvms/rtl/lib_signal.c has, and why the survey
 * tallies 8 of them) gcc emits ONE TLSDESC pair against the synthetic UND
 * symbol `_TLS_MODULE_BASE_` — "give me this module's TLS block base" — and
 * then addresses each variable as %fs:DTPOFF32(%rax), where the DTPOFF32 field
 * holds the variable's MODULE-RELATIVE offset as a plain absolute 32-bit
 * constant. That value is fixed at link time and is NOT load-biased, so it is
 * deliberately not recorded in .vms$rel. */
#ifndef R_X86_64_DTPOFF32
#define R_X86_64_DTPOFF32        21
#endif
/* Classic (non-TLSDESC "gnu"/"gnu2"-less) general-/local-dynamic TLS relocs the
 * x86_64 psABI defines for the __tls_get_addr access model. STOCK upstream
 * archives (Alpine libstdc++/libsupc++/libgcc) are compiled with the classic
 * dialect and emit these; the OVMX producer graph uses -mtls-dialect=gnu2
 * (TLSDESC, handled above). For a SINGLE static image (no dlopen) both relax to
 * Local-Exec — read TP from %fs:0 and add a link-time-final TP-relative offset
 * (vms-76a). TLSGD names the accessed TLS variable; TLSLD names an arbitrary
 * placeholder and its per-variable offsets ride paired R_X86_64_DTPOFF32. */
#ifndef R_X86_64_TLSGD
#define R_X86_64_TLSGD           19
#endif
#ifndef R_X86_64_TLSLD
#define R_X86_64_TLSLD           20
#endif
#ifndef R_X86_64_GOTPC32_TLSDESC
#define R_X86_64_GOTPC32_TLSDESC 34
#endif
#ifndef R_X86_64_TLSDESC_CALL
#define R_X86_64_TLSDESC_CALL    35
#endif

/* The synthetic UND TLS symbol gcc names in an x86_64 local-dynamic TLSDESC
 * pair. It is not a variable: its "module offset" is 0 by definition. */
#define TLS_MODULE_BASE_SYM "_TLS_MODULE_BASE_"

/*
 * PT_INTERP baked into every LINK.EXE executable: the OVMX image activator.
 * This is the POSIX path the Linux kernel opens as the interpreter when it
 * execve()s a native image, BEFORE any OVMX code runs.
 *
 * ATOMIC FLIP (vms-5f0), spot #3. OVERRIDABLE so the boot flip and the native
 * test suite can disagree on where IMGACT.EXE lives:
 *
 *   - DEFAULT "/vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE": the ~30 native
 *     activation tests (src/imgact/test/*.sh via lib_build_graph.sh) build
 *     their OWN LINK.EXE from this source and stage IMGACT.EXE at exactly this
 *     path, so the default keeps them green untouched.
 *
 *   - BOOTABLE BUILD "/run/ovmx-boot/IMGACT.EXE": the /vms POSIX passthrough is
 *     retired, so this path no longer resolves at boot and the kernel cannot
 *     exec IMGACT.EXE for a native image (DCL.EXE/LOGINOUT.EXE). PID 1 stages
 *     IMGACT.EXE off the genuine ODS-2 volume THROUGH the ACP into
 *     OVMX_BOOT_STAGE_DIR ("/run/ovmx-boot", src/ovmx_init/ovmx_boot_acp_read.c
 *     + src/libvms/include/ovmx_layout.h), so the CMake `vmslink` target that
 *     LINK.EXE-builds the bootable DCL.EXE/LOGINOUT.EXE defines IMGACT_INTERP
 *     to that staged path (src/vmslink/CMakeLists.txt). Keep the two in sync.
 *
 * The basename stays IMGACT.EXE either way, so sys_imgact.c's in-process
 * external-image activation (which matches on the basename, vms-db2) is
 * unaffected by which absolute path is baked.
 */
/*
 * The CMake `vmslink` target overrides the interp via -DIMGACT_INTERP_PATH=
 * <unquoted path> (NOT a -DIMGACT_INTERP="..." string): a quoted string macro
 * lands in compile_commands.json as \"...\" backslash escapes, which the
 * kif_caller_census authenticity gate's line reader refuses (vms-5f0). Passing
 * the path as a bare token and stringifying it here keeps the compile database
 * escape-free. The standalone native-activation tests build link.c with neither
 * macro and keep the /vms default, exactly as before.
 */
#ifndef IMGACT_INTERP
# ifdef IMGACT_INTERP_PATH
#  define IMGACT_INTERP_STR_(s) #s
#  define IMGACT_INTERP_STR(s)  IMGACT_INTERP_STR_(s)
#  define IMGACT_INTERP IMGACT_INTERP_STR(IMGACT_INTERP_PATH)
# else
#  define IMGACT_INTERP "/vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE"
# endif
#endif

/* --------------------------------------------------------------------------
 * Declared universal symbols (from SYMBOL_VECTOR=).
 * -------------------------------------------------------------------------- */
struct univ {
    char     name[256];     /* the EXPORTED universal name (what consumers import) */
    char     internal[256]; /* the INTERNAL symbol that defines it (what we resolve
                             * against input objects). Equals `name` for a plain
                             * `name=KIND` entry; differs for the VSI Linker
                             * `SYMBOL_VECTOR=(universal/internal=KIND)` alias form —
                             * exactly how real DECC$SHR exports `decc$<name>` bound
                             * to the C-RTL implementation symbol. (vms-c07 R1) */
    uint32_t kind;          /* enum ovmx_sv_kind */
    uint64_t value;         /* image-relative address, filled during layout */
    int      resolved;
};

#define MAX_UNIV 2048   /* raised from 512 for the decc$ CRTL alias vector (vms-3e4
                         * R1b): DECC$SHR's musl universals + the decc$<name>
                         * aliases the alpha-dec-vms port imports. uv[] is static
                         * (BSS), so the larger struct univ (name+internal) costs
                         * no stack. */

static void die(const char *msg)
{
    fprintf(stderr, "%%LINK-F-ERROR, %s\n", msg);
    exit(1);
}

/* Section bucket: input sections are classified + merged by ELF flags, not by
 * exact name, so gcc's split sections (.text.unlikely, .rodata.str1.8,
 * .rodata.cst8, .data.rel.ro, ...) all land in the right output region. (vms-fa1) */
enum { B_NONE = 0, B_TEXT, B_RODATA, B_DATA, B_INIT_ARRAY, B_BSS, B_TDATA, B_TBSS,
       B_EH_FRAME };

/* The buckets LINK.EXE places FLAT (a real image vaddr per input section) and
 * can therefore apply relocations into. B_BSS/B_TBSS are NOBITS (no bytes to
 * patch); B_TDATA is reached through TLSDESC, not a flat address; B_NONE is an
 * allocatable section type this linker does not place at all. Anything outside
 * this set that still carries relocations is REPORTED, never dropped in
 * silence. (vms-a66)
 *
 * B_INIT_ARRAY (vms-ee2) is SHT_INIT_ARRAY: the ELF ctor-pointer table gcc
 * emits for a real GNU-C static constructor (__attribute__((constructor)),
 * a C++ static object, ...). It is placed in its OWN writable, dedicated
 * output region (never merged into plain .data) precisely so its start/end
 * can be reported as a clean range -- see the .init_array output-section
 * block in emit_shareable() and its use in imgact.c's symbol-vector ctor
 * runner. Each entry is a plain ABS64 pointer initializer, patched by the
 * SAME reloc-apply loop as B_DATA (no special-casing needed there). This is
 * OVMX's ELF-native carrier for "run these before the image starts" -- the
 * functional equivalent of VMS's LIB$INITIALIZE constructor pass, NOT a
 * reproduction of VMS's PSECT-collection layout (see docs/design-link-native-
 * toolchain.md). */
static int bucket_is_patchable(int b)
{
    return b == B_TEXT || b == B_RODATA || b == B_DATA || b == B_INIT_ARRAY ||
           b == B_EH_FRAME;
}

/* One relocation, tagged with the section it patches (site = sec_va + off). */
struct reloc { uint64_t off; uint64_t info; int64_t add; int sec; };

/* --------------------------------------------------------------------------
 * Input object: slurp the file and index the ELF structures we need.
 * -------------------------------------------------------------------------- */
struct obj {
    uint8_t      *buf;
    size_t        size;
    Elf64_Ehdr   *eh;
    Elf64_Shdr   *sh;       /* section headers */
    int           nsh;
    const char   *shstr;    /* section header string table */
    Elf64_Sym    *sym;      /* .symtab */
    int           nsym;
    const char   *str;      /* .strtab */
    /* Legacy single-section handles (the *first* of each), used by the simple
     * legacy single-.text consumer path (pre-vms-ba1). */
    int           text_ndx; /* .text section index */
    Elf64_Shdr   *text;     /* .text section header */
    int           rodata_ndx; /* .rodata section index (0 if none) */
    Elf64_Shdr   *rodata;   /* .rodata section header (0 if none) */
    int           data_ndx; /* .data section index (0 if none) */
    Elf64_Shdr   *data;     /* .data section header (0 if none) */
    int           bss_ndx;  /* .bss section index (0 if none) */
    Elf64_Shdr   *bss;      /* .bss section header (0 if none) */
    int           tdata_ndx; /* .tdata (TLS init data) index (0 if none) */
    Elf64_Shdr   *tdata;    /* .tdata section header (0 if none) */
    int           tbss_ndx; /* .tbss (TLS zero data) index (0 if none) */
    Elf64_Shdr   *tbss;     /* .tbss section header (0 if none) */
    Elf64_Rela   *rela;     /* relocations against the first .text (SHT_RELA) */
    int           nrela;
    /* Per-section classification + placement (the shareable path, vms-fa1).
     * Dynamically sized to nsh — no fixed cap, so whole-archive combines with
     * thousands of sections ingest without a %LINK-F "too many sections". (vms-004) */
    uint8_t      *sec_bucket; /* [nsh] B_* per section index (0 = unplaced) */
    uint64_t     *sec_va;     /* [nsh] assigned image vaddr, filled at layout */
    struct reloc *relocs;   /* relocs against every code AND data section */
    int           nreloc;
};

static void *xat(struct obj *o, uint64_t off, uint64_t sz, const char *what)
{
    if (off + sz > o->size)
        die(what);
    return o->buf + off;
}

/* Parse an ELF relocatable object already resident in memory. `buf` must remain
 * live for the whole run (o->buf points into it) — the caller owns/keeps it.
 * `name` is a diagnostic label (file path or "archive.a(member.o)"). (vms-004) */
/* Target machine for the emitted image, derived from the input object set (not
 * hardcoded) — set by the first object parsed, checked against every object
 * after it. 0 == not yet seen. (vms-8f5) */
static uint16_t g_out_machine;

static void parse_obj(struct obj *o, uint8_t *buf, size_t size, const char *name)
{
    memset(o, 0, sizeof *o);
    o->buf = buf;
    o->size = size;

    if (o->size < sizeof(Elf64_Ehdr) || memcmp(o->buf, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "%%LINK-F-ERROR, %s: input is not ELF\n", name);
        exit(1);
    }
    o->eh = (Elf64_Ehdr *)o->buf;
    if (o->eh->e_ident[EI_CLASS] != ELFCLASS64)
        die("input is not ELF64");
    if (o->eh->e_type != ET_REL)
        die("input is not a relocatable object (.o)");
    if (o->eh->e_machine != EM_AARCH64 && o->eh->e_machine != EM_X86_64)
        die("MVP supports aarch64 and x86_64 objects only");
    if (g_out_machine == 0)
        g_out_machine = o->eh->e_machine;
    else if (g_out_machine != o->eh->e_machine)
        die("mixed-architecture link: all input objects must share one e_machine "
            "(aarch64 and x86_64 objects cannot be linked into the same image)");

    o->sh = (Elf64_Shdr *)xat(o, o->eh->e_shoff,
                              (uint64_t)o->eh->e_shnum * sizeof(Elf64_Shdr),
                              "bad section header table");
    o->nsh = o->eh->e_shnum;
    o->shstr = (const char *)(o->buf + o->sh[o->eh->e_shstrndx].sh_offset);

    /* Per-section arrays sized to this object's section count (no fixed cap). */
    o->sec_bucket = calloc((size_t)o->nsh, 1);
    o->sec_va     = calloc((size_t)o->nsh, sizeof *o->sec_va);
    if (!o->sec_bucket || !o->sec_va) die("oom allocating per-section tables");

    /* Find .text, .symtab, .strtab. */
    for (int i = 0; i < o->nsh; i++) {
        const char *nm = o->shstr + o->sh[i].sh_name;
        if (o->sh[i].sh_type == SHT_PROGBITS && strcmp(nm, ".text") == 0) {
            o->text_ndx = i;
            o->text = &o->sh[i];
        } else if (o->sh[i].sh_type == SHT_PROGBITS && strcmp(nm, ".rodata") == 0) {
            o->rodata_ndx = i;
            o->rodata = &o->sh[i];
        } else if (o->sh[i].sh_type == SHT_PROGBITS && strcmp(nm, ".data") == 0) {
            o->data_ndx = i;
            o->data = &o->sh[i];
        } else if (o->sh[i].sh_type == SHT_NOBITS && strcmp(nm, ".bss") == 0) {
            o->bss_ndx = i;
            o->bss = &o->sh[i];
        } else if ((o->sh[i].sh_flags & SHF_TLS) &&
                   o->sh[i].sh_type == SHT_PROGBITS && strcmp(nm, ".tdata") == 0) {
            o->tdata_ndx = i;
            o->tdata = &o->sh[i];
        } else if ((o->sh[i].sh_flags & SHF_TLS) &&
                   o->sh[i].sh_type == SHT_NOBITS && strcmp(nm, ".tbss") == 0) {
            o->tbss_ndx = i;
            o->tbss = &o->sh[i];
        } else if (o->sh[i].sh_type == SHT_SYMTAB) {
            o->sym  = (Elf64_Sym *)(o->buf + o->sh[i].sh_offset);
            o->nsym = o->sh[i].sh_size / sizeof(Elf64_Sym);
            o->str  = (const char *)(o->buf + o->sh[o->sh[i].sh_link].sh_offset);
        }
    }
    /* A data-only archive member (e.g. musl's stdout.lo) legitimately has no
     * .text; only the single-object executable path requires one (checked
     * there). An archive can also carry EMPTY stub members with no symbol table
     * at all — libgcc's config-disabled objects (_trampoline.o, _xf_to_dd.o, ...)
     * are all-zero-size ELF placeholders. Such a member defines and references
     * nothing, so it contributes nothing to the link: treat it as inert (no
     * symbols, no relocations) rather than aborting, so the whole-archive C-RTL
     * build (DECC$SHR, vms-61f.1) can pull every member of libc.a AND libgcc.a
     * unconditionally. (vms-61f.1, relaxing the vms-004 hard check.) */
    if (!o->sym) { o->nsym = 0; o->nreloc = 0; o->relocs = NULL; return; }

    /* Collect relocations against the first .text (SHT_RELA), for the simple
     * consumer path. REL (implicit-addend) is not emitted by aarch64 gcc. */
    for (int i = 0; i < o->nsh; i++) {
        if (o->sh[i].sh_info != (Elf64_Word)o->text_ndx || o->sh[i].sh_size == 0)
            continue;
        if (o->sh[i].sh_type == SHT_REL)
            die("REL relocations against .text are unsupported (expected RELA)");
        if (o->sh[i].sh_type == SHT_RELA) {
            o->rela  = (Elf64_Rela *)(o->buf + o->sh[i].sh_offset);
            o->nrela = o->sh[i].sh_size / sizeof(Elf64_Rela);
        }
    }

    /* Classify every allocatable section by ELF flags (not by name), so gcc's
     * split sections merge into the right output region. (vms-fa1) */
    for (int i = 0; i < o->nsh; i++) {
        Elf64_Shdr *s = &o->sh[i];
        if (!(s->sh_flags & SHF_ALLOC)) continue;
        if (s->sh_flags & SHF_TLS)
            o->sec_bucket[i] = (s->sh_type == SHT_NOBITS) ? B_TBSS : B_TDATA;
        else if (s->sh_type == SHT_NOBITS)
            o->sec_bucket[i] = B_BSS;
        else if (s->sh_type == SHT_INIT_ARRAY)
            o->sec_bucket[i] = B_INIT_ARRAY;   /* ctor pointer table (vms-ee2) */
        else if (s->sh_type == SHT_PROGBITS &&
                 strcmp(o->shstr + s->sh_name, ".eh_frame") == 0)
            /* DWARF unwinder frame table (vms-70d). Read-only PROGBITS that
             * would otherwise fall into B_RODATA, but it needs its OWN
             * contiguous output region (like .init_array) so the whole block
             * is one [begin .. 0-terminator] range libgcc's __register_frame
             * can register -- see the .eh_frame layout + .vms$ehf below. Its
             * CIE/FDE PC32 relocs are collected/applied exactly as when it was
             * B_RODATA (bucket_is_patchable includes B_EH_FRAME). */
            o->sec_bucket[i] = B_EH_FRAME;
        else if (s->sh_type == SHT_PROGBITS)
            o->sec_bucket[i] = (s->sh_flags & SHF_EXECINSTR) ? B_TEXT
                             : (s->sh_flags & SHF_WRITE)     ? B_DATA
                             :                                 B_RODATA;
        /* Other allocatable types (SHT_NOTE, SHT_FINI_ARRAY, ...) stay B_NONE;
         * a relocation into one dies loudly rather than silently misplacing.
         * SHT_FINI_ARRAY (image-teardown destructors) is out of scope here:
         * IMGACT.EXE symbol-vector images never return to an "unload" path --
         * see docs/design-image-activation.md -- so there is nothing for a
         * fini-array runner to be called from. */
    }

    /* Collect relocations against every FLAT-PLACED allocatable section into one
     * flat list, each tagged with the section it patches.
     *
     *   B_TEXT   — instruction patches (call/jmp, PC-rel, GOT, TLSDESC).
     *   B_DATA   — .rela.data ABS64 pointer initializers (stdio FILE structs,
     *              locale ptables, *_lockptr sets, ...) resolved + biased at
     *              emit time (vms-004, folds in vms-a17).
     *   B_RODATA — READ-ONLY relocated data. This is NOT decoration: gcc emits
     *              every `switch` jump table into a per-function read-only
     *              section (`.rodata.<fn>`) as `.long target - table_base`, and
     *              because the targets live in .text and the table lives in
     *              .rodata the assembler CANNOT fold the difference — it leaves
     *              a real R_X86_64_PC32 per arm (and .eh_frame's CIE/FDE
     *              pointers are the same shape). Dropping these left every such
     *              table ALL ZERO, so the dispatch `jmp *rdx` computed
     *              table_base + 0 and executed the table's own bytes: musl's
     *              printf_core/pop_arg (i.e. any %-conversion) jumped into the
     *              "(null)" string in DECC$SHR's .rodata. Empirically 902 such
     *              relocations in musl's libc.a and 554 in DCL's own objects, so
     *              the crash needs no unusual image size to appear — only a code
     *              path that reaches a jump table. (vms-a66)
     *
     * A RELA section whose target is allocatable but NOT flat-placed cannot be
     * patched (nothing assigns it an address). LINK.EXE reports that on
     * SYS$ERROR rather than dropping it silently — a silent drop is exactly how
     * the .rodata gap survived four proofs. (vms-a66) */
    int cap = 0;
    for (int i = 0; i < o->nsh; i++) {
        if (o->sh[i].sh_type != SHT_RELA) continue;
        int t = (int)o->sh[i].sh_info;
        if (t >= 0 && t < o->nsh && bucket_is_patchable(o->sec_bucket[t]))
            cap += o->sh[i].sh_size / sizeof(Elf64_Rela);
    }
    o->relocs = cap ? malloc((size_t)cap * sizeof(struct reloc)) : NULL;
    if (cap && !o->relocs) die("oom collecting relocations");
    o->nreloc = 0;
    for (int i = 0; i < o->nsh; i++) {
        int t = (int)o->sh[i].sh_info;
        if (t < 0 || t >= o->nsh) continue;
        if (o->sh[i].sh_type == SHT_REL && bucket_is_patchable(o->sec_bucket[t]))
            die("REL relocations are unsupported (expected RELA)");
        if (o->sh[i].sh_type != SHT_RELA || o->sh[i].sh_size == 0) continue;
        if (!bucket_is_patchable(o->sec_bucket[t])) {
            if (o->sh[t].sh_flags & SHF_ALLOC)
                fprintf(stderr, "%%LINK-W-RELSKIP, %s(%s): %d relocation%s NOT "
                        "applied — target section %s is allocatable but LINK.EXE "
                        "does not place it flat\n",
                        name,
                        o->shstr + o->sh[i].sh_name,
                        (int)(o->sh[i].sh_size / sizeof(Elf64_Rela)),
                        o->sh[i].sh_size == sizeof(Elf64_Rela) ? "" : "s",
                        o->shstr + o->sh[t].sh_name);
            continue;
        }
        Elf64_Rela *ra = (Elf64_Rela *)(o->buf + o->sh[i].sh_offset);
        int n = o->sh[i].sh_size / sizeof(Elf64_Rela);
        for (int j = 0; j < n; j++)
            o->relocs[o->nreloc++] = (struct reloc){
                ra[j].r_offset, ra[j].r_info, ra[j].r_addend, t };
    }
}

/* Read a whole file into a fresh malloc buffer (kept live for the run). */
static uint8_t *slurp(const char *path, size_t *out_size)
{
#ifdef OVMX_RMS_IO
    /* vms-b5a: route the input-object read through RMS (sys$open/$get to EOF).
     * Byte-exact (mrs=1) — see ovmx_link_rms_io.h. */
    uint8_t *rbuf = ovmx_link_rms_slurp(path, out_size);
    if (!rbuf) die("cannot open input file (RMS)");
    return rbuf;
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("cannot open input file");
    struct stat st;
    if (fstat(fd, &st) < 0) die("fstat input");
    size_t sz = (size_t)st.st_size;
    uint8_t *buf = malloc(sz ? sz : 1);
    if (!buf) die("oom reading file");
    size_t got = 0;
    while (got < sz) {
        ssize_t r = read(fd, buf + got, sz - got);
        if (r <= 0) die("short read input");
        got += (size_t)r;
    }
    close(fd);
    *out_size = sz;
    return buf;
#endif
}

/* Load a single relocatable object from a file path. */
static void load_obj(const char *path, struct obj *o)
{
    size_t sz;
    uint8_t *buf = slurp(path, &sz);
    parse_obj(o, buf, sz, path);
}

/* --------------------------------------------------------------------------
 * `ar` archive ingestion (whole-archive, VMS-native, in-process). (vms-004)
 *
 * Operator ruling "no unix/linux, all VMS": LINK.EXE parses the System V/GNU
 * `ar` format itself and pulls EVERY object member — never `ld -r`. Each member
 * is copied to an 8-byte-aligned buffer (aarch64-safe ELF field access) and
 * parsed like a standalone .o. The `/` symbol table and `//` long-name string
 * table members are skipped; long member names (`/<offset>`) are ignored for
 * labeling (resolution is by symbol, not filename).
 * -------------------------------------------------------------------------- */
#define AR_MAGIC "!<arch>\n"
#define AR_MAGIC_LEN 8
#define AR_HDR_SIZE  60

/* Read a right-space-padded decimal field of `len` bytes. */
static uint64_t ar_dec(const char *f, int len)
{
    uint64_t v = 0;
    for (int i = 0; i < len && f[i] >= '0' && f[i] <= '9'; i++)
        v = v * 10 + (uint64_t)(f[i] - '0');
    return v;
}

/* Grow the objs array by one and return the (zeroed) new slot. */
static struct obj *push_obj(struct obj **objs, int *n, int *cap)
{
    if (*n >= *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *objs = realloc(*objs, (size_t)*cap * sizeof(struct obj));
        if (!*objs) die("oom growing object table");
    }
    return &(*objs)[(*n)++];
}

/* Parse every object member of an `ar` archive into the growable objs array. */
static void load_archive(const char *path, struct obj **objs, int *n, int *cap)
{
    size_t asize;
    uint8_t *abuf = slurp(path, &asize);
    size_t pos = AR_MAGIC_LEN;
    int members = 0;
    while (pos + AR_HDR_SIZE <= asize) {
        const char *h = (const char *)(abuf + pos);
        if (h[58] != '`' || h[59] != '\n')
            die("malformed ar member header");
        uint64_t msize = ar_dec(h + 48, 10);
        size_t mdata = pos + AR_HDR_SIZE;
        if (mdata + msize > asize) die("ar member extends past end of archive");

        /* Member name: 16-byte field, trailing spaces (GNU trims a '/'). */
        char nm[17];
        memcpy(nm, h, 16); nm[16] = '\0';
        int e = 16; while (e > 0 && nm[e - 1] == ' ') nm[--e] = '\0';

        /* Skip the symbol table ("/"/"/SYM64/") and long-name table ("//"). */
        int is_special = (strcmp(nm, "/") == 0 || strcmp(nm, "//") == 0 ||
                          strcmp(nm, "/SYM64/") == 0);
        if (!is_special) {
            /* Copy to an 8-aligned buffer for safe ELF field access on arm64. */
            uint8_t *mb = malloc(msize ? msize : 1);
            if (!mb) die("oom copying ar member");
            memcpy(mb, abuf + mdata, msize);
            char label[300];
            snprintf(label, sizeof label, "%s(%s)", path, nm);
            struct obj *o = push_obj(objs, n, cap);
            parse_obj(o, mb, (size_t)msize, label);
            members++;
        }
        pos = mdata + msize;
        if (pos & 1) pos++;   /* members are 2-byte aligned */
    }
    free(abuf);
    fprintf(stderr, "%%LINK-I-ARCHIVE, %s: %d object member%s pulled (whole-archive)\n",
            path, members, members == 1 ? "" : "s");
}

/* --------------------------------------------------------------------------
 * Selective object-library (.OLB) extraction (vms-ca9, self-host spine #3).
 *
 * A `.a` archive is ingested WHOLE (load_archive above) — the OVMX policy for
 * the musl C-RTL and OVMX library shareables, where every member is wanted. An
 * OVMX object library (`.OLB`, produced by LIBRARIAN.EXE / DCL LIBRARY/OBJECT)
 * is instead searched like a real object library: LINK.EXE pulls ONLY the
 * members needed to resolve currently-undefined external references, iterating
 * to a fixpoint (a pulled member may reference symbols another member defines).
 *
 * This is the classic archive-member selection an ELF linker performs, keyed on
 * the member symbol tables directly (no dependence on the ar `/` symbol index —
 * LIBRARIAN does not write one). The .OLB is an OVMX-labeled `ar` container
 * (Rule 8; docs/design-self-host-mmk-spine.md §3, src/vmslink/include/ovmx_olb.h);
 * the byte-level parse below is identical to load_archive's `ar` walk. STRONG
 * (STB_GLOBAL) undefined references force extraction; a WEAK undefined reference
 * does not (matching ld: it resolves to 0 if nothing defines it). A member is
 * pulled if it DEFINES (st_shndx != SHN_UNDEF) a name that is currently an
 * unresolved strong reference.
 * -------------------------------------------------------------------------- */

/* A parsed-but-not-yet-pulled candidate pool for one .OLB. Each member owns its
 * own 8-aligned buffer (like load_archive), so the archive file buffer is freed
 * after parse; a member's resources are transferred to objs[] when pulled. */
struct olb_pool {
    const char *path;
    struct obj *mem;      /* parsed members (own buffers) */
    int         nmem;
    int        *pulled;   /* [nmem] 1 once moved into objs[] */
};

/* Return non-zero if `path` names an OVMX object library by extension (.OLB,
 * case-insensitive). Selection is by extension: a `.a` stays whole-archive. */
static int file_is_olb(const char *path)
{
    size_t l = strlen(path);
    if (l < 4) return 0;
    const char *e = path + l - 4;
    return (e[0] == '.') &&
           (e[1] == 'o' || e[1] == 'O') &&
           (e[2] == 'l' || e[2] == 'L') &&
           (e[3] == 'b' || e[3] == 'B');
}

/* Pre-parse every object member of an .OLB into a candidate pool (NOT into the
 * link's objs[] — the resolver decides which to pull). Same `ar` walk as
 * load_archive. */
static void load_olb_pool(const char *path, struct olb_pool *pool)
{
    size_t asize;
    uint8_t *abuf = slurp(path, &asize);
    if (asize < AR_MAGIC_LEN || memcmp(abuf, AR_MAGIC, AR_MAGIC_LEN) != 0)
        die("input .OLB is not an ar-container object library");
    pool->path = path;
    pool->mem  = NULL;
    pool->nmem = 0;
    int cap = 0;
    size_t pos = AR_MAGIC_LEN;
    while (pos + AR_HDR_SIZE <= asize) {
        const char *h = (const char *)(abuf + pos);
        if (h[58] != '`' || h[59] != '\n')
            die("malformed ar member header in .OLB");
        uint64_t msize = ar_dec(h + 48, 10);
        size_t mdata = pos + AR_HDR_SIZE;
        if (mdata + msize > asize) die(".OLB member extends past end of archive");

        char nm[17];
        memcpy(nm, h, 16); nm[16] = '\0';
        int e = 16; while (e > 0 && nm[e - 1] == ' ') nm[--e] = '\0';
        int is_special = (strcmp(nm, "/") == 0 || strcmp(nm, "//") == 0 ||
                          strcmp(nm, "/SYM64/") == 0);
        if (!is_special) {
            uint8_t *mb = malloc(msize ? msize : 1);
            if (!mb) die("oom copying .OLB member");
            memcpy(mb, abuf + mdata, msize);
            char label[300];
            snprintf(label, sizeof label, "%s(%s)", path, nm);
            struct obj *o = push_obj(&pool->mem, &pool->nmem, &cap);
            parse_obj(o, mb, (size_t)msize, label);
        }
        pos = mdata + msize;
        if (pos & 1) pos++;
    }
    free(abuf);
    pool->pulled = calloc((size_t)(pool->nmem ? pool->nmem : 1), sizeof(int));
    if (!pool->pulled) die("oom tracking .OLB members");
}

/* -------- minimal string set (open addressing, FNV-1a), keyed on names that
 * live in the objects' string tables (kept live for the run). -------------- */
struct symset { const char **slot; uint32_t cap; uint32_t n; };

static uint64_t symset_hash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}
static void symset_init(struct symset *S)
{
    S->cap = 256; S->n = 0;
    S->slot = calloc(S->cap, sizeof(*S->slot));
    if (!S->slot) die("oom building symbol set");
}
static void symset_free(struct symset *S) { free(S->slot); S->slot = NULL; S->cap = S->n = 0; }
static int symset_has(struct symset *S, const char *name)
{
    uint32_t m = S->cap - 1, i = (uint32_t)symset_hash(name) & m;
    while (S->slot[i]) { if (strcmp(S->slot[i], name) == 0) return 1; i = (i + 1) & m; }
    return 0;
}
static void symset_add(struct symset *S, const char *name)
{
    if ((S->n + 1) * 4 >= S->cap * 3) {          /* grow at 75% load */
        uint32_t ncap = S->cap * 2;
        const char **ns = calloc(ncap, sizeof(*ns));
        if (!ns) die("oom growing symbol set");
        for (uint32_t j = 0; j < S->cap; j++) if (S->slot[j]) {
            uint32_t m = ncap - 1, i = (uint32_t)symset_hash(S->slot[j]) & m;
            while (ns[i]) i = (i + 1) & m;
            ns[i] = S->slot[j];
        }
        free(S->slot); S->slot = ns; S->cap = ncap;
    }
    uint32_t m = S->cap - 1, i = (uint32_t)symset_hash(name) & m;
    while (S->slot[i]) { if (strcmp(S->slot[i], name) == 0) return; i = (i + 1) & m; }
    S->slot[i] = name; S->n++;
}

/* Add every global/weak DEFINED symbol of `o` to D. */
static void collect_defined(struct obj *o, struct symset *D)
{
    for (int k = 0; k < o->nsym; k++) {
        Elf64_Sym *s = &o->sym[k];
        unsigned char b = ELF64_ST_BIND(s->st_info);
        if ((b == STB_GLOBAL || b == STB_WEAK) && s->st_shndx != SHN_UNDEF) {
            const char *nm = o->str + s->st_name;
            if (nm[0]) symset_add(D, nm);
        }
    }
}

/* Does `o` define a symbol that is currently an unresolved strong reference? */
static int member_satisfies(struct obj *o, struct symset *U)
{
    for (int k = 0; k < o->nsym; k++) {
        Elf64_Sym *s = &o->sym[k];
        unsigned char b = ELF64_ST_BIND(s->st_info);
        if ((b == STB_GLOBAL || b == STB_WEAK) && s->st_shndx != SHN_UNDEF) {
            const char *nm = o->str + s->st_name;
            if (nm[0] && symset_has(U, nm)) return 1;
        }
    }
    return 0;
}

/* Iterate the .OLB pools, pulling members that resolve currently-undefined
 * strong references, to a fixpoint. Pulled members are moved into objs[].
 *
 * `uv`/`nuniv` are the --symbol-vector universals (may be NULL/0). In real VMS
 * a SYMBOL_VECTOR entry is an unresolved reference that the library search must
 * satisfy: the vector roots the selective pull. So the universal names seed the
 * initial unresolved set U alongside the root objects' own undefined refs. This
 * is what lets a /SHAREABLE link from an .OLB alone (no explicit object TU list)
 * pull exactly the modules that define the universals + their transitive refs
 * (design-vms-native-shareable-build.md Part C, C.4.1). A retired slot
 * (OVMX_SV_RETIRED) names no symbol that still exists, so it never roots a
 * search. Seeding an empty vector (nuniv==0) leaves current behavior unchanged. */
static void resolve_olbs(struct obj **objs, int *nobj, int *cap,
                         struct olb_pool *pools, int npool,
                         const struct univ *uv, int nuniv)
{
    for (;;) {
        struct symset D, U;
        symset_init(&D);
        symset_init(&U);
        for (int i = 0; i < *nobj; i++) collect_defined(&(*objs)[i], &D);
        for (int i = 0; i < *nobj; i++) {
            struct obj *o = &(*objs)[i];
            for (int k = 0; k < o->nsym; k++) {
                Elf64_Sym *s = &o->sym[k];
                if (s->st_shndx != SHN_UNDEF) continue;
                if (ELF64_ST_BIND(s->st_info) != STB_GLOBAL) continue; /* strong only */
                const char *nm = o->str + s->st_name;
                if (nm[0] && !symset_has(&D, nm)) symset_add(&U, nm);
            }
        }
        /* Root the search at the symbol vector: each still-undefined universal
         * is a reference the .OLB must satisfy (VMS §1.2.3.2 default library
         * search rooted at the SYMBOL_VECTOR). Once its defining member is
         * pulled, the name enters D and drops out on the next iteration. */
        for (int i = 0; i < nuniv; i++) {
            if (uv[i].kind == OVMX_SV_RETIRED) continue;
            const char *nm = uv[i].internal;   /* the DEFINING symbol (alias-aware):
                                                * a `decc$fprintf/fprintf` universal
                                                * is satisfied by the member defining
                                                * `fprintf`, not `decc$fprintf`. */
            if (nm[0] && !symset_has(&D, nm)) symset_add(&U, nm);
        }

        int pulled = 0;
        if (U.n) {
            for (int p = 0; p < npool; p++) {
                for (int m = 0; m < pools[p].nmem; m++) {
                    if (pools[p].pulled[m]) continue;
                    if (member_satisfies(&pools[p].mem[m], &U)) {
                        *push_obj(objs, nobj, cap) = pools[p].mem[m];
                        pools[p].pulled[m] = 1;
                        pulled++;
                    }
                }
            }
        }
        symset_free(&D);
        symset_free(&U);
        if (!pulled) break;
    }

    for (int p = 0; p < npool; p++) {
        int got = 0;
        for (int m = 0; m < pools[p].nmem; m++) if (pools[p].pulled[m]) got++;
        fprintf(stderr, "%%LINK-I-LIBRARY, %s: %d of %d member%s pulled (selective)\n",
                pools[p].path, got, pools[p].nmem, pools[p].nmem == 1 ? "" : "s");
    }
}

/* --------------------------------------------------------------------------
 * Option parsing.
 * -------------------------------------------------------------------------- */
/*
 * SYMBOL_VECTOR= keywords.
 *
 * PRIVATE_PROCEDURE / PRIVATE_DATA are the RETIREMENT keywords, and they are
 * the reason OVMX_SV_RETIRED exists. Public VMS upward-compatibility rules
 * (VSI OpenVMS Linker Utility Manual; docs/design-link-native-toolchain.md
 * §5.1/§5.3) are: preserve the order and placement of existing entries, NEVER
 * delete an entry, add only at the end — and when a universal goes away,
 * *retire it in place* with PRIVATE_PROCEDURE/PRIVATE_DATA rather than
 * removing it, so that every later entry keeps its bound index and GSMATCH
 * LEQUAL stays valid. There is no SPARE keyword in the public docs (§5.6).
 *
 * A retired slot is not publicly bound: find_universal() below, IMGACT's
 * sv_find_named() and ovmx_sv_at() all skip OVMX_SV_RETIRED, and its `value`
 * is left 0 because the symbol it named no longer exists in any input object.
 */
static uint32_t parse_kind(const char *k)
{
    if (strcmp(k, "PROCEDURE") == 0)
        return OVMX_SV_PROCEDURE;
    if (strcmp(k, "DATA") == 0)
        return OVMX_SV_DATA;
    if (strcmp(k, "PRIVATE_PROCEDURE") == 0 || strcmp(k, "PRIVATE_DATA") == 0)
        return OVMX_SV_RETIRED;
    if (strcmp(k, "GLOBALVALUE") == 0)
        die("GLOBALVALUE requires a value: name=GLOBALVALUE:0x<hex>");
    die("unknown SYMBOL_VECTOR keyword "
        "(want PROCEDURE|DATA|PRIVATE_PROCEDURE|PRIVATE_DATA|GLOBALVALUE:<val>)");
    return 0;
}

/* Parse "name=PROCEDURE,name2=DATA,..." into the univ[] table. */
static int parse_symbol_vector(char *spec, struct univ *uv)
{
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(spec, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) die("SYMBOL_VECTOR entry needs name=KEYWORD");
        *eq = '\0';
        if (n >= MAX_UNIV) die("too many universal symbols");
        /* "universal" or "universal/internal" (VSI OpenVMS Linker SYMBOL_VECTOR
         * alias form, Utility Manual §5.6): the exported universal name may differ
         * from the internal symbol that defines it — exactly how real DECC$SHR
         * exports `decc$<name>` bound to the C-RTL implementation. Absent a '/',
         * internal == universal (current behavior unchanged). (vms-c07 R1) */
        char *slash = strchr(tok, '/');
        if (slash) *slash = '\0';
        snprintf(uv[n].name, sizeof uv[n].name, "%s", tok);
        snprintf(uv[n].internal, sizeof uv[n].internal, "%s",
                 slash ? slash + 1 : tok);
        /* GLOBALVALUE form: "name=GLOBALVALUE:0x<hex>" (OVMX authoring syntax —
         * the .vms$sv is an OVMX-original section, so this keyword is ours). The
         * value is an ABSOLUTE link-time constant carried in the vector entry,
         * not resolved from any input object; it is preset here and left intact
         * by the layout pass. A globalvalue names no internal symbol. (vms-954) */
        char *kw = eq + 1;
        char *colon = strchr(kw, ':');
        if (colon && (size_t)(colon - kw) == strlen("GLOBALVALUE") &&
            strncmp(kw, "GLOBALVALUE", strlen("GLOBALVALUE")) == 0) {
            uv[n].kind  = OVMX_SV_GLOBALVALUE;
            uv[n].value = strtoull(colon + 1, NULL, 0);   /* 0x.. hex or decimal */
            uv[n].internal[0] = '\0';                     /* no defining symbol  */
        } else {
            uv[n].kind = parse_kind(kw);
        }
        n++;
    }
    if (n == 0) die("SYMBOL_VECTOR= is empty");
    return n;
}

/* Parse GSMATCH "KEYWORD,major,minor". */
static void parse_gsmatch(char *spec, uint32_t *kind, uint32_t *maj, uint32_t *min)
{
    char *save = NULL;
    char *k = strtok_r(spec, ",", &save);
    char *a = strtok_r(NULL, ",", &save);
    char *b = strtok_r(NULL, ",", &save);
    if (!k) die("GSMATCH needs KEYWORD,major,minor");
    if (strcmp(k, "ALWAYS") == 0)      *kind = OVMX_GSMATCH_ALWAYS;
    else if (strcmp(k, "EQUAL") == 0)  *kind = OVMX_GSMATCH_EQUAL;
    else if (strcmp(k, "LEQUAL") == 0) *kind = OVMX_GSMATCH_LEQUAL;
    else die("GSMATCH keyword must be ALWAYS|EQUAL|LEQUAL");
    *maj = a ? (uint32_t)strtoul(a, NULL, 0) : 0;
    *min = b ? (uint32_t)strtoul(b, NULL, 0) : 0;
}

/* --------------------------------------------------------------------------
 * Image emit.
 * -------------------------------------------------------------------------- */
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((uint64_t)(a) - 1))
#define PAGE 0x1000u
#define CRT0_NINSN 7   /* aarch64 instruction count in the synthesized executable
                        * crt0 (vms-ba1); *4 = 28 bytes reserved for the stub,
                        * which the x86_64 crt0 stub (vms-206) also fits in
                        * exactly (its 7 variable-length instructions total 28
                        * bytes too) -- same reservation, both encodings. */

/* --------------------------------------------------------------------------
 * Consumer/executable linking: bind imports to producer symbol vectors.
 * -------------------------------------------------------------------------- */

/* aarch64 instruction encoders for the PLT stub + call patch. */
static uint32_t enc_adrp(int rd, int64_t page_delta)
{
    uint32_t immlo = (uint32_t)(page_delta & 0x3);
    uint32_t immhi = (uint32_t)((page_delta >> 2) & 0x7FFFF);
    return 0x90000000u | (immlo << 29) | (immhi << 5) | (uint32_t)rd;
}
static uint32_t enc_ldr_u64(int rt, int rn, uint32_t off /*8-aligned*/)
{
    return 0xF9400000u | ((off / 8) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static uint32_t enc_br(int rn) { return 0xD61F0000u | ((uint32_t)rn << 5); }

/* A loaded producer shareable image (read to bind universal symbols). */
struct producer {
    char                    name[256];   /* soname used in .vms$imp (basename) */
    uint8_t                *buf;
    struct ovmx_sv_header  *sv;
};

/* A leading VMS logical the caller may name a --use producer by (vms-104). */
static int leading_ci(const char *s, const char *pfx)
{
    for (; *pfx; s++, pfx++) {
        char a = *s, b = *pfx;
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return 0;
    }
    return 1;
}

/*
 * resolve_producer_path (vms-104) - map a --use producer named by VMS logical
 * spec to the POSIX file that carries its bytes. A shareable installed on the
 * ODS-2 system volume is named SYS$SHARE:/SYS$LIBRARY:/SYS$SYSTEM:<NAME.EXE> --
 * NOT a /vms POSIX path (the atomic-flip-retired passthrough). ovmx_init read
 * each installed shareable off the volume THROUGH the Files-11 ACP and staged it
 * to OVMX_BOOT_STAGE_DIR ("/run/ovmx-boot"); LINK.EXE, a native musl tool that
 * opens the producer with POSIX open(), resolves the logical to that staged copy
 * -- so the producer bytes come from the volume over the ACP, never /vms
 * (Rule 9 / INV-6). Any other spec (a bare name, an absolute POSIX path from a
 * host bootstrap build) is returned unchanged. `out` is a caller buffer.
 */
static const char *resolve_producer_path(const char *path, char *out, size_t sz)
{
    /* Each installed-image logical -> the SYS$SYSROOT subdirectory it lives in. */
    static const struct { const char *log; const char *sub; } maps[] = {
        { "SYS$SHARE:",   "SYSLIB" },
        { "SYS$LIBRARY:", "SYSLIB" },
        { "SYS$SYSTEM:",  "SYSEXE" },
    };
    for (unsigned i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) {
        if (!leading_ci(path, maps[i].log))
            continue;
        const char *leaf = path + strlen(maps[i].log);
        /* Defend against an embedded directory: a spec is SYS$SHARE:NAME.EXE. */
        const char *slash = strrchr(leaf, '/');
        if (slash) leaf = slash + 1;

        /* (1) RUNTIME: the boot bridge read the installed image off the ODS-2
         * volume THROUGH the ACP and staged it here (the /vms passthrough is
         * retired on the runtime path). Prefer it when present. */
        snprintf(out, sz, "/run/ovmx-boot/%s", leaf);
        if (access(out, R_OK) == 0)
            return out;

        /* (2) HOST CTEST (no /dev/vms, no boot bridge -- e.g. the BUILD.COM S3.2
         * DCL driver): the installed images live at their legacy POSIX
         * SYS$SYSROOT location. This /vms read is the sanctioned legacy path for
         * the no-executive case (the flip only retires /vms when the ACP is
         * live), NEVER reached on the runtime where (1) resolves first. */
        snprintf(out, sz, "/vms/SYS0/SYSCOMMON/%s/%s", maps[i].sub, leaf);
        return out;
    }
    return path;
}

static void load_producer(const char *path_in, struct producer *p)
{
    memset(p, 0, sizeof *p);
    char pbuf[512];
    const char *path = resolve_producer_path(path_in, pbuf, sizeof pbuf);
    const char *base = strrchr(path, '/');
    snprintf(p->name, sizeof p->name, "%s", base ? base + 1 : path);

    int fd = open(path, O_RDONLY);
    if (fd < 0) die("cannot open producer image (--use)");
    struct stat st;
    if (fstat(fd, &st) < 0) die("fstat producer");
    p->buf = malloc(st.st_size);
    if (!p->buf || read(fd, p->buf, st.st_size) != (ssize_t)st.st_size)
        die("read producer");
    close(fd);

    Elf64_Ehdr *eh = (Elf64_Ehdr *)p->buf;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_type != ET_DYN)
        die("producer is not an OVMX shareable image (ET_DYN)");
    Elf64_Shdr *sh = (Elf64_Shdr *)(p->buf + eh->e_shoff);
    const char *shstr = (const char *)(p->buf + sh[eh->e_shstrndx].sh_offset);
    for (int i = 0; i < eh->e_shnum; i++)
        if (strcmp(shstr + sh[i].sh_name, OVMX_SV_SECTION) == 0)
            p->sv = (struct ovmx_sv_header *)(p->buf + sh[i].sh_offset);
    if (!p->sv || p->sv->magic != OVMX_SV_MAGIC)
        die("producer image has no .vms$sv symbol vector");
}

/* Find universal `name` across producers -> (producer index, vector index). */
static int find_universal(struct producer *ps, int np, const char *name,
                          int *pidx, uint32_t *svidx)
{
    for (int p = 0; p < np; p++) {
        const struct ovmx_sv_entry *e = ovmx_sv_entries(ps[p].sv);
        const char *nm = ovmx_sv_names(ps[p].sv);
        for (uint32_t i = 0; i < ps[p].sv->count; i++) {
            if (e[i].kind == OVMX_SV_RETIRED) continue;
            if (strcmp(nm + e[i].name_off, name) == 0) {
                *pidx = p; *svidx = i; return 1;
            }
        }
    }
    return 0;
}

struct import {
    char     name[256];
    int      pidx;         /* producer index (-1 for a weak-by-name import) */
    uint32_t svidx;        /* vector index within that producer */
    uint64_t plt_va;       /* PLT stub address (assigned at layout) */
    uint64_t got_va;       /* GOT cell address (assigned at layout) */
    int      is_data;      /* 1 = DATA import (GOT-read), 0 = call import (PLT) */
    int      is_weak;      /* 1 = resolved by NAME at activation (.vms$wimp): no
                            * --use producer exports it, but a loaded producer
                            * MAY (a lower layer reaching a higher one across a
                            * build cycle). Absent at run time -> cell stays 0.
                            * (vms-5f0) */
};

/* Patch a GOT-relative reference to reach `slot` PC-relatively: the aarch64
 * ADR_GOT_PAGE/LD64_GOT_LO12_NC PAIR, or x86_64's single GOTPCREL/
 * REX_GOTPCRELX flat disp32 (which carries a real addend, usually -4 — see
 * the definition below) (defined below; forward-declared for the executable
 * data-import path). */
static void patch_got(uint32_t type, uint32_t *insn, uint64_t site, uint64_t slot,
                      int64_t add);

/* Find an interned import by name -> index, or -1. */
static int import_find(struct import *imp, int nimp, const char *nm)
{
    for (int i = 0; i < nimp; i++)
        if (strcmp(imp[i].name, nm) == 0) return i;
    return -1;
}

/* Number of STRONG (by producer+index) vs WEAK (by name) imports in imp[]. Both
 * kinds share the PLT/import-GOT layout; they split only at section emission —
 * strong -> .vms$imp, weak -> .vms$wimp. (vms-5f0) */
static int import_count_strong(struct import *imp, int nimp)
{
    int n = 0;
    for (int i = 0; i < nimp; i++) if (!imp[i].is_weak) n++;
    return n;
}
static int import_count_weak(struct import *imp, int nimp)
{
    int n = 0;
    for (int i = 0; i < nimp; i++) if (imp[i].is_weak) n++;
    return n;
}

/* Byte size of a .vms$imp section: header + STRONG entries + deduped soname blob.
 * Shared by the executable and shareable emit paths (a lib
 * shareable's own cross-image imports — vms-e65). */
static uint64_t vms_imp_size(int nimp, struct import *imp, struct producer *ps, int np)
{
    uint32_t names_sz = 0;
    for (int p = 0; p < np; p++) names_sz += (uint32_t)strlen(ps[p].name) + 1;
    return sizeof(struct ovmx_imp_header)
         + (uint64_t)import_count_strong(imp, nimp) * sizeof(struct ovmx_imp_entry)
         + names_sz;
}

/* Byte size of a .vms$wimp section: header + WEAK entries + symbol-name blob. */
static uint64_t vms_wimp_size(int nimp, struct import *imp)
{
    uint32_t names_sz = 0;
    for (int i = 0; i < nimp; i++)
        if (imp[i].is_weak) names_sz += (uint32_t)strlen(imp[i].name) + 1;
    return sizeof(struct ovmx_wimp_header)
         + (uint64_t)import_count_weak(imp, nimp) * sizeof(struct ovmx_wimp_entry)
         + names_sz;
}

/* Write the .vms$imp section at img+off_imp. Each import's patch_off is its GOT
 * cell (imp[i].got_va); IMGACT resolves (producer soname, sv_index) -> run-time
 * address and stores it there at activation. Producer sonames are deduped into a
 * trailing name blob. Shared by the executable and shareable emit paths (vms-e65). */
static void vms_imp_write(uint8_t *img, uint64_t off_imp, struct import *imp,
                          int nimp, struct producer *ps, int np)
{
    int nstrong = import_count_strong(imp, nimp);
    uint64_t imp_hdr = sizeof(struct ovmx_imp_header);
    uint64_t imp_ents = (uint64_t)nstrong * sizeof(struct ovmx_imp_entry);
    uint64_t imp_names_o = imp_hdr + imp_ents;

    uint32_t *prod_off = calloc((size_t)(np > 0 ? np : 1), sizeof *prod_off);
    if (!prod_off) die("oom .vms$imp producer offsets");
    char *nb = (char *)(img + off_imp + imp_names_o);
    uint32_t names_sz = 0;
    for (int p = 0; p < np; p++) {
        prod_off[p] = names_sz;
        size_t l = strlen(ps[p].name) + 1;
        memcpy(nb + names_sz, ps[p].name, l);
        names_sz += (uint32_t)l;
    }
    struct ovmx_imp_header *ih = (struct ovmx_imp_header *)(img + off_imp);
    ih->magic = OVMX_IMP_MAGIC; ih->count = (uint32_t)nstrong;
    ih->names_off = (uint32_t)imp_names_o; ih->names_size = names_sz;
    struct ovmx_imp_entry *ie =
        (struct ovmx_imp_entry *)(img + off_imp + imp_hdr);
    int o = 0;
    for (int i = 0; i < nimp; i++) {
        if (imp[i].is_weak) continue;   /* -> .vms$wimp, not here */
        ie[o].producer_off = prod_off[imp[i].pidx];
        ie[o].sv_index = imp[i].svidx;
        ie[o].patch_off = imp[i].got_va;
        ie[o].req_major = ps[imp[i].pidx].sv->gsmatch_major;
        ie[o].req_minor = ps[imp[i].pidx].sv->gsmatch_minor;
        o++;
    }
    free(prod_off);
}

/* Write the .vms$wimp section at img+off_wimp: header, WEAK import entries
 * (name_off, patch_off = import-GOT cell), then a symbol-name blob. IMGACT
 * resolves each name against the loaded producer set at activation. (vms-5f0) */
static void vms_wimp_write(uint8_t *img, uint64_t off_wimp, struct import *imp,
                           int nimp)
{
    int nweak = import_count_weak(imp, nimp);
    uint64_t hdr = sizeof(struct ovmx_wimp_header);
    uint64_t ents = (uint64_t)nweak * sizeof(struct ovmx_wimp_entry);
    uint64_t names_o = hdr + ents;

    struct ovmx_wimp_header *wh = (struct ovmx_wimp_header *)(img + off_wimp);
    wh->magic = OVMX_WIMP_MAGIC; wh->count = (uint32_t)nweak;
    wh->names_off = (uint32_t)names_o;
    struct ovmx_wimp_entry *we =
        (struct ovmx_wimp_entry *)(img + off_wimp + hdr);
    char *nb = (char *)(img + off_wimp + names_o);
    uint32_t names_sz = 0;
    int o = 0;
    for (int i = 0; i < nimp; i++) {
        if (!imp[i].is_weak) continue;
        we[o].name_off = names_sz;
        we[o].reserved = 0;
        we[o].patch_off = imp[i].got_va;
        size_t l = strlen(imp[i].name) + 1;
        memcpy(nb + names_sz, imp[i].name, l);
        names_sz += (uint32_t)l;
        o++;
    }
    wh->names_size = names_sz;
}


/* Patch one PC-relative relocation instruction to reach `target`. */
static void patch_pcrel(uint32_t type, uint32_t *insn, uint64_t site, uint64_t target)
{
    switch (type) {
    case R_AARCH64_CALL26:
    case R_AARCH64_JUMP26: {
        int64_t d = (int64_t)target - (int64_t)site;
        *insn = (*insn & ~0x03FFFFFFu) | (uint32_t)((d >> 2) & 0x03FFFFFF);
        break;
    }
    case R_AARCH64_ADR_PREL_PG_HI21: {
        int64_t d = (int64_t)(target >> 12) - (int64_t)(site >> 12);
        uint32_t immlo = (uint32_t)(d & 3), immhi = (uint32_t)((d >> 2) & 0x7FFFF);
        *insn = (*insn & ~((3u << 29) | (0x7FFFFu << 5))) | (immlo << 29) | (immhi << 5);
        break;
    }
    case R_AARCH64_ADR_PREL_LO21: {   /* adr Xd, sym : 21-bit byte displacement */
        int64_t d = (int64_t)target - (int64_t)site;
        uint32_t immlo = (uint32_t)(d & 3), immhi = (uint32_t)((d >> 2) & 0x7FFFF);
        *insn = (*insn & ~((3u << 29) | (0x7FFFFu << 5))) | (immlo << 29) | (immhi << 5);
        break;
    }
    case R_AARCH64_CONDBR19:          /* b.cond / cbz / cbnz : imm19 << 5, *4 */
    case R_AARCH64_LD_PREL_LO19: {    /* ldr (literal)      : imm19 << 5, *4 */
        int64_t d = ((int64_t)target - (int64_t)site) >> 2;
        *insn = (*insn & ~(0x7FFFFu << 5)) | (((uint32_t)d & 0x7FFFF) << 5);
        break;
    }
    case R_AARCH64_TSTBR14: {         /* tbz / tbnz : imm14 << 5, *4 */
        int64_t d = ((int64_t)target - (int64_t)site) >> 2;
        *insn = (*insn & ~(0x3FFFu << 5)) | (((uint32_t)d & 0x3FFF) << 5);
        break;
    }
    case R_AARCH64_ADD_ABS_LO12_NC:
        *insn = (*insn & ~(0xFFFu << 10)) | (((uint32_t)target & 0xFFF) << 10);
        break;
    case R_AARCH64_LDST8_ABS_LO12_NC:
    case R_AARCH64_LDST16_ABS_LO12_NC:
    case R_AARCH64_LDST32_ABS_LO12_NC:
    case R_AARCH64_LDST64_ABS_LO12_NC:
    case R_AARCH64_LDST128_ABS_LO12_NC: {
        int scale = type == R_AARCH64_LDST8_ABS_LO12_NC  ? 0 :
                    type == R_AARCH64_LDST16_ABS_LO12_NC ? 1 :
                    type == R_AARCH64_LDST32_ABS_LO12_NC ? 2 :
                    type == R_AARCH64_LDST64_ABS_LO12_NC ? 3 : 4;
        uint32_t imm = (uint32_t)((target & 0xFFF) >> scale);
        *insn = (*insn & ~(0xFFFu << 10)) | (imm << 10);
        break;
    }
    case R_AARCH64_PREL32:
    case R_X86_64_PC32:
    case R_X86_64_PLT32: {
        /* 32-bit PC-relative data word: S + A - P. `target` already carries the
         * addend (caller adds rl->add before calling); write S+A-P at the site.
         * Not an instruction field — a full 32-bit relative datum (switch/
         * relative tables for AARCH64_PREL32; x86_64's workhorse call/jmp/
         * lea-rip disp32 for PC32/PLT32 — vms-8f5, docs/design-link-x86_64-
         * relocs.md). PLT32 is written identically to PC32 here: an intra-link
         * callee needs no PLT stub, only a cross-image import would (out of
         * scope). Fixes a latent emit_shareable gap too. (vms-ba1) */
        int64_t d = (int64_t)target - (int64_t)site;
        *insn = (uint32_t)(uint64_t)d;
        break;
    }
    default:
        die("unsupported .text relocation (need a PC-relative type)");
    }
}

/* --------------------------------------------------------------------------
 * Global defined-symbol hash. Whole-archive musl links 1345 members with tens
 * of thousands of cross-object references; a linear symbol scan per reference
 * is O(refs · members · syms) and does not finish under emulation. A single
 * name -> (object, symbol) hash makes every cross-object resolution O(1) and is
 * what lets LINK.EXE ingest the whole archive in seconds, not minutes. (vms-004)
 * -------------------------------------------------------------------------- */
struct symref { const char *name; int oi; int ki; unsigned char bind; };
static struct symref *g_syms;
static size_t g_syms_cap;          /* power of two */

/* Undefined-external policy: with --allow-undefined a reference not defined by
 * any input object is a *deferred import* (satisfied later by the C RTL / a
 * companion shareable — vms-61f), recorded loudly rather than aborting. Off by
 * default: a normal single-shareable build still fails hard on a dangling ref. */
static int  g_allow_undef;
static long g_deferred;            /* count of deferred (unresolved) externals */

/* Producer GLOBALVALUE table (vms-954). A --use'd producer (DECC$SHR is the C
 * RTL surface) may export universals of kind OVMX_SV_GLOBALVALUE: absolute
 * link-time constants (VMS globalvalues — the errno message codes such as
 * C$_EXIT1 that the alpha-dec-vms crt0 references as `&C$_EXIT1`). Unlike a
 * PROCEDURE/DATA universal, a globalvalue is NOT bound at activation through an
 * import cell; it is a LINK-TIME constant, folded directly into every reference
 * (VMS resolves globalvalues at link, not activation). This table is built once
 * per link from the loaded producers' symbol vectors, then consulted by
 * resolve_ref() and the GOT/ABS64 apply so a reference to such a name resolves
 * to the constant WITHOUT a load bias and WITHOUT a .vms$imp/.vms$rel entry. */
struct gvalue { char name[256]; uint64_t value; };
static struct gvalue *g_gval;
static int            g_ngval;

/* Look up an absolute globalvalue by name; 1 + *out on hit, 0 on miss.
 * *out may be NULL when the caller only needs the yes/no. */
static int gval_find(const char *nm, uint64_t *out)
{
    for (int i = 0; i < g_ngval; i++)
        if (strcmp(g_gval[i].name, nm) == 0) {
            if (out) *out = g_gval[i].value;
            return 1;
        }
    return 0;
}

/* Collect every OVMX_SV_GLOBALVALUE universal across the loaded producers into
 * g_gval. Called at the top of a consumer/executable link, before the import
 * scan (a globalvalue must NOT become an import). Idempotent-safe: frees any
 * prior table first. */
static void collect_globalvalues(struct producer *ps, int np)
{
    free(g_gval); g_gval = NULL; g_ngval = 0;
    int cap = 0;
    for (int p = 0; p < np; p++) {
        const struct ovmx_sv_entry *e = ovmx_sv_entries(ps[p].sv);
        const char *nm = ovmx_sv_names(ps[p].sv);
        for (uint32_t i = 0; i < ps[p].sv->count; i++) {
            if (e[i].kind != OVMX_SV_GLOBALVALUE) continue;
            if (g_ngval >= cap) {
                cap = cap ? cap * 2 : 16;
                g_gval = realloc(g_gval, (size_t)cap * sizeof *g_gval);
                if (!g_gval) die("oom growing globalvalue table");
            }
            snprintf(g_gval[g_ngval].name, sizeof g_gval[g_ngval].name,
                     "%s", nm + e[i].name_off);
            g_gval[g_ngval].value = e[i].value;
            g_ngval++;
        }
    }
}

/* Names referenced with a WEAK undefined reference and defined by NO input
 * object. Standard ELF semantics resolve a weak undefined symbol to address 0 —
 * it is NOT a deferred import and NOT an error. For DECC$SHR these are exactly
 * the linker-defined section-boundary symbols __init_array_start/__init_array_end,
 * __fini_array_start/__fini_array_end and _DYNAMIC: musl's libc.a + libgcc.a
 * carry no .init_array/.fini_array sections and no dynamic section, so an empty
 * (start == end == 0) constructor range and a null _DYNAMIC are the *correct*
 * values — the C-RTL startup loop iterates zero static constructors. Resolving
 * them to 0 is what lets DECC$SHR link with ZERO deferred externals. (vms-61f.1) */
static char **g_weak;
static int    g_nweak;

static int weak_has(const char *name)
{
    for (int i = 0; i < g_nweak; i++)
        if (strcmp(g_weak[i], name) == 0) return 1;
    return 0;
}

static void weak_add(const char *name)
{
    if (weak_has(name)) return;
    g_weak = realloc(g_weak, (size_t)(g_nweak + 1) * sizeof *g_weak);
    if (!g_weak) die("oom recording weak-undefined symbol");
    g_weak[g_nweak++] = strdup(name);
}

static size_t djb2(const char *s)
{
    size_t h = 5381; int c;
    while ((c = (unsigned char)*s++)) h = ((h << 5) + h) + (size_t)c;
    return h;
}

/* Insert a defined global; prefer a STRONG (GLOBAL) def over a WEAK one. */
static void sym_insert(const char *name, int oi, int ki, unsigned char bind)
{
    size_t mask = g_syms_cap - 1;
    size_t i = djb2(name) & mask;
    for (;;) {
        if (!g_syms[i].name) {
            g_syms[i] = (struct symref){ name, oi, ki, bind };
            return;
        }
        if (strcmp(g_syms[i].name, name) == 0) {
            if (g_syms[i].bind == STB_WEAK && bind == STB_GLOBAL)
                g_syms[i] = (struct symref){ name, oi, ki, bind };  /* strong wins */
            return;
        }
        i = (i + 1) & mask;
    }
}

/* Look up a defined global by name -> (object index, symbol index), or -1. */
static int sym_lookup(const char *name, int *oi, int *ki)
{
    if (!g_syms) return 0;
    size_t mask = g_syms_cap - 1;
    size_t i = djb2(name) & mask;
    for (;;) {
        if (!g_syms[i].name) return 0;
        if (strcmp(g_syms[i].name, name) == 0) {
            *oi = g_syms[i].oi; *ki = g_syms[i].ki; return 1;
        }
        i = (i + 1) & mask;
    }
}

/* Build the hash over every defined (non-LOCAL, non-UNDEF) global symbol. */
static void build_symhash(struct obj *objs, int nobj)
{
    size_t total = 0;
    for (int i = 0; i < nobj; i++) total += (size_t)objs[i].nsym;
    size_t cap = 64;
    while (cap < total * 2 + 16) cap <<= 1;
    g_syms_cap = cap;
    g_syms = calloc(cap, sizeof *g_syms);
    if (!g_syms) die("oom building symbol hash");
    for (int i = 0; i < nobj; i++)
        for (int k = 0; k < objs[i].nsym; k++) {
            Elf64_Sym *s = &objs[i].sym[k];
            unsigned char bind = ELF64_ST_BIND(s->st_info);
            if (bind == STB_LOCAL || s->st_shndx == SHN_UNDEF) continue;
            const char *nm = objs[i].str + s->st_name;
            if (!nm[0]) continue;
            sym_insert(nm, i, k, bind);
        }
    /* Second pass: a WEAK undefined reference to a symbol no object defines
     * resolves to address 0 (ELF weak-undef semantics). Record such names so the
     * resolver returns 0 legitimately instead of aborting / deferring. (vms-61f.1) */
    for (int i = 0; i < nobj; i++)
        for (int k = 0; k < objs[i].nsym; k++) {
            Elf64_Sym *s = &objs[i].sym[k];
            if (s->st_shndx != SHN_UNDEF) continue;
            if (ELF64_ST_BIND(s->st_info) != STB_WEAK) continue;
            const char *nm = objs[i].str + s->st_name;
            if (!nm[0]) continue;
            int doi, dki;
            if (!sym_lookup(nm, &doi, &dki)) weak_add(nm);
        }
}

/* Map a symbol defined in object `d` to its final image vaddr, using the
 * per-section placement filled at layout. Returns 0 for a symbol in a section
 * this linker does not place flat (unplaced, or TLS — addressed via TLSDESC). */
static uint64_t placed_addr(struct obj *d, Elf64_Sym *s)
{
    int sh = (int)s->st_shndx;
    if (sh <= 0 || sh >= d->nsh) return 0;
    switch (d->sec_bucket[sh]) {
    case B_TEXT: case B_RODATA: case B_DATA: case B_INIT_ARRAY: case B_BSS:
    case B_EH_FRAME:
        return d->sec_va[sh] + s->st_value;
    default:
        return 0;
    }
}

/* Resolve a relocation's symbol to a final image vaddr (local or cross-object).
 * Returns 0 for a deferred external under --allow-undefined (caller skips the
 * patch); otherwise dies on a dangling reference. */
static uint64_t resolve_ref(struct obj *objs, int nobj, int oi, uint32_t symidx)
{
    struct obj *o = &objs[oi];
    Elf64_Sym *s = &o->sym[symidx];
    /* Weak-override (ELF symbol resolution): a WEAK symbol DEFINED in this
     * object must yield to a STRONG global definition of the same name in
     * another object. A relocation whose symtab entry is the locally-defined
     * WEAK symbol must therefore bind to the strong def, not the local weak one.
     *
     * This is load-bearing for whole-archive musl: lite_malloc.lo defines
     * __libc_malloc_impl as a WEAK alias of its __simple_malloc bump allocator,
     * and mallocng's malloc.lo defines __libc_malloc_impl STRONG. gcc emits the
     * intra-object JUMP26 in __libc_malloc / malloc against the *locally-defined
     * weak* __libc_malloc_impl, so binding it to the local address would route
     * the exported malloc to the simple allocator while free stays mallocng —
     * the freed pointer then carries no mallocng metadata and free's get_meta
     * dereferences p[-4] off the mapping (SIGSEGV). build_symhash already keeps
     * the STRONG def for a name, so resolving weak-defined references by name
     * through the global hash restores correct override. (vms-36a) */
    if (s->st_shndx != SHN_UNDEF &&
        ELF64_ST_BIND(s->st_info) == STB_WEAK) {
        const char *nm = o->str + s->st_name;
        int doi, dki;
        if (nm[0] && sym_lookup(nm, &doi, &dki) &&
            ELF64_ST_BIND(objs[doi].sym[dki].st_info) == STB_GLOBAL) {
            uint64_t da = placed_addr(&objs[doi], &objs[doi].sym[dki]);
            if (da) return da;
        }
    }
    uint64_t a = placed_addr(o, s);
    if (a) return a;
    if (s->st_shndx == SHN_UNDEF) {
        const char *nm = o->str + s->st_name;
        if (!nm[0]) die("undefined unnamed symbol in relocation");
        int doi, dki;
        if (sym_lookup(nm, &doi, &dki)) {
            uint64_t da = placed_addr(&objs[doi], &objs[doi].sym[dki]);
            if (da) return da;
        }
        /* A producer globalvalue (VMS globalvalue, e.g. C$_EXIT1): resolve to
         * its ABSOLUTE link-time constant. The caller's ABS64 apply must NOT
         * add a .vms$rel bias for it (it is absolute, not image-relative) — it
         * re-checks gval_find() to suppress that. (vms-954) */
        { uint64_t gv; if (gval_find(nm, &gv)) return gv; }
        if (weak_has(nm)) return 0;   /* weak-undef resolves to 0 (ELF semantics) */
        if (g_allow_undef) { g_deferred++; return 0; }
        /* NAME THE SYMBOL. Without it this diagnostic says only that *a*
         * symbol did not bind, which turns "one libc call was added to an
         * OVMX library whose producer image does not export it" -- the
         * single commonest way to break the VMS-native toolchain jobs --
         * into a manual nm/comm hunt across 40 objects and five producer
         * vectors. The name is the whole content of the report. */
        {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "unresolved external symbol '%s' (no --use'd shareable "
                     "exports it as a universal; if it is a C RTL entry point "
                     "it must be appended to DECC$SHR's symbol vector -- "
                     "vms-61f; pass --allow-undefined to record it as a "
                     "deferred import)", nm);
            die(buf);
        }
    }
    (void)nobj;
    /* Defined, but in a section this linker doesn't place flat (TLS, or an
     * allocatable type like SHT_NOTE): a pointer into it is deferred under
     * --allow-undefined, otherwise a hard error. (SHT_INIT_ARRAY is placed
     * flat as B_INIT_ARRAY as of vms-ee2 and no longer reaches this branch.) */
    if (g_allow_undef) { g_deferred++; return 0; }
    die("relocation against an unsupported section");
    return 0;
}

/* Resolve a symbol by NAME (global def) -> image vaddr. Returns 0 when not
 * defined by any input object (caller decides: die for a declared universal,
 * defer for a GOT import under --allow-undefined). */
static uint64_t resolve_named(struct obj *objs, int nobj,
                              const char *name, const char *whaterr)
{
    (void)nobj;
    int doi, dki;
    if (sym_lookup(name, &doi, &dki)) {
        uint64_t da = placed_addr(&objs[doi], &objs[doi].sym[dki]);
        if (da) return da;
    }
    if (whaterr) die(whaterr);
    return 0;
}

/* A synthesized GOT slot: one per distinct symbol referenced GOT-indirectly.
 *
 * GLOBAL-bind references dedup by NAME (is_local=0): one slot serves every
 * object that GOT-references the same global, resolved through the global
 * symbol hash (build_symhash / resolve_named).
 *
 * LOCAL-bind references (STB_LOCAL — statics, string-literal labels `L.n` that
 * tcc's arm64 backend routes through the GOT, arm64-gen.c:495-508) are NOT in
 * the global hash (build_symhash skips STB_LOCAL, link.c:755) and their names
 * are not unique across translation units — two TUs may each define their own
 * local `L.1`. Such references get a PER-OBJECT slot keyed by (oi, sym) and are
 * resolved directly via placed_addr() — the same mechanism resolve_ref() uses
 * for non-GOT relocations against locals — so cross-TU name collisions are
 * impossible and each local resolves to its own definition. (vms-9c1) */
struct gotslot {
    /* Global dedup key + diagnostic label: a pointer into the defining object's
     * .strtab (live for the whole run), NOT a fixed buffer. A truncating copy
     * would (a) miss at apply time — find_got does an exact strcmp against the
     * FULL reference name, so a >255-char mangled C++ template symbol stored
     * truncated never matches and dies "GOT slot missing" — and (b) silently
     * alias two distinct symbols that share a 255-char prefix (routine with
     * deeply-nested template instantiations). The pointer key has neither
     * failure and no arbitrary length cap. (vms-da2) */
    const char *name;
    uint64_t va;
    uint64_t value;
    int      is_local;   /* 1 = per-object local slot keyed by (oi, sym) */
    int      oi;         /* defining object index      (valid when is_local) */
    int      sym;        /* defining symbol index in oi (valid when is_local) */
};

/* Find an existing GLOBAL (name-keyed) slot. Local slots are never matched by
 * name — their names are not unique across objects. (vms-9c1) */
static int find_got(struct gotslot *g, int ng, const char *name)
{
    for (int i = 0; i < ng; i++)
        if (!g[i].is_local && strcmp(g[i].name, name) == 0) return i;
    return -1;
}

/* Find an existing per-object LOCAL slot keyed by (object index, symbol index). */
static int find_got_local(struct gotslot *g, int ng, int oi, int sym)
{
    for (int i = 0; i < ng; i++)
        if (g[i].is_local && g[i].oi == oi && g[i].sym == sym) return i;
    return -1;
}

/* Patch an ADR_GOT_PAGE / LD64_GOT_LO12_NC pair to reach `slot` PC-relatively.
 * Identical bit-layout to ADR_PREL_PG_HI21 / LDST64_ABS_LO12_NC, but the target
 * is the GOT cell rather than the symbol.
 *
 * x86_64 GOTPCREL/REX_GOTPCRELX (vms-cd1): a SINGLE relocation, not a pair —
 * `mov sym@GOTPCREL(%rip), reg` disassembles to a disp32 whose value is
 * GOT_entry_addr+A-P, written as a flat 32-bit word exactly like PC32/PLT32 in
 * patch_pcrel() (not bitfield-packed into the instruction). Unlike the aarch64
 * pair (addend always 0 in practice — the page/lo12 split carries no separate
 * addend slot LINK.EXE models), the x86_64 form's `add` is real: gcc/gas emit
 * addend -4 for GOTPCREL (site is the start of the 4-byte disp32 field, 4
 * bytes before the next instruction, so A=-4 makes S+A-P land on the GOT cell
 * relative to the instruction's end, matching how the CPU computes %rip at
 * execution time) — so `add` must be threaded through here and included in
 * the write, unlike the two aarch64 branches which don't take one. */
static void patch_got(uint32_t type, uint32_t *insn, uint64_t site, uint64_t slot,
                      int64_t add)
{
    if (type == R_AARCH64_ADR_GOT_PAGE) {
        int64_t d = (int64_t)(slot >> 12) - (int64_t)(site >> 12);
        uint32_t immlo = (uint32_t)(d & 3), immhi = (uint32_t)((d >> 2) & 0x7FFFF);
        *insn = (*insn & ~((3u << 29) | (0x7FFFFu << 5))) | (immlo << 29) | (immhi << 5);
    } else if (type == R_AARCH64_LD64_GOT_LO12_NC) { /* 8-byte load, scale 3 */
        uint32_t imm = (uint32_t)((slot & 0xFFF) >> 3);
        *insn = (*insn & ~(0xFFFu << 10)) | (imm << 10);
    } else { /* R_X86_64_GOTPCREL / R_X86_64_GOTPCRELX / R_X86_64_REX_GOTPCRELX */
        int64_t d = (int64_t)slot + add - (int64_t)site;
        *insn = (uint32_t)(uint64_t)d;
    }
}

static int is_got_reloc(uint32_t type)
{
    return type == R_AARCH64_ADR_GOT_PAGE || type == R_AARCH64_LD64_GOT_LO12_NC ||
           type == R_X86_64_GOTPCREL || type == R_X86_64_GOTPCRELX ||
           type == R_X86_64_REX_GOTPCRELX;
}

/* A synthesized TLSDESC entry (two quadwords): [0]=resolver (IMGACT fills with
 * __tlsdesc_static), [1]=TP-relative offset (LINK pre-fills the module-relative
 * part; IMGACT adds the module's assigned TLS block offset). */
/* name: a pointer into the defining object's live .strtab, not a fixed buffer
 * — same truncation/prefix-collision hazard as gotslot for long mangled C++
 * thread_local template names. (vms-da2) */
struct tlsslot { const char *name; int64_t addend; uint64_t va; uint64_t modoff; };

static int find_tls(struct tlsslot *t, int nt, const char *name)
{
    for (int i = 0; i < nt; i++) if (strcmp(t[i].name, name) == 0) return i;
    return -1;
}

/* Absolute 64-bit pointer-initializer relocation, either architecture: written
 * as a flat 8-byte S+A and recorded in .vms$rel for load-bias. (vms-8f5 adds
 * R_X86_64_64 alongside the existing R_AARCH64_ABS64.) */
static int is_abs64_reloc(uint32_t type)
{
    return type == R_AARCH64_ABS64 || type == R_X86_64_64;
}

static int is_tlsdesc_reloc(uint32_t type)
{
    return type == R_AARCH64_TLSDESC_ADR_PAGE21 ||
           type == R_AARCH64_TLSDESC_LD64_LO12 ||
           type == R_AARCH64_TLSDESC_ADD_LO12 ||
           type == R_AARCH64_TLSDESC_CALL ||
           type == R_X86_64_GOTPC32_TLSDESC ||
           type == R_X86_64_TLSDESC_CALL;
}

/* x86_64 TLSDESC relocations carry a FIELD addend (-4 on GOTPC32_TLSDESC, for
 * the disp32-field-vs-instruction-end delta), never a symbol offset — so the
 * addend must be excluded from the descriptor's module-offset computation,
 * unlike the aarch64 TLSDESC relocs whose addend IS a symbol offset. */
static int is_x86_tlsdesc_reloc(uint32_t type)
{
    return type == R_X86_64_GOTPC32_TLSDESC || type == R_X86_64_TLSDESC_CALL;
}

/* R_X86_64_DTPOFF32: the local-dynamic operand half — a plain absolute 32-bit
 * module-relative TLS offset written at the site, added at run time to the
 * module base the TLSDESC pair resolved. No image bias, no .vms$rel slot. */
static int is_dtpoff_reloc(uint32_t type)
{
    return type == R_X86_64_DTPOFF32;
}

/* True for a classic x86_64 general-/local-dynamic TLS lea reloc (vms-76a). Its
 * paired `call __tls_get_addr` (an R_X86_64_PLT32/GOTPCREL against the symbol
 * `__tls_get_addr`) is subsumed by the LE relaxation of this lea and must not be
 * patched separately — its bytes are overwritten by patch_tls_le(). */
static int is_classic_gdld_reloc(uint32_t type)
{
    return type == R_X86_64_TLSGD || type == R_X86_64_TLSLD;
}

/* GD/LD -> Local-Exec relaxation (x86_64 psABI). Valid for a SINGLE static image
 * (no dlopen): every classic general-/local-dynamic access becomes a local-exec
 * read of TP (%fs:0) plus a link-time-final TP-relative offset — the same value
 * the proven gnu2/TLSDESC path resolves at run time (the executable's TLS block
 * base sits at TP - ALIGN_UP(tls_memsz, tls_align), matching IMGACT's
 * assign_tls_offsets()). `site` is the VA of the TLSGD/TLSLD reloc field (the
 * lea's disp32); the fixed-size instruction window the psABI mandates begins 4
 * bytes (GD, 16-byte window) or 3 bytes (LD, 12-byte window) before it.
 *
 *   GD:  66 48 8d 3d <d32>          lea x@tlsgd(%rip),%rdi
 *        66 66 48 e8 <d32>          call __tls_get_addr@plt
 *     -> 64 48 8b 04 25 00 00 00 00 mov %fs:0,%rax
 *        48 8d 80 <tpoff32>         lea x@tpoff(%rax),%rax     (tpoff embedded)
 *
 *   LD:  48 8d 3d <d32>             lea x@tlsld(%rip),%rdi
 *        e8 <d32> | ff 15 <d32>     call __tls_get_addr@plt | *..@gotpcrel(%rip)
 *     -> 66 [66] 66 66              (3- or 4-byte 0x66 padding)
 *        64 48 8b 04 25 00 00 00 00 mov %fs:0,%rax             (leaves %rax = TP)
 * For LD the paired R_X86_64_DTPOFF32 operands carry each variable's TP-relative
 * offset (moff - aligned_tls_size), written by the DTPOFF32 arm below.
 *
 * BOTH call dialects occur in the wild: -fplt emits a 5-byte direct `e8` call,
 * -fno-plt (Alpine's libstdc++/libgcc, GOTPCRELX) a 6-byte indirect `ff 15`.
 * The GD lea+call window is 16 bytes for both (the direct call carries an extra
 * 0x66 prefix). The LD window is 12 bytes for the direct call, 13 for the
 * indirect, so its 0x66 padding is sized from the actual call opcode. */
static void patch_tls_le(uint32_t type, uint8_t *img, uint64_t site, int32_t tpoff)
{
    static const uint8_t movfs[9] = /* mov %fs:0, %rax */
        { 0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00 };
    if (type == R_X86_64_TLSGD) {
        uint8_t *w = img + site - 4;           /* 16-byte GD window */
        memcpy(w, movfs, 9);
        w[9] = 0x48; w[10] = 0x8d; w[11] = 0x80;  /* lea tpoff(%rax), %rax */
        memcpy(w + 12, &tpoff, 4);
    } else {                                   /* R_X86_64_TLSLD */
        /* lea is a fixed 7 bytes (48 8d 3d <d32>) starting 3 before the reloc
         * field, so the paired call begins at site+4. Its first opcode byte
         * selects the window length: 0xff (ff 15, indirect) -> 13-byte window
         * with 4-byte padding; anything else (0xe8, direct) -> 12-byte, 3-byte
         * padding. The extra 0x66 prefixes are ignored at execution. */
        uint8_t *w   = img + site - 3;
        int      pad = (img[site + 4] == 0xff) ? 4 : 3;
        for (int k = 0; k < pad; k++) w[k] = 0x66;
        memcpy(w + pad, movfs, 9);
    }
}

/* Patch a TLSDESC ADR_PAGE21 / LD64_LO12 / ADD_LO12 to reach the 2-word TLSDESC
 * entry PC-relatively (same encodings as ADRP / LDR64 / ADD-imm12). TLSDESC_CALL
 * is a marker at the blr and needs no patch.
 *
 * x86_64 (vms-2e4): GOTPC32_TLSDESC is a SINGLE flat disp32 —
 * descriptor_addr + A - P, the same write patch_got() does for GOTPCREL, so
 * `add` must be threaded through (A is -4 here). R_X86_64_TLSDESC_CALL, like
 * its aarch64 namesake, is a pure marker: writing anything at its 2-byte site
 * would clobber the next instruction. */
static void patch_tlsdesc(uint32_t type, uint32_t *insn, uint64_t site,
                          uint64_t slot, int64_t add)
{
    if (type == R_AARCH64_TLSDESC_ADR_PAGE21) {
        int64_t d = (int64_t)(slot >> 12) - (int64_t)(site >> 12);
        uint32_t immlo = (uint32_t)(d & 3), immhi = (uint32_t)((d >> 2) & 0x7FFFF);
        *insn = (*insn & ~((3u << 29) | (0x7FFFFu << 5))) | (immlo << 29) | (immhi << 5);
    } else if (type == R_AARCH64_TLSDESC_LD64_LO12) {
        uint32_t imm = (uint32_t)((slot & 0xFFF) >> 3);
        *insn = (*insn & ~(0xFFFu << 10)) | (imm << 10);
    } else if (type == R_AARCH64_TLSDESC_ADD_LO12) {
        *insn = (*insn & ~(0xFFFu << 10)) | (((uint32_t)slot & 0xFFF) << 10);
    } else if (type == R_X86_64_GOTPC32_TLSDESC) {
        int64_t d = (int64_t)slot + add - (int64_t)site;
        *insn = (uint32_t)(uint64_t)d;
    }
    /* R_AARCH64_TLSDESC_CALL / R_X86_64_TLSDESC_CALL: no-op markers. */
}

/* True if section index `sh` of object `o` is a thread-local section (.tdata /
 * .tbss, INCLUDING gcc's per-variable .tdata.<sym> / .tbss.<sym> split sections
 * that a function-local or COMDAT `thread_local` lands in). Classified by the
 * SHF_TLS flag in parse_obj, so the exact section name does not matter. */
static int is_tls_section(struct obj *o, int sh)
{
    return sh > 0 && sh < o->nsh &&
           (o->sec_bucket[sh] == B_TDATA || o->sec_bucket[sh] == B_TBSS);
}

/* Module-relative TLS offset of a TLS symbol: its byte offset within the image's
 * single combined TLS block. With multi-module TLS (vms-da2) the block holds
 * every input object's TLS sections — .tdata (and .tdata.*) concatenated, then
 * .tbss (and .tbss.*). Each such section was assigned its own base offset within
 * the block (stored in sec_va[] during emit_shareable's TLS-layout pass, which
 * placed_addr leaves untouched for TLS buckets), so a TLS symbol resolves to
 * (its defining section's block base + st_value). */
static uint64_t tls_module_offset(struct obj *objs, int nobj,
                                  const char *name, int64_t addend)
{
    /* x86_64 local-dynamic (vms-2e4): `_TLS_MODULE_BASE_` is a synthetic UND
     * symbol naming the combined block's base, defined by no object — offset 0
     * by definition. Each `static _Thread_local` access then adds its own
     * R_X86_64_DTPOFF32 operand offset (the symbol's combined-block offset) on
     * top, so the whole image is treated as one module with base 0. */
    if (strcmp(name, TLS_MODULE_BASE_SYM) == 0)
        return (uint64_t)addend;
    for (int j = 0; j < nobj; j++) {
        struct obj *d = &objs[j];
        for (int k = 0; k < d->nsym; k++) {
            Elf64_Sym *s = &d->sym[k];
            if (strcmp(d->str + s->st_name, name) != 0) continue;
            if (is_tls_section(d, (int)s->st_shndx))
                return d->sec_va[s->st_shndx] + s->st_value + (uint64_t)addend;
        }
    }
    fprintf(stderr, "%%LINK-F-ERROR, TLS symbol not defined in any input "
                    ".tdata/.tbss: %s\n", name);
    exit(1);
    return 0;
}

/* Module-relative TLS offset of the symbol a specific relocation names, resolved
 * in the REFERENCING object first (vms-2e4). R_X86_64_DTPOFF32 references are
 * normally STB_LOCAL — `static _Thread_local` variables — whose names are not
 * unique across translation units, so a name-keyed lookup is the wrong tool:
 * resolve directly through (object, symbol index) when the symbol is defined in
 * its own object, and fall back to the cross-object name lookup only for a
 * genuinely undefined (external / _TLS_MODULE_BASE_) reference. */
static uint64_t tls_ref_offset(struct obj *objs, int nobj, int oi, uint32_t si,
                               int64_t addend)
{
    struct obj *o = &objs[oi];
    Elf64_Sym  *s = &o->sym[si];
    if (s->st_shndx != SHN_UNDEF && is_tls_section(o, (int)s->st_shndx))
        return o->sec_va[s->st_shndx] + s->st_value + (uint64_t)addend;
    return tls_module_offset(objs, nobj, o->str + s->st_name, addend);
}

/* True if `name` is defined by some input object in a section this linker places
 * flat (text/rodata/data/bss) — i.e. resolve_named would yield a nonzero image
 * vaddr at emit. Used to distinguish an intra-image reference (resolved locally)
 * from a cross-image import (bound to a --use producer). build_symhash must have
 * run. (vms-e65) */
static int defined_placed(struct obj *objs, const char *name)
{
    int doi, dki;
    if (!sym_lookup(name, &doi, &dki)) return 0;
    Elf64_Sym *s = &objs[doi].sym[dki];
    int shx = (int)s->st_shndx;
    if (shx <= 0 || shx >= objs[doi].nsh) return 0;
    int b = objs[doi].sec_bucket[shx];
    return b == B_TEXT || b == B_RODATA || b == B_DATA || b == B_INIT_ARRAY ||
           b == B_BSS || b == B_EH_FRAME;
}

/* Emit an OVMX shareable image from N objects: merge .text/.rodata/.data/.bss,
 * apply PC-relative relocations (local + cross-object), synthesize a GOT for
 * GOT-indirect global references, and record every image-relative slot that
 * needs +load_bias in .vms$rel. Exports declared universals via .vms$sv. (vms-20b)
 *
 * Cross-image imports (vms-e65): a CALL/JUMP or GOT reference to a symbol not
 * defined by any input object but exported by a --use producer becomes an import
 * — a PLT stub (calls) + import-GOT cell + .vms$imp entry, bound at activation to
 * the producer universal. This is what lets a lib shareable resolve its libc/
 * pthread calls against DECC$SHR.
 *
 * is_exec (vms-ba1): the SAME machinery emits a leaf EXECUTABLE. An executable is
 * a shareable that (a) exports no symbol vector (nuniv may be 0), (b) carries
 * PT_PHDR + PT_INTERP=IMGACT.EXE program headers so the kernel activates it
 * through IMGACT, (c) force-binds exit() as an import, and (d) gets a synthesized
 * crt0 entry stub (e_entry) that recovers argc/argv/envp off the initial process
 * stack and calls main() then exit(). All the multi-object merge / reloc / GOT /
 * ABS64 .vms$rel / cross-image import logic is shared verbatim — a real main()
 * C program links exactly like a library does. */
static void emit_shareable(struct obj *objs, int nobj, struct univ *uv, int nuniv,
                           uint32_t gk, uint32_t gmaj, uint32_t gmin,
                           int allow_undef, struct producer *ps, int np,
                           const char *out, int is_exec)
{
    g_allow_undef = allow_undef;
    g_deferred = 0;
    build_symhash(objs, nobj);
    /* Collect producer globalvalues (VMS globalvalues — absolute link-time
     * constants exported by a --use'd producer, e.g. C$_EXIT1 from DECC$SHR)
     * before the import scan, so a reference to one folds the constant instead
     * of becoming an activation import. (vms-954) */
    collect_globalvalues(ps, np);

    /* Executable entry mode. An object set that defines its own `_start` is a
     * FREESTANDING program (it owns entry + exit — the pre-vms-ba1 consumers and
     * STARTUP.EXE): e_entry -> _start, no crt0, no C-RTL exit import. Otherwise a
     * `main()` program gets the synthesized crt0 (argc/argv/envp -> main -> exit).
     * A shareable (is_exec == 0) uses neither. (vms-ba1) */
    int exe_start = 0;   /* executable defines its own _start (freestanding)   */
    if (is_exec) {
        int oi, ki;
        exe_start = sym_lookup("_start", &oi, &ki);
        if (!exe_start && !sym_lookup("main", &oi, &ki))
            die("--executable object set defines neither _start nor main()");
    }
    int use_crt0 = is_exec && !exe_start;   /* synthesize crt0 for a main() prog */

    /* ---- Cross-image imports (bind to --use producers). Scan every reloc: a
     * CALL/JUMP26 or GOT reference to a symbol that is UNDEF in its own object,
     * not defined by any input object, but exported by a --use producer, is an
     * import. Call uses get a PLT stub; a symbol used as a call anywhere forces a
     * stub even if also address-taken. These are excluded from the intra-image
     * GOT below and routed to import cells IMGACT fills via .vms$imp. (vms-e65) */
    struct import *imp = NULL; int nimp = 0, imp_cap = 0;
    for (int i = 0; i < nobj; i++)
        for (int r = 0; r < objs[i].nreloc; r++) {
            uint32_t type = ELF64_R_TYPE(objs[i].relocs[r].info);
            /* R_X86_64_PLT32 is x86_64's call/jmp relocation (vms-206): the
             * SAME reloc type covers both an intra-image callee (patched as a
             * plain PC32-style datum below, no stub) and a cross-image call
             * to a --use producer's universal (routed here exactly like
             * aarch64's CALL26/JUMP26, into a PLT stub). Which one applies is
             * decided by defined_placed()/find_universal() below, same as the
             * aarch64 path -- an intra-image PLT32 never reaches find_universal
             * because defined_placed() is true and the loop `continue`s. */
            int is_call = (type == R_AARCH64_CALL26 || type == R_AARCH64_JUMP26 ||
                           type == R_X86_64_PLT32);
            int is_gotr = is_got_reloc(type);
            if (!is_call && !is_gotr) continue;
            uint32_t si = ELF64_R_SYM(objs[i].relocs[r].info);
            Elf64_Sym *s = &objs[i].sym[si];
            if (s->st_shndx != SHN_UNDEF) continue;      /* locally defined     */
            const char *nm = objs[i].str + s->st_name;
            if (!nm[0]) continue;
            /* __tls_get_addr calls are the classic-GD/LD access model; LINK
             * relaxes every one to Local-Exec (patch_tls_le), overwriting the
             * call site, so the symbol is never actually referenced at run time.
             * Do NOT create an import/PLT stub for it. (vms-76a) */
            if (strcmp(nm, "__tls_get_addr") == 0) continue;
            if (defined_placed(objs, nm)) continue;      /* intra-image def     */
            int pidx; uint32_t svidx;
            if (!find_universal(ps, np, nm, &pidx, &svidx))
                continue;   /* not a producer universal: weak/deferred path below */
            if (ovmx_sv_entries(ps[pidx].sv)[svidx].kind == OVMX_SV_GLOBALVALUE)
                continue;   /* globalvalue: a LINK-TIME constant folded at the
                             * reference site (resolve_ref / GOT fill), never an
                             * activation-bound import. (vms-954) */
            int k = import_find(imp, nimp, nm);
            if (k < 0) {
                if (nimp >= imp_cap) {
                    imp_cap = imp_cap ? imp_cap * 2 : 32;
                    imp = realloc(imp, (size_t)imp_cap * sizeof *imp);
                    if (!imp) die("oom growing import table");
                }
                k = nimp++;
                memset(&imp[k], 0, sizeof imp[k]);
                snprintf(imp[k].name, sizeof imp[k].name, "%s", nm);
                imp[k].pidx = pidx; imp[k].svidx = svidx;
                imp[k].is_data = is_gotr ? 1 : 0;
            }
            if (is_call) imp[k].is_data = 0;   /* any call use needs a PLT stub */
        }

    /* ---- Weak-by-name imports (vms-5f0). A CALL/GOT reference to a symbol that
     * is UNDEF in its own object, defined by NO input object, exported by NO
     * --use'd producer (the strong-import scan above skipped it), but declared
     * `#pragma weak` in the source (build_symhash recorded it in g_weak) is NOT
     * a link error and NOT a bake-to-0: it becomes a WEAK import. LINK emits a
     * PLT stub + import-GOT cell for it exactly like a strong import, but records
     * it in .vms$wimp for IMGACT to resolve by NAME against the loaded producer
     * set at activation -- found -> bound, absent -> the cell stays 0 (the ELF
     * weak-undef result rms_services_present() reads as "service not present").
     *
     * This is the ONLY way a lower-layer producer can reach a universal a
     * HIGHER-layer producer exports: LIBVMS$SHR's rms_textfile.c weak-references
     * sys$open/$get/$connect/$close, exported by LIBVMSRMS$SHR, which --use's
     * LIBVMS$SHR -- so LIBVMS$SHR cannot --use LIBVMSRMS$SHR to import them by
     * (producer,index) without a build cycle. IMGACT's by-name activation bind
     * closes that cycle, matching how VMS resolves inter-shareable references.
     *
     * Linker-defined weak-undef section symbols (__init_array_start/_DYNAMIC on
     * DECC$SHR) also land here: no producer exports them, so IMGACT leaves them
     * 0 -- identical to today's bake-to-0, so including them is harmless. */
    for (int i = 0; i < nobj; i++)
        for (int r = 0; r < objs[i].nreloc; r++) {
            uint32_t type = ELF64_R_TYPE(objs[i].relocs[r].info);
            int is_call = (type == R_AARCH64_CALL26 || type == R_AARCH64_JUMP26 ||
                           type == R_X86_64_PLT32);
            int is_gotr = is_got_reloc(type);
            if (!is_call && !is_gotr) continue;
            uint32_t si = ELF64_R_SYM(objs[i].relocs[r].info);
            Elf64_Sym *s = &objs[i].sym[si];
            if (s->st_shndx != SHN_UNDEF) continue;
            const char *nm = objs[i].str + s->st_name;
            if (!nm[0]) continue;
            if (!weak_has(nm)) continue;                 /* only weak-undef refs */
            if (defined_placed(objs, nm)) continue;      /* intra-image def      */
            int pidx; uint32_t svidx;
            if (find_universal(ps, np, nm, &pidx, &svidx))
                continue;   /* a --use producer exports it: already a strong import */
            int k = import_find(imp, nimp, nm);
            if (k < 0) {
                if (nimp >= imp_cap) {
                    imp_cap = imp_cap ? imp_cap * 2 : 32;
                    imp = realloc(imp, (size_t)imp_cap * sizeof *imp);
                    if (!imp) die("oom growing import table");
                }
                k = nimp++;
                memset(&imp[k], 0, sizeof imp[k]);
                snprintf(imp[k].name, sizeof imp[k].name, "%s", nm);
                imp[k].pidx = -1;          /* producer chosen by IMGACT by name */
                imp[k].is_weak = 1;
                imp[k].is_data = is_gotr ? 1 : 0;
            }
            if (is_call) imp[k].is_data = 0;   /* any call use needs a PLT stub */
        }

    /* An executable's synthesized crt0 (below) tail-calls exit() to flush the
     * C-RTL and terminate. Bind exit as a call import even when no input object
     * references it directly, so IMGACT resolves it from a --use producer
     * (DECC$SHR) at activation and the crt0 `bl exit` reaches a real PLT stub. */
    int exit_imp = -1;
    if (use_crt0) {
        exit_imp = import_find(imp, nimp, "exit");
        if (exit_imp < 0) {
            int pidx; uint32_t svidx;
            if (!find_universal(ps, np, "exit", &pidx, &svidx))
                die("--executable needs exit() from a --use producer (DECC$SHR)");
            if (nimp >= imp_cap) {
                imp_cap = imp_cap ? imp_cap * 2 : 32;
                imp = realloc(imp, (size_t)imp_cap * sizeof *imp);
                if (!imp) die("oom growing import table");
            }
            exit_imp = nimp++;
            memset(&imp[exit_imp], 0, sizeof imp[exit_imp]);
            snprintf(imp[exit_imp].name, sizeof imp[exit_imp].name, "%s", "exit");
            imp[exit_imp].pidx = pidx; imp[exit_imp].svidx = svidx;
        }
        imp[exit_imp].is_data = 0;   /* always a call import */
    }

    int has_ro = 0, has_data = 0, has_init_array = 0, has_bss = 0, has_eh_frame = 0;
    for (int i = 0; i < nobj; i++)
        for (int s = 0; s < objs[i].nsh; s++) {
            if (objs[i].sh[s].sh_size == 0) continue;
            if (objs[i].sec_bucket[s] == B_RODATA)     has_ro = 1;
            if (objs[i].sec_bucket[s] == B_DATA)       has_data = 1;
            if (objs[i].sec_bucket[s] == B_INIT_ARRAY) has_init_array = 1;
            if (objs[i].sec_bucket[s] == B_BSS)        has_bss = 1;
            if (objs[i].sec_bucket[s] == B_EH_FRAME)   has_eh_frame = 1;
        }

    /* TLS geometry: COMBINED multi-module TLS block (vms-da2). A C++ image
     * whole-archives libstdc++/libsupc++/libgcc, each of which can contribute
     * its own .tdata/.tbss; a single image therefore has MANY TLS-bearing
     * objects, not one. LINK builds ONE combined per-thread TLS block for the
     * whole image and emits a SINGLE PT_TLS over it — matching what a real
     * linker (ld) does when it statically combines a program with its runtime.
     *
     * Layout (the ELF-standard TLS block shape): every module's .tdata is
     * concatenated first (the file-backed init image = PT_TLS p_filesz), then
     * every module's .tbss (zero-fill = the p_memsz tail). Each section is
     * placed at its own alignment; each object records the base offset it was
     * assigned (tls_tdata_off / tls_tbss_off) so a TLS symbol resolves to
     * (its module's base + st_value). No per-image cap and no one-object
     * limitation — the count of TLS-bearing objects is unbounded. */
    uint64_t tls_align = 1;   /* max alignment over all TLS sections (>=1) */
    /* Pass 1: place every .tdata / .tdata.* (initialized image) contiguously.
     * Each section's block-relative base offset is recorded in sec_va[] (which
     * placed_addr ignores for TLS buckets), so a TLS symbol later resolves to
     * (its section's base + st_value) regardless of which object or which split
     * per-variable section it came from. */
    uint64_t tls_cursor = 0;
    for (int i = 0; i < nobj; i++)
        for (int s = 0; s < objs[i].nsh; s++) {
            if (objs[i].sec_bucket[s] != B_TDATA || !objs[i].sh[s].sh_size) continue;
            uint64_t al = objs[i].sh[s].sh_addralign ? objs[i].sh[s].sh_addralign : 8;
            if (al > tls_align) tls_align = al;
            tls_cursor = ALIGN_UP(tls_cursor, al);
            objs[i].sec_va[s] = tls_cursor;
            tls_cursor += objs[i].sh[s].sh_size;
        }
    uint64_t tdata_sz = tls_cursor;   /* total .tdata = PT_TLS p_filesz */
    /* Pass 2: place every .tbss / .tbss.* (zero image) after all .tdata. */
    for (int i = 0; i < nobj; i++)
        for (int s = 0; s < objs[i].nsh; s++) {
            if (objs[i].sec_bucket[s] != B_TBSS || !objs[i].sh[s].sh_size) continue;
            uint64_t al = objs[i].sh[s].sh_addralign ? objs[i].sh[s].sh_addralign : 1;
            if (al > tls_align) tls_align = al;
            tls_cursor = ALIGN_UP(tls_cursor, al);
            objs[i].sec_va[s] = tls_cursor;
            tls_cursor += objs[i].sh[s].sh_size;
        }
    uint64_t tls_memsz = tls_cursor;  /* total block = PT_TLS p_memsz */
    uint64_t tbss_sz   = tls_memsz - tdata_sz; /* zero-tail size (diagnostic/hdr) */
    int has_tls = (tls_memsz > 0);

    /* Collect the distinct GOT-referenced symbols (across all code sections).
     * Growable — musl references hundreds of globals GOT-indirectly. (vms-004) */
    struct gotslot *got = NULL; int ngot = 0, got_cap = 0;
    for (int i = 0; i < nobj; i++)
        for (int r = 0; r < objs[i].nreloc; r++) {
            uint32_t type = ELF64_R_TYPE(objs[i].relocs[r].info);
            if (!is_got_reloc(type)) continue;
            uint32_t si = ELF64_R_SYM(objs[i].relocs[r].info);
            Elf64_Sym *sym = &objs[i].sym[si];
            const char *nm = objs[i].str + sym->st_name;
            int is_local = (ELF64_ST_BIND(sym->st_info) == STB_LOCAL);
            /* A LOCAL-bind GOT reference (tcc's per-TU statics / `L.n` string
             * labels) is resolved per-object by (oi, symidx), never by the
             * global name hash — so two TUs' distinct local `L.1`s get DISTINCT
             * slots and each resolves to its own definition. (vms-9c1) */
            if (is_local) {
                if (find_got_local(got, ngot, i, (int)si) >= 0) continue;
            } else {
                /* A GLOBAL GOT reference bound to a --use producer is a
                 * cross-image DATA import (its own import cell, filled via
                 * .vms$imp) — not an intra-image GOT slot. (vms-e65) */
                if (import_find(imp, nimp, nm) >= 0) continue;
                if (find_got(got, ngot, nm) >= 0) continue;
            }
            if (ngot >= got_cap) {
                got_cap = got_cap ? got_cap * 2 : 256;
                got = realloc(got, (size_t)got_cap * sizeof *got);
                if (!got) die("oom growing GOT table");
            }
            got[ngot].name = nm;   /* strtab pointer, live for the run */
            got[ngot].is_local = is_local;
            got[ngot].oi  = is_local ? i : 0;
            got[ngot].sym = is_local ? (int)si : 0;
            ngot++;
        }

    /* Collect the distinct TLSDESC-referenced symbols (one 2-word entry each). */
    struct tlsslot *tls = NULL; int ntls = 0, tls_cap = 0;
    for (int i = 0; i < nobj; i++)
        for (int r = 0; r < objs[i].nreloc; r++) {
            uint32_t type = ELF64_R_TYPE(objs[i].relocs[r].info);
            if (!is_tlsdesc_reloc(type)) continue;
            uint32_t si = ELF64_R_SYM(objs[i].relocs[r].info);
            const char *nm = objs[i].str + objs[i].sym[si].st_name;
            if (find_tls(tls, ntls, nm) < 0) {
                if (ntls >= tls_cap) {
                    tls_cap = tls_cap ? tls_cap * 2 : 64;
                    tls = realloc(tls, (size_t)tls_cap * sizeof *tls);
                    if (!tls) die("oom growing TLSDESC table");
                }
                tls[ntls].name = nm;   /* strtab pointer, live for the run */
                /* aarch64's TLSDESC addend is a symbol offset and belongs in the
                 * descriptor's module offset; x86_64's is a disp32 FIELD addend
                 * (-4) that belongs only in the PC-relative write. (vms-2e4) */
                tls[ntls].addend = is_x86_tlsdesc_reloc(type)
                                   ? 0 : objs[i].relocs[r].add;
                ntls++;
            }
        }

    /* Count ABS64 data-pointer relocations up front — each needs a .vms$rel
     * slot (image-relative pointer biased at activation), so the section must
     * be sized before layout. (vms-004) */
    int nabs = 0;
    for (int i = 0; i < nobj; i++)
        for (int r = 0; r < objs[i].nreloc; r++)
            if (is_abs64_reloc(ELF64_R_TYPE(objs[i].relocs[r].info))) {
                /* A globalvalue ABS64 reference resolves to an ABSOLUTE constant
                 * that is NOT recorded in .vms$rel (the apply loop skips it), so
                 * it must not inflate the .vms$rel upper bound either — else an
                 * image whose only ABS64 ref is a globalvalue would carry an
                 * empty .vms$rel. (vms-954) */
                const char *anm = objs[i].str +
                    objs[i].sym[ELF64_R_SYM(objs[i].relocs[r].info)].st_name;
                if (gval_find(anm, NULL)) continue;
                nabs++;
            }

    /* ---- Layout: [ehdr][phdr] text|rodata|got|tlsdesc|data|init_array|tdata|sv|rel|tls|bss --- */
    uint64_t off_ph   = sizeof(Elf64_Ehdr);
    /* shareable: PT_LOAD (+ PT_TLS). executable: PT_PHDR, PT_INTERP, PT_LOAD
     * (+ PT_TLS) — the kernel maps the executable and reads PT_INTERP=IMGACT. */
    int      nph      = is_exec ? (3 + (has_tls ? 1 : 0)) : (has_tls ? 2 : 1);
    uint64_t cur      = ALIGN_UP(off_ph + nph * sizeof(Elf64_Phdr), 16);

    /* executable: the PT_INTERP string (IMGACT.EXE), placed in the loaded range
     * ahead of .text so its file offset == vaddr (identity map). */
    uint64_t off_interp = 0, interp_sz = 0;
    if (is_exec) {
        off_interp = cur;
        interp_sz  = strlen(IMGACT_INTERP) + 1;
        cur = ALIGN_UP(cur + interp_sz, 16);
    }

    /* Place every input section of a bucket contiguously, recording each
     * section's assigned image vaddr in objs[i].sec_va[] for symbol resolution.
     * `bkt` selects which bucket; returns the end cursor. */
    uint64_t text_beg = cur;
    for (int i = 0; i < nobj; i++)
        for (int s = 0; s < objs[i].nsh; s++)
            if (objs[i].sec_bucket[s] == B_TEXT && objs[i].sh[s].sh_size) {
                uint64_t al = objs[i].sh[s].sh_addralign ? objs[i].sh[s].sh_addralign : 4;
                cur = ALIGN_UP(cur, al < 4 ? 4 : al);
                objs[i].sec_va[s] = cur;
                cur += objs[i].sh[s].sh_size;
            }
    /* executable: reserve the crt0 entry stub at the end of the merged text.
     * crt0 reads argc/argv/envp off the initial process stack (the kernel set it
     * up; IMGACT's _start preserves SP across the bl to imgact_bootstrap), calls
     * main(argc,argv,envp), then tail-calls exit(). e_entry -> crt0. (vms-ba1) */
    uint64_t crt0_va = 0;
    if (use_crt0) {
        cur = ALIGN_UP(cur, 4);
        crt0_va = cur;
        cur += CRT0_NINSN * 4;
    }
    uint64_t text_end = cur;
    uint64_t ro_beg = cur;
    for (int i = 0; i < nobj; i++)
        for (int s = 0; s < objs[i].nsh; s++)
            if (objs[i].sec_bucket[s] == B_RODATA && objs[i].sh[s].sh_size) {
                uint64_t al = objs[i].sh[s].sh_addralign ? objs[i].sh[s].sh_addralign : 8;
                cur = ALIGN_UP(cur, al);
                objs[i].sec_va[s] = cur;
                cur += objs[i].sh[s].sh_size;
            }
    uint64_t ro_end = cur;

    /* .eh_frame (vms-70d): the DWARF unwinder CIE/FDE table, placed in ONE
     * contiguous region (like .init_array) so the whole block can be handed to
     * libgcc's __register_frame as a single [begin .. 0-terminator] range. gcc
     * emits exactly one `.eh_frame` per object; whole-archiving libstdc++/libgcc
     * yields many, concatenated here in object order. A 4-byte-zero FDE
     * terminator is appended after the last one (the image is calloc'd, so the
     * reserved word is already zero) -- the terminating null crtbegin/crtend
     * would otherwise supply. Read-only; lives in the single PT_LOAD. */
    uint64_t ehf_beg = cur, ehf_end = cur;
    if (has_eh_frame) {
        int first = 1;
        for (int i = 0; i < nobj; i++)
            for (int s = 0; s < objs[i].nsh; s++)
                if (objs[i].sec_bucket[s] == B_EH_FRAME && objs[i].sh[s].sh_size) {
                    uint64_t al = objs[i].sh[s].sh_addralign ? objs[i].sh[s].sh_addralign : 8;
                    cur = ALIGN_UP(cur, al);
                    if (first) { ehf_beg = cur; first = 0; }
                    objs[i].sec_va[s] = cur;
                    cur += objs[i].sh[s].sh_size;
                }
        cur = ALIGN_UP(cur, 4);   /* 4-byte-zero terminator after the last FDE */
        cur += 4;
        ehf_end = cur;
    }

    /* GOT cells (writable, 8-aligned). */
    uint64_t got_beg = ALIGN_UP(cur, 8);
    for (int i = 0; i < ngot; i++) got[i].va = got_beg + (uint64_t)i * 8;
    uint64_t got_end = got_beg + (uint64_t)ngot * 8;
    cur = got_end;

    /* TLSDESC entries (writable, 2 quadwords = 16 bytes each). The x86_64
     * TLSDESC ABI requires each descriptor 16-byte ALIGNED: a
     * `lea sym@TLSDESC(%rip),%rax` must resolve to a descriptor boundary, and
     * the resolver treats %rax as a 16-byte-aligned [resolver,offset] pair.
     * Align the TABLE BASE to 16 (was 8, vms-da2's combined-TLS-block reorg
     * shifted `cur` so an 8-aligned base landed at 8 mod 16 -> descriptors off
     * boundary; caught by the x86_64 TLSX86.EXE reloc test). i*16 then keeps
     * every entry ≡0 mod 16. */
    uint64_t tlsdesc_beg = ALIGN_UP(cur, 16);
    for (int i = 0; i < ntls; i++) tls[i].va = tlsdesc_beg + (uint64_t)i * 16;
    uint64_t tlsdesc_end = tlsdesc_beg + (uint64_t)ntls * 16;
    cur = tlsdesc_end;

    /* .data (writable, initialized). */
    uint64_t data_beg = cur;
    for (int i = 0; i < nobj; i++)
        for (int s = 0; s < objs[i].nsh; s++)
            if (objs[i].sec_bucket[s] == B_DATA && objs[i].sh[s].sh_size) {
                uint64_t al = objs[i].sh[s].sh_addralign ? objs[i].sh[s].sh_addralign : 8;
                cur = ALIGN_UP(cur, al);
                objs[i].sec_va[s] = cur;
                cur += objs[i].sh[s].sh_size;
            }
    uint64_t data_end = cur;

    /* .init_array (writable, ABS64-relocated ctor function-pointer table,
     * vms-ee2). A DEDICATED region -- deliberately not merged into .data --
     * so the placed range is exactly the ctor table, nothing else: IMGACT's
     * symbol-vector activator (imgact.c) reads this section's own sh_addr/
     * sh_size (via the same generic by-name section lookup it already uses
     * for .vms$imp/.vms$rel/.vms$tls/.vms$sv) to bound its constructor-call
     * loop. Each entry is patched by the ordinary ABS64 reloc-apply loop
     * below (bucket_is_patchable() now includes B_INIT_ARRAY) -- no special
     * casing needed there. Most images (TCC.EXE, DECC$SHR, every pure musl+
     * libgcc image) carry no SHT_INIT_ARRAY input section at all, so
     * has_init_array is 0 and this region is simply empty (initarr_beg ==
     * initarr_end): the correct, unaffected case.
     *
     * ORDERING (vms-0962): the input SHT_INIT_ARRAY sections MUST be laid down
     * in GNU-ld order, not object-encounter order. gcc emits a priority-tagged
     * constructor into its own section `.init_array.NNNNN` (NNNNN = the numeric
     * init_priority, zero-padded); untagged (default-priority) ctors land in
     * plain `.init_array`. GNU ld's default script places
     *   KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))   -- numbered, ASCENDING
     *   KEEP(*(.init_array))                             -- plain, AFTER those
     * Because .init_array entries execute front-to-back at activation, a lower
     * NNNNN (higher priority) must be placed EARLIER so it runs EARLIER. libstdc++
     * has hundreds of priority-ordered ctors (std::ios_base::Init, locale facets,
     * ...); concatenated in object order a later-priority ctor can run before the
     * earlier-priority ctor that establishes the state it reads -> garbage ptr ->
     * SIGSEGV in static init. Sort a flat (obj,sec) index list by the parsed
     * priority (plain = sentinel MAX, so it sorts last), ties broken by encounter
     * order (stable, matching ld's input order), then assign sec_va in that order.
     * The copy + ABS64 reloc-apply loops below are keyed on sec_va, so they follow
     * this ordering with no further change. */
    uint64_t initarr_beg = cur;
    if (has_init_array) {
        /* Flat list of every non-empty B_INIT_ARRAY input section. */
        int nia_cap = 0;
        for (int i = 0; i < nobj; i++)
            for (int s = 0; s < objs[i].nsh; s++)
                if (objs[i].sec_bucket[s] == B_INIT_ARRAY && objs[i].sh[s].sh_size)
                    nia_cap++;
        struct ia_ent { int i, s; uint64_t prio; int order; } *ia =
            nia_cap ? malloc((size_t)nia_cap * sizeof *ia) : NULL;
        int nia = 0;
        for (int i = 0; i < nobj; i++)
            for (int s = 0; s < objs[i].nsh; s++)
                if (objs[i].sec_bucket[s] == B_INIT_ARRAY && objs[i].sh[s].sh_size) {
                    const char *nm = objs[i].shstr + objs[i].sh[s].sh_name;
                    /* `.init_array.NNNNN` -> prio = NNNNN (ascending). Plain
                     * `.init_array` (no numeric suffix) -> sentinel MAX = last. */
                    uint64_t prio = UINT64_MAX;
                    const char *pfx = ".init_array.";
                    size_t pl = strlen(pfx);
                    if (strncmp(nm, pfx, pl) == 0 && nm[pl] >= '0' && nm[pl] <= '9')
                        prio = strtoull(nm + pl, NULL, 10);
                    ia[nia].i = i; ia[nia].s = s; ia[nia].prio = prio;
                    ia[nia].order = nia;
                    nia++;
                }
        /* Stable ascending sort by (prio, encounter order). Section counts are
         * modest (hundreds); an in-place insertion sort is clearer than qsort_r
         * and preserves ld's stable input-order tiebreak trivially. */
        for (int a = 1; a < nia; a++) {
            struct ia_ent key = ia[a];
            int b = a - 1;
            while (b >= 0 && (ia[b].prio > key.prio ||
                             (ia[b].prio == key.prio && ia[b].order > key.order))) {
                ia[b + 1] = ia[b];
                b--;
            }
            ia[b + 1] = key;
        }
        for (int k = 0; k < nia; k++) {
            int i = ia[k].i, s = ia[k].s;
            uint64_t al = objs[i].sh[s].sh_addralign ? objs[i].sh[s].sh_addralign : 8;
            cur = ALIGN_UP(cur, al);
            objs[i].sec_va[s] = cur;
            cur += objs[i].sh[s].sh_size;
        }
        free(ia);
    }
    uint64_t initarr_end = cur;

    /* .tdata (TLS init image, file-backed). PT_TLS references it; a reserved
     * vaddr is assigned even for a pure-.tbss image (tdata_sz == 0). */
    uint64_t tdata_va = 0, tdata_end = initarr_end;
    if (has_tls) {
        cur = ALIGN_UP(cur, tls_align);
        tdata_va = cur;
        cur += tdata_sz;
        tdata_end = cur;
    }

    /* Import-GOT cells: one per cross-image import (writable — IMGACT fills each
     * with the resolved producer address from .vms$imp at activation). Excluded
     * from .vms$rel (absolute, not load-biased). (vms-e65) */
    uint64_t impgot_beg = ALIGN_UP(cur, 8);
    for (int i = 0; i < nimp; i++) imp[i].got_va = impgot_beg + (uint64_t)i * 8;
    uint64_t impgot_end = impgot_beg + (uint64_t)nimp * 8;
    cur = impgot_end;

    /* PLT stubs: one 12-byte slot per import (only call imports emit a stub, but
     * a uniform per-import slot keeps indexing trivial). Executable — lives in
     * the RWX PT_LOAD. (vms-e65) */
    uint64_t plt_beg = ALIGN_UP(cur, 4);
    for (int i = 0; i < nimp; i++) imp[i].plt_va = plt_beg + (uint64_t)i * 12;
    uint64_t plt_end = plt_beg + (uint64_t)nimp * 12;
    cur = plt_end;

    uint64_t off_sv = ALIGN_UP(cur, 8);

    /* A RETIRED slot (PRIVATE_*) holds no address: the routine it named is gone
     * from the image, which is why the slot was retired instead of deleted. Its
     * value stays 0 and no reader ever dereferences it — find_universal(),
     * ovmx_sv_at() and IMGACT's sv_find_named() all skip OVMX_SV_RETIRED. */
    for (int i = 0; i < nuniv; i++) {
        if (uv[i].kind == OVMX_SV_RETIRED)
            uv[i].value = 0;
        else if (uv[i].kind == OVMX_SV_GLOBALVALUE)
            /* absolute link-time constant, preset at parse — keep it (no input
             * symbol defines it; it is bound unbiased by ovmx_sv_resolve) */
            ;
        else
            uv[i].value = resolve_named(objs, nobj, uv[i].internal,
                            "universal symbol not defined in any input object");
    }

    uint32_t names_size = 0;
    for (int i = 0; i < nuniv; i++) names_size += (uint32_t)strlen(uv[i].name) + 1;
    uint64_t sv_hdr_sz = sizeof(struct ovmx_sv_header);
    uint64_t sv_names_o = sv_hdr_sz + (uint64_t)nuniv * sizeof(struct ovmx_sv_entry);
    uint64_t sv_size = sv_names_o + names_size;

    /* .vms$rel: one image-relative offset per GOT slot AND per ABS64 data
     * pointer — all hold an image-relative address biased at activation. Sized
     * for the upper bound (ngot + nabs); the header count records how many were
     * actually resolved (deferred imports are excluded). (vms-004) */
    int nrel = ngot + nabs;
    uint64_t off_rel = 0, rel_size = 0;
    if (nrel) {
        off_rel = ALIGN_UP(off_sv + sv_size, 8);
        rel_size = sizeof(struct ovmx_rel_header) + (uint64_t)nrel * 8;
    }

    /* .vms$tls: one image-relative offset per TLSDESC entry. */
    uint64_t after_rel = nrel ? off_rel + rel_size : off_sv + sv_size;
    int ntlsdesc = ntls;
    uint64_t off_tls = 0, tls_sec_size = 0;
    if (ntlsdesc) {
        off_tls = ALIGN_UP(after_rel, 8);
        tls_sec_size = sizeof(struct ovmx_tls_header) + (uint64_t)ntlsdesc * 8;
    }

    /* .vms$imp: this shareable's OWN cross-image imports (bound to --use
     * producers at activation, like a consumer). Must be in the loaded range so
     * IMGACT can read it by section vaddr. (vms-e65) */
    uint64_t after_tls = ntlsdesc ? off_tls + tls_sec_size : after_rel;
    int nstrong = import_count_strong(imp, nimp);
    int nweak   = import_count_weak(imp, nimp);
    uint64_t off_imp = 0, imp_size = 0;
    if (nstrong) {
        off_imp = ALIGN_UP(after_tls, 8);
        imp_size = vms_imp_size(nimp, imp, ps, np);
    }
    /* .vms$wimp: weak-by-name imports, resolved by IMGACT at activation. Placed
     * right after .vms$imp, also inside the loaded range. (vms-5f0) */
    uint64_t after_imp = nstrong ? off_imp + imp_size : after_tls;
    uint64_t off_wimp = 0, wimp_size = 0;
    if (nweak) {
        off_wimp = ALIGN_UP(after_imp, 8);
        wimp_size = vms_wimp_size(nimp, imp);
    }

    /* .vms$ehf: the DWARF frame-registration descriptor (vms-70d). Emitted only
     * when the image both has a non-empty .eh_frame region AND whole-archived
     * libgcc's __register_frame (a pure-C image has neither the registration
     * machinery nor a need for it). IMGACT reads it and registers the frames
     * before .init_array runs; absent -> IMGACT skips registration. Must be in
     * the loaded range so IMGACT can read it by section vaddr. */
    /* .vms$ehf sits after .vms$wimp (which sits after .vms$imp), so bias it off
     * the end of the weak-import block. after_wimp == after_imp when there are
     * no weak imports. (reconciles vms-70d's ehf placement with vms-5f0's wimp.) */
    uint64_t after_wimp = nweak ? off_wimp + wimp_size : after_imp;
    uint64_t register_frame_va =
        has_eh_frame ? resolve_named(objs, nobj, "__register_frame", NULL) : 0;
    uint64_t off_ehf = 0, ehf_desc_size = 0;
    if (has_eh_frame && register_frame_va && ehf_end > ehf_beg) {
        off_ehf = ALIGN_UP(after_wimp, 8);
        ehf_desc_size = sizeof(struct ovmx_ehf_desc);
    }

    /* End of file-backed loaded content; .bss (NOBITS) extends memsz beyond it. */
    uint64_t file_loaded_end = off_ehf ? off_ehf + ehf_desc_size : after_wimp;
    uint64_t bss_beg = file_loaded_end, bss_end = file_loaded_end;
    if (has_bss) {
        int first = 1;
        for (int i = 0; i < nobj; i++)
            for (int s = 0; s < objs[i].nsh; s++)
                if (objs[i].sec_bucket[s] == B_BSS && objs[i].sh[s].sh_size) {
                    uint64_t al = objs[i].sh[s].sh_addralign ? objs[i].sh[s].sh_addralign : 8;
                    bss_end = ALIGN_UP(bss_end, al);
                    if (first) { bss_beg = bss_end; first = 0; }
                    objs[i].sec_va[s] = bss_end;
                    bss_end += objs[i].sh[s].sh_size;
                }
    }

    uint64_t off_shstr = ALIGN_UP(file_loaded_end, 4);

    const char *secn[26]; int nsec = 0;
    secn[nsec++] = "";
    int ix_text = nsec; secn[nsec++] = ".text";
    int ix_ro = -1;   if (has_ro)   { ix_ro   = nsec; secn[nsec++] = ".rodata"; }
    int ix_ehf_sec = -1; if (ehf_end > ehf_beg) { ix_ehf_sec = nsec; secn[nsec++] = ".eh_frame"; }
    int ix_got = -1;  if (ngot)     { ix_got  = nsec; secn[nsec++] = ".got"; }
    int ix_tlsd = -1; if (ntls)     { ix_tlsd = nsec; secn[nsec++] = ".tlsdesc"; }
    int ix_data = -1; if (has_data) { ix_data = nsec; secn[nsec++] = ".data"; }
    int ix_initarr = -1; if (has_init_array) { ix_initarr = nsec; secn[nsec++] = ".init_array"; }
    int ix_tdata = -1; if (has_tls && tdata_sz) { ix_tdata = nsec; secn[nsec++] = ".tdata"; }
    int ix_igot = -1; if (nimp)     { ix_igot = nsec; secn[nsec++] = ".igot"; }
    int ix_plt = -1;  if (nimp)     { ix_plt  = nsec; secn[nsec++] = ".plt"; }
    int ix_sv = nsec; secn[nsec++] = OVMX_SV_SECTION;
    int ix_rel = -1;  if (nrel)     { ix_rel  = nsec; secn[nsec++] = OVMX_REL_SECTION; }
    int ix_tls = -1;  if (ntlsdesc) { ix_tls  = nsec; secn[nsec++] = OVMX_TLS_SECTION; }
    int ix_imp = -1;  if (nstrong)  { ix_imp  = nsec; secn[nsec++] = OVMX_IMP_SECTION; }
    int ix_wimp = -1; if (nweak)    { ix_wimp = nsec; secn[nsec++] = OVMX_WIMP_SECTION; }
    int ix_ehf = -1;  if (off_ehf)  { ix_ehf  = nsec; secn[nsec++] = OVMX_EHF_SECTION; }
    int ix_bss = -1;  if (has_bss)  { ix_bss  = nsec; secn[nsec++] = ".bss"; }
    int ix_tbss = -1; if (has_tls && tbss_sz) { ix_tbss = nsec; secn[nsec++] = ".tbss"; }
    int ix_str = nsec; secn[nsec++] = ".shstrtab";
    uint64_t sn_off[26]; uint64_t sn_sz = 0;
    for (int i = 0; i < nsec; i++) { sn_off[i] = sn_sz; sn_sz += strlen(secn[i]) + 1; }
    uint64_t off_shdr = ALIGN_UP(off_shstr + sn_sz, 8);
    uint64_t file_sz = off_shdr + (uint64_t)nsec * sizeof(Elf64_Shdr);

    uint8_t *img = calloc(1, file_sz);
    if (!img) die("oom building image");

    Elf64_Ehdr *eh = (Elf64_Ehdr *)img;
    memcpy(eh->e_ident, ELFMAG, SELFMAG);
    eh->e_ident[EI_CLASS] = ELFCLASS64; eh->e_ident[EI_DATA] = ELFDATA2LSB;
    eh->e_ident[EI_VERSION] = EV_CURRENT;
    /* e_machine follows the input object set (vms-8f5) — g_out_machine is set
     * in parse_obj and validated there to be uniform across every input .o. */
    if (g_out_machine != EM_AARCH64 && g_out_machine != EM_X86_64)
        die("internal: no input machine recorded");
    eh->e_type = ET_DYN; eh->e_machine = g_out_machine; eh->e_version = EV_CURRENT;
    eh->e_phoff = off_ph; eh->e_shoff = off_shdr;
    eh->e_ehsize = sizeof *eh; eh->e_phentsize = sizeof(Elf64_Phdr); eh->e_phnum = nph;
    eh->e_shentsize = sizeof(Elf64_Shdr); eh->e_shnum = nsec; eh->e_shstrndx = ix_str;
    /* kernel adds the load bias -> AT_ENTRY. Freestanding _start programs enter
     * at _start directly; main() programs enter at the synthesized crt0. */
    if (is_exec)
        eh->e_entry = exe_start
            ? resolve_named(objs, nobj, "_start", "--executable: _start not placed")
            : crt0_va;

    /* One PT_LOAD. RWX when it carries a writable GOT/TLSDESC/.data/.bss (the
     * activator writes those in place); R+X for a pure leaf/rodata image.
     * A PT_TLS follows when the image has thread-local storage. An executable
     * adds PT_PHDR (so IMGACT derives the load bias) + PT_INTERP=IMGACT.EXE, and
     * its GOT/import cells are written at activation so it is always writable. */
    int writable = (ngot || ntls || has_data || has_init_array || has_bss ||
                    has_tls || nimp || is_exec);
    Elf64_Phdr *ph = (Elf64_Phdr *)(img + off_ph);
    int li;   /* index of the PT_LOAD phdr */
    if (is_exec) {
        ph[0].p_type = PT_PHDR; ph[0].p_flags = PF_R;
        ph[0].p_offset = off_ph; ph[0].p_vaddr = off_ph; ph[0].p_paddr = off_ph;
        ph[0].p_filesz = (uint64_t)nph * sizeof(Elf64_Phdr);
        ph[0].p_memsz  = ph[0].p_filesz; ph[0].p_align = 8;
        ph[1].p_type = PT_INTERP; ph[1].p_flags = PF_R;
        ph[1].p_offset = off_interp; ph[1].p_vaddr = off_interp; ph[1].p_paddr = off_interp;
        ph[1].p_filesz = interp_sz; ph[1].p_memsz = interp_sz; ph[1].p_align = 1;
        li = 2;
    } else {
        li = 0;
    }
    ph[li].p_type = PT_LOAD;
    ph[li].p_flags = writable ? (PF_R | PF_W | PF_X) : (PF_R | PF_X);
    ph[li].p_filesz = file_loaded_end; ph[li].p_memsz = bss_end; ph[li].p_align = PAGE;
    if (has_tls) {
        int ti = li + 1;
        ph[ti].p_type = PT_TLS; ph[ti].p_flags = PF_R;
        ph[ti].p_offset = tdata_va; ph[ti].p_vaddr = tdata_va; ph[ti].p_paddr = tdata_va;
        ph[ti].p_filesz = tdata_sz; ph[ti].p_memsz = tls_memsz; ph[ti].p_align = tls_align;
    }
    if (is_exec) memcpy(img + off_interp, IMGACT_INTERP, interp_sz);

    /* Copy each placed PROGBITS section (text/rodata/data/init_array) to its
     * vaddr. SHT_INIT_ARRAY's file-resident bytes are the pre-relocation
     * addends; the ABS64 reloc-apply loop below overwrites each entry with
     * the real placed pointer value. */
    for (int i = 0; i < nobj; i++)
        for (int s = 0; s < objs[i].nsh; s++) {
            int b = objs[i].sec_bucket[s];
            if ((b == B_TEXT || b == B_RODATA || b == B_DATA || b == B_INIT_ARRAY ||
                 b == B_EH_FRAME) &&
                objs[i].sh[s].sh_size)
                memcpy(img + objs[i].sec_va[s],
                       objs[i].buf + objs[i].sh[s].sh_offset, objs[i].sh[s].sh_size);
        }

    /* Copy the combined TLS init image: every .tdata / .tdata.* section into its
     * assigned slot within the block's [tdata_va, tdata_va+tdata_sz) init region
     * (vms-da2). sec_va[s] holds the section's block-relative base. .tbss is
     * zero-filled per thread (already zero in the calloc'd image, and re-zeroed
     * by the activator's anonymous TLS mapping). */
    if (has_tls)
        for (int i = 0; i < nobj; i++)
            for (int s = 0; s < objs[i].nsh; s++)
                if (objs[i].sec_bucket[s] == B_TDATA && objs[i].sh[s].sh_size)
                    memcpy(img + tdata_va + objs[i].sec_va[s],
                           objs[i].buf + objs[i].sh[s].sh_offset,
                           objs[i].sh[s].sh_size);

    /* Image-relative slots (GOT cells + ABS64 data pointers) to bias at
     * activation; filled as they resolve, header count set at the end. */
    uint64_t *rel_off = nrel ? calloc((size_t)nrel, 8) : NULL;
    int nrel_filled = 0;

    /* Fill GOT cells with image-relative target addresses. A GOT symbol not
     * defined by any input object is a deferred import (vms-61f): under
     * --allow-undefined its cell stays 0 and is NOT recorded in .vms$rel. */
    for (int i = 0; i < ngot; i++) {
        if (got[i].is_local) {
            /* Defined LOCAL target: resolve directly to its placed vaddr (same
             * mechanism resolve_ref uses for non-GOT local relocations). A
             * defined local always lands in a flat-placed bucket (text/rodata/
             * data/bss), so placed_addr is nonzero; a 0 here means the local
             * lives in a section this linker does not place (e.g. TLS/ABS) — a
             * reloc shape LINK.EXE does not model for GOT locals. (vms-9c1) */
            got[i].value = placed_addr(&objs[got[i].oi],
                                       &objs[got[i].oi].sym[got[i].sym]);
            if (!got[i].value)
                die("GOT reference to a LOCAL symbol in an unplaced section "
                    "(TLS/ABS local GOT slots are unsupported)");
            *(uint64_t *)(img + got[i].va) = got[i].value;
            rel_off[nrel_filled++] = got[i].va;   /* image-relative -> bias at activation */
            continue;
        }
        got[i].value = resolve_named(objs, nobj, got[i].name, NULL);
        if (got[i].value) {
            *(uint64_t *)(img + got[i].va) = got[i].value;
            rel_off[nrel_filled++] = got[i].va;   /* image-relative -> bias at activation */
            continue;
        }
        /* A producer globalvalue address-taken via the GOT (e.g. `&C$_EXIT1` if
         * the port's codegen routes it GOT-indirect): fill the cell with the
         * ABSOLUTE constant and do NOT record it in .vms$rel — it is not an
         * image-relative address, so it must not be load-biased. (vms-954) */
        {
            uint64_t gv;
            if (gval_find(got[i].name, &gv)) {
                got[i].value = gv;
                *(uint64_t *)(img + got[i].va) = gv;
                continue;
            }
        }
        /* Undefined GOT symbol. A WEAK reference (no definition anywhere) is a
         * legitimate address-0 resolution — the linker-defined empty
         * __init_array/__fini_array bounds and null _DYNAMIC of a C-RTL with no
         * static constructors. Otherwise it is a deferred import
         * (--allow-undefined) or, strict, a hard error. (vms-61f.1) */
        *(uint64_t *)(img + got[i].va) = 0;   /* cell = 0, NOT biased/recorded */
        if (weak_has(got[i].name))      { /* correct 0; nothing to defer */ }
        else if (g_allow_undef)         { g_deferred++; }
        else die("GOT symbol undefined (cross-image DATA import is a later increment)");
    }

    /* Fill TLSDESC entries: [0]=0 (IMGACT sets the resolver), [1]=module offset. */
    for (int i = 0; i < ntls; i++) {
        tls[i].modoff = tls_module_offset(objs, nobj,
                                          tls[i].name, tls[i].addend);
        uint64_t *e = (uint64_t *)(img + tls[i].va);
        e[0] = 0;
        e[1] = tls[i].modoff;
    }

    /* Apply relocations across every code section: GOT-indirect pairs -> GOT
     * slot; TLSDESC -> TLSDESC entry; the rest PC-relative (with addend). */
    /* Aligned size of the combined TLS block. In x86_64 Variant II the block
     * base sits at TP - tls_tp_size, so a variable at combined-block offset moff
     * is at TP-relative offset (moff - tls_tp_size) — the Local-Exec offset the
     * GD/LD relaxation embeds, matching IMGACT assign_tls_offsets(). (vms-76a) */
    uint64_t tls_tp_size = has_tls ? ALIGN_UP(tls_memsz, tls_align) : 0;
    for (int i = 0; i < nobj; i++) {
        /* Per-object TLS dialect: an object carrying any classic GD/LD reloc was
         * compiled without -mtls-dialect=gnu2, so after LD->LE relaxation its
         * %rax holds TP (not the module base the gnu2/TLSDESC path leaves), and
         * its paired R_X86_64_DTPOFF32 operands must be TP-relative rather than
         * module-relative. A single object uses one dialect throughout. */
        int classic_tls = 0;
        for (int r = 0; r < objs[i].nreloc; r++)
            if (is_classic_gdld_reloc(ELF64_R_TYPE(objs[i].relocs[r].info))) {
                classic_tls = 1;
                break;
            }
        for (int r = 0; r < objs[i].nreloc; r++) {
            struct reloc *rl = &objs[i].relocs[r];
            uint32_t type = ELF64_R_TYPE(rl->info);
            uint64_t site = objs[i].sec_va[rl->sec] + rl->off;
            uint32_t *insn = (uint32_t *)(img + site);
            const char *nm = objs[i].str +
                             objs[i].sym[ELF64_R_SYM(rl->info)].st_name;
            /* The classic GD/LD call to __tls_get_addr is subsumed by the LE
             * relaxation of its paired lea; its site bytes were overwritten by
             * patch_tls_le(), so never patch it separately. (vms-76a) */
            if (strcmp(nm, "__tls_get_addr") == 0) continue;
            if (is_got_reloc(type)) {
                uint32_t si = ELF64_R_SYM(rl->info);
                if (ELF64_ST_BIND(objs[i].sym[si].st_info) == STB_LOCAL) {
                    /* LOCAL GOT reference -> its per-object (oi, sym) slot. */
                    int gi = find_got_local(got, ngot, i, (int)si);
                    if (gi < 0) {
                        fprintf(stderr, "%%LINK-F-ERROR, internal: local GOT slot "
                                "missing for symbol '%s' (reloc type %u)\n", nm, type);
                        exit(1);
                    }
                    patch_got(type, insn, site, got[gi].va, rl->add);
                } else {
                    int ii = import_find(imp, nimp, nm);
                    if (ii >= 0) {
                        /* Cross-image DATA import: read its import-GOT cell. */
                        patch_got(type, insn, site, imp[ii].got_va, rl->add);
                    } else {
                        int gi = find_got(got, ngot, nm);
                        if (gi < 0) {
                            fprintf(stderr, "%%LINK-F-ERROR, internal: GOT slot "
                                    "missing for symbol '%s' (reloc type %u)\n", nm, type);
                            exit(1);
                        }
                        patch_got(type, insn, site, got[gi].va, rl->add);
                    }
                }
            } else if (is_tlsdesc_reloc(type)) {
                int ti = find_tls(tls, ntls, nm);
                if (ti < 0) die("internal: TLSDESC slot missing for symbol");
                patch_tlsdesc(type, insn, site, tls[ti].va, rl->add);
            } else if (is_classic_gdld_reloc(type)) {
                /* Classic GD/LD -> Local-Exec relaxation (vms-76a). Rewrite the
                 * psABI-fixed lea+call window to read TP and (for GD) add the
                 * variable's TP-relative offset in place. For LD the offsets ride
                 * the paired DTPOFF32 operands, so tpoff here is unused. */
                uint64_t moff = tls_ref_offset(objs, nobj, i,
                                               ELF64_R_SYM(rl->info), 0);
                int32_t tpoff = (int32_t)(int64_t)(moff - tls_tp_size);
                patch_tls_le(type, (uint8_t *)img, site, tpoff);
            } else if (is_dtpoff_reloc(type)) {
                /* x86_64 dynamic TLS operand: the variable's TLS offset written
                 * as a flat absolute 32-bit constant, link-time-final — NOT
                 * load-biased, NOT in .vms$rel. In a gnu2/TLSDESC object this is
                 * MODULE-relative (added at run time to the module base the
                 * TLSDESC pair resolves). In a classic object whose TLSLD was
                 * relaxed to LE, %rax already holds TP, so the operand must be
                 * TP-relative (moff - aligned block size). (vms-76a) */
                uint64_t moff = tls_ref_offset(objs, nobj, i,
                                               ELF64_R_SYM(rl->info),
                                               rl->add);
                *insn = classic_tls ? (uint32_t)(moff - tls_tp_size)
                                    : (uint32_t)moff;
            } else if (is_abs64_reloc(type)) {
                /* Pointer initializer (.rela.data): write S+A as a 64-bit
                 * image-relative address and record the slot in .vms$rel so
                 * the activator adds the load bias. A deferred external leaves
                 * the slot 0 (unbiased). (vms-004, folds in vms-a17) */
                uint64_t s = resolve_ref(objs, nobj, i, ELF64_R_SYM(rl->info));
                if (s == 0) continue;   /* deferred (counted in resolve_ref) */
                uint64_t value = s + (uint64_t)rl->add;
                *(uint64_t *)(img + site) = value;
                /* A producer globalvalue is an ABSOLUTE constant (VMS
                 * globalvalue): write it, but do NOT record the slot in
                 * .vms$rel — biasing it at activation would corrupt the
                 * constant (e.g. C$_EXIT1 = 0x0035A009). (vms-954) */
                if (gval_find(nm, NULL)) continue;
                rel_off[nrel_filled++] = site;
            } else {
                int ii = import_find(imp, nimp, nm);
                if (ii >= 0 &&
                    (type == R_AARCH64_CALL26 || type == R_AARCH64_JUMP26)) {
                    /* Cross-image CALL/JUMP import: branch to its PLT stub. */
                    int64_t disp = (int64_t)imp[ii].plt_va - (int64_t)site;
                    uint32_t imm26 = (uint32_t)((disp >> 2) & 0x03FFFFFF);
                    uint32_t op = (type == R_AARCH64_JUMP26) ? 0x14000000u
                                                             : 0x94000000u;
                    *insn = op | imm26;
                } else if (ii >= 0 && type == R_X86_64_PLT32) {
                    /* Cross-image CALL import (vms-206): a PC32-style call to
                     * the PLT stub instead of the (nonexistent, in this
                     * object set) callee -- same S+A-P shape the intra-image
                     * PC32/PLT32 datum write below uses, just with S = the
                     * stub's address. rl->add carries the real x86_64 addend
                     * (typically -4, same as the GOTPCREL/PC32 cases above). */
                    int64_t d = (int64_t)imp[ii].plt_va + rl->add - (int64_t)site;
                    *insn = (uint32_t)(uint64_t)d;
                } else {
                    uint64_t target =
                        resolve_ref(objs, nobj, i, ELF64_R_SYM(rl->info));
                    if (target == 0) continue;  /* deferred external, skip patch */
                    target += (uint64_t)rl->add;
                    patch_pcrel(type, insn, site, target);
                }
            }
        }
    }

    /* PLT stubs for call imports: aarch64 = adrp x16,cell ; ldr x16,[x16,#lo12] ;
     * br x16 (page+lo12 split GOT load then indirect branch). x86_64 = a single
     * `jmp *disp32(%rip)` (FF 25 imm32): RIP-relative addressing reaches the
     * whole address space in one instruction, so no page/lo12 split is needed --
     * the memory operand IS the import-GOT cell, read indirectly exactly like
     * the aarch64 stub's ldr does. Both stubs jump through the SAME import-GOT
     * cell (imp[i].got_va), left 0 here; IMGACT fills it with the producer's
     * resolved address from .vms$imp at activation. (Data-import slots stay
     * unused — the site reads the import cell directly.) (vms-e65, vms-206) */
    for (int i = 0; i < nimp; i++) {
        if (imp[i].is_data) continue;
        if (g_out_machine == EM_X86_64) {
            uint8_t *stub = img + imp[i].plt_va;
            stub[0] = 0xFFu; stub[1] = 0x25u;   /* jmp *disp32(%rip) */
            int32_t d = (int32_t)((int64_t)imp[i].got_va -
                                  (int64_t)(imp[i].plt_va + 6));
            memcpy(stub + 2, &d, 4);
        } else {
            uint32_t *stub = (uint32_t *)(img + imp[i].plt_va);
            int64_t pd = (int64_t)(imp[i].got_va >> 12) - (int64_t)(imp[i].plt_va >> 12);
            stub[0] = enc_adrp(16, pd);
            stub[1] = enc_ldr_u64(16, 16, (uint32_t)(imp[i].got_va & 0xfff));
            stub[2] = enc_br(16);
        }
    }

    /* Synthesized crt0 (executable entry). The initial process stack the kernel
     * built for the PT_INTERP'd program (argc, argv[], NULL, envp[], NULL, auxv)
     * is intact at SP on entry — IMGACT's _start hands SP to imgact_bootstrap via
     * a bl/ret pair that leaves SP unchanged, then branches to e_entry with GP
     * regs cleared, exactly as the kernel enters a _start. musl's __init_libc has
     * already run (IMGACT drove it from DECC$SHR before transfer), so the C-RTL
     * is live. crt0 recovers argc/argv/envp and calls main(argc,argv,envp), then
     * tail-calls exit(ret) to flush stdio and terminate. (vms-ba1) */
    if (use_crt0) {
        uint64_t main_va = resolve_named(objs, nobj, "main",
            "--executable object set defines no main()");
        if (exit_imp < 0) die("internal: exit import missing for crt0");
        if (g_out_machine == EM_X86_64) {
            /* x86_64 crt0 (vms-206): same contract as the aarch64 stub below --
             * recover argc/argv/envp off the initial process stack per the
             * SysV entry layout ([rsp]=argc, rsp+8..=argv[], NULL, envp[],
             * NULL, auxv) and call main(argc,argv,envp) in the SysV integer
             * arg registers (rdi,rsi,rdx), then tail the return value (eax)
             * into exit()'s argument (edi). No pushes happen before `call
             * main`, so RSP stays exactly as the kernel/IMGACT left it --
             * 16-byte aligned per the ABI's call-site rule, matching what a
             * real _start entry requires. */
            uint8_t *c = img + crt0_va;
            memcpy(c +  0, "\x48\x8B\x3C\x24", 4);     /* mov rdi,[rsp]          ; argc */
            memcpy(c +  4, "\x48\x8D\x74\x24\x08", 5); /* lea rsi,[rsp+8]        ; argv */
            memcpy(c +  9, "\x48\x8D\x54\xFE\x08", 5); /* lea rdx,[rsi+rdi*8+8]  ; envp */
            c[14] = 0xE8;                              /* call main (rel32)          */
            int32_t dmain = (int32_t)((int64_t)main_va - (int64_t)(crt0_va + 19));
            memcpy(c + 15, &dmain, 4);
            memcpy(c + 19, "\x89\xC7", 2);              /* mov edi,eax  ; exit code   */
            c[21] = 0xE8;                               /* call exit (rel32)          */
            int32_t dexit = (int32_t)((int64_t)imp[exit_imp].plt_va -
                                      (int64_t)(crt0_va + 26));
            memcpy(c + 22, &dexit, 4);
            memcpy(c + 26, "\x0F\x0B", 2);               /* ud2 ; exit never returns  */
        } else {
            uint32_t *c = (uint32_t *)(img + crt0_va);
            c[0] = 0xF94003E0u;   /* ldr  x0, [sp]              ; x0 = argc          */
            c[1] = 0x910023E1u;   /* add  x1, sp, #8            ; x1 = argv          */
            c[2] = 0x8B000C22u;   /* add  x2, x1, x0, lsl #3    ; &argv[argc] (=NULL) */
            c[3] = 0x91002042u;   /* add  x2, x2, #8            ; x2 = envp          */
            int64_t dmain = (int64_t)main_va - (int64_t)(crt0_va + 16);
            c[4] = 0x94000000u | (uint32_t)((dmain >> 2) & 0x03FFFFFF);  /* bl main   */
            int64_t dexit = (int64_t)imp[exit_imp].plt_va - (int64_t)(crt0_va + 20);
            c[5] = 0x94000000u | (uint32_t)((dexit >> 2) & 0x03FFFFFF);  /* bl exit   */
            c[6] = 0xD4200000u;   /* brk #0                     ; exit never returns */
        }
    }

    struct ovmx_sv_header *svh = (struct ovmx_sv_header *)(img + off_sv);
    svh->magic = OVMX_SV_MAGIC; svh->count = nuniv;
    svh->gsmatch_kind = gk; svh->gsmatch_major = gmaj; svh->gsmatch_minor = gmin;
    svh->names_off = (uint32_t)sv_names_o; svh->names_size = names_size;
    struct ovmx_sv_entry *sve = (struct ovmx_sv_entry *)(img + off_sv + sv_hdr_sz);
    char *nblob = (char *)(img + off_sv + sv_names_o); uint32_t noff = 0;
    for (int i = 0; i < nuniv; i++) {
        sve[i].value = uv[i].value; sve[i].kind = uv[i].kind; sve[i].name_off = noff;
        size_t l = strlen(uv[i].name) + 1; memcpy(nblob + noff, uv[i].name, l);
        noff += (uint32_t)l;
    }

    if (nrel) {
        struct ovmx_rel_header *rh = (struct ovmx_rel_header *)(img + off_rel);
        rh->magic = OVMX_REL_MAGIC; rh->count = (uint32_t)nrel_filled;
        uint64_t *off = (uint64_t *)(img + off_rel + sizeof *rh);
        for (int i = 0; i < nrel_filled; i++) off[i] = rel_off[i];
    }

    if (ntlsdesc) {
        struct ovmx_tls_header *th = (struct ovmx_tls_header *)(img + off_tls);
        th->magic = OVMX_TLS_MAGIC; th->count = (uint32_t)ntlsdesc;
        uint64_t *eo = (uint64_t *)(img + off_tls + sizeof *th);
        for (int i = 0; i < ntls; i++) eo[i] = tls[i].va;
    }

    /* .vms$imp: this shareable's own cross-image imports (patch_off = import-GOT
     * cell). IMGACT binds each to its --use producer at activation. (vms-e65) */
    if (nstrong)
        vms_imp_write(img, off_imp, imp, nimp, ps, np);
    /* .vms$wimp: weak-by-name imports; IMGACT binds each by NAME at activation
     * against the loaded producer set (absent -> cell stays 0). (vms-5f0) */
    if (nweak)
        vms_wimp_write(img, off_wimp, imp, nimp);

    /* .vms$ehf descriptor: image-relative .eh_frame start + __register_frame
     * addr (both biased by IMGACT). Only emitted when register_frame_va != 0. */
    if (off_ehf) {
        struct ovmx_ehf_desc *ed = (struct ovmx_ehf_desc *)(img + off_ehf);
        ed->magic = OVMX_EHF_MAGIC;
        ed->reserved = 0;
        ed->eh_frame_begin = ehf_beg;
        ed->register_frame = register_frame_va;
    }

    char *shstr = (char *)(img + off_shstr);
    for (int i = 0; i < nsec; i++) memcpy(shstr + sn_off[i], secn[i], strlen(secn[i]) + 1);
    Elf64_Shdr *sh = (Elf64_Shdr *)(img + off_shdr);
    sh[ix_text].sh_name = sn_off[ix_text]; sh[ix_text].sh_type = SHT_PROGBITS;
    sh[ix_text].sh_flags = SHF_ALLOC | SHF_EXECINSTR; sh[ix_text].sh_addr = text_beg;
    sh[ix_text].sh_offset = text_beg; sh[ix_text].sh_size = text_end - text_beg;
    sh[ix_text].sh_addralign = 16;
    if (has_ro) {
        sh[ix_ro].sh_name = sn_off[ix_ro]; sh[ix_ro].sh_type = SHT_PROGBITS;
        sh[ix_ro].sh_flags = SHF_ALLOC; sh[ix_ro].sh_addr = ro_beg;
        sh[ix_ro].sh_offset = ro_beg; sh[ix_ro].sh_size = ro_end - ro_beg;
        sh[ix_ro].sh_addralign = 16;
    }
    if (ix_ehf_sec >= 0) {
        /* .eh_frame output section spans the whole contiguous block INCLUDING
         * the 4-byte-zero terminator (ehf_end), so a reader/registrar that
         * bounds by sh_size sees the terminated FDE list. */
        sh[ix_ehf_sec].sh_name = sn_off[ix_ehf_sec]; sh[ix_ehf_sec].sh_type = SHT_PROGBITS;
        sh[ix_ehf_sec].sh_flags = SHF_ALLOC; sh[ix_ehf_sec].sh_addr = ehf_beg;
        sh[ix_ehf_sec].sh_offset = ehf_beg; sh[ix_ehf_sec].sh_size = ehf_end - ehf_beg;
        sh[ix_ehf_sec].sh_addralign = 8;
    }
    if (ngot) {
        sh[ix_got].sh_name = sn_off[ix_got]; sh[ix_got].sh_type = SHT_PROGBITS;
        sh[ix_got].sh_flags = SHF_ALLOC | SHF_WRITE; sh[ix_got].sh_addr = got_beg;
        sh[ix_got].sh_offset = got_beg; sh[ix_got].sh_size = got_end - got_beg;
        sh[ix_got].sh_addralign = 8;
    }
    if (ntls) {
        sh[ix_tlsd].sh_name = sn_off[ix_tlsd]; sh[ix_tlsd].sh_type = SHT_PROGBITS;
        sh[ix_tlsd].sh_flags = SHF_ALLOC | SHF_WRITE; sh[ix_tlsd].sh_addr = tlsdesc_beg;
        sh[ix_tlsd].sh_offset = tlsdesc_beg; sh[ix_tlsd].sh_size = tlsdesc_end - tlsdesc_beg;
        sh[ix_tlsd].sh_addralign = 8;
    }
    if (has_data) {
        sh[ix_data].sh_name = sn_off[ix_data]; sh[ix_data].sh_type = SHT_PROGBITS;
        sh[ix_data].sh_flags = SHF_ALLOC | SHF_WRITE; sh[ix_data].sh_addr = data_beg;
        sh[ix_data].sh_offset = data_beg; sh[ix_data].sh_size = data_end - data_beg;
        sh[ix_data].sh_addralign = 8;
    }
    if (has_init_array) {
        /* Real SHT_INIT_ARRAY output section: preserves the type so a reader
         * (readelf, IMGACT's ovmx_find_section by name) sees exactly what it
         * is -- the placed ctor-pointer range, not generic writable data. */
        sh[ix_initarr].sh_name = sn_off[ix_initarr]; sh[ix_initarr].sh_type = SHT_INIT_ARRAY;
        sh[ix_initarr].sh_flags = SHF_ALLOC | SHF_WRITE; sh[ix_initarr].sh_addr = initarr_beg;
        sh[ix_initarr].sh_offset = initarr_beg; sh[ix_initarr].sh_size = initarr_end - initarr_beg;
        sh[ix_initarr].sh_addralign = 8;
    }
    if (has_tls && tdata_sz) {
        sh[ix_tdata].sh_name = sn_off[ix_tdata]; sh[ix_tdata].sh_type = SHT_PROGBITS;
        sh[ix_tdata].sh_flags = SHF_ALLOC | SHF_WRITE | SHF_TLS;
        sh[ix_tdata].sh_addr = tdata_va; sh[ix_tdata].sh_offset = tdata_va;
        sh[ix_tdata].sh_size = tdata_sz; sh[ix_tdata].sh_addralign = tls_align;
    }
    sh[ix_sv].sh_name = sn_off[ix_sv]; sh[ix_sv].sh_type = SHT_PROGBITS;
    sh[ix_sv].sh_flags = SHF_ALLOC; sh[ix_sv].sh_addr = off_sv;
    sh[ix_sv].sh_offset = off_sv; sh[ix_sv].sh_size = sv_size; sh[ix_sv].sh_addralign = 8;
    if (nrel) {
        sh[ix_rel].sh_name = sn_off[ix_rel]; sh[ix_rel].sh_type = SHT_PROGBITS;
        sh[ix_rel].sh_flags = SHF_ALLOC; sh[ix_rel].sh_addr = off_rel;
        sh[ix_rel].sh_offset = off_rel; sh[ix_rel].sh_size = rel_size;
        sh[ix_rel].sh_addralign = 8;
    }
    if (ntlsdesc) {
        sh[ix_tls].sh_name = sn_off[ix_tls]; sh[ix_tls].sh_type = SHT_PROGBITS;
        sh[ix_tls].sh_flags = SHF_ALLOC; sh[ix_tls].sh_addr = off_tls;
        sh[ix_tls].sh_offset = off_tls; sh[ix_tls].sh_size = tls_sec_size;
        sh[ix_tls].sh_addralign = 8;
    }
    if (nimp) {
        sh[ix_igot].sh_name = sn_off[ix_igot]; sh[ix_igot].sh_type = SHT_PROGBITS;
        sh[ix_igot].sh_flags = SHF_ALLOC | SHF_WRITE; sh[ix_igot].sh_addr = impgot_beg;
        sh[ix_igot].sh_offset = impgot_beg; sh[ix_igot].sh_size = impgot_end - impgot_beg;
        sh[ix_igot].sh_addralign = 8;
        sh[ix_plt].sh_name = sn_off[ix_plt]; sh[ix_plt].sh_type = SHT_PROGBITS;
        sh[ix_plt].sh_flags = SHF_ALLOC | SHF_EXECINSTR; sh[ix_plt].sh_addr = plt_beg;
        sh[ix_plt].sh_offset = plt_beg; sh[ix_plt].sh_size = plt_end - plt_beg;
        sh[ix_plt].sh_addralign = 4;
    }
    if (nstrong) {
        sh[ix_imp].sh_name = sn_off[ix_imp]; sh[ix_imp].sh_type = SHT_PROGBITS;
        sh[ix_imp].sh_flags = SHF_ALLOC; sh[ix_imp].sh_addr = off_imp;
        sh[ix_imp].sh_offset = off_imp; sh[ix_imp].sh_size = imp_size;
        sh[ix_imp].sh_addralign = 8;
    }
    if (nweak) {
        sh[ix_wimp].sh_name = sn_off[ix_wimp]; sh[ix_wimp].sh_type = SHT_PROGBITS;
        sh[ix_wimp].sh_flags = SHF_ALLOC; sh[ix_wimp].sh_addr = off_wimp;
        sh[ix_wimp].sh_offset = off_wimp; sh[ix_wimp].sh_size = wimp_size;
        sh[ix_wimp].sh_addralign = 8;
    }
    if (ix_ehf >= 0) {
        sh[ix_ehf].sh_name = sn_off[ix_ehf]; sh[ix_ehf].sh_type = SHT_PROGBITS;
        sh[ix_ehf].sh_flags = SHF_ALLOC; sh[ix_ehf].sh_addr = off_ehf;
        sh[ix_ehf].sh_offset = off_ehf; sh[ix_ehf].sh_size = ehf_desc_size;
        sh[ix_ehf].sh_addralign = 8;
    }
    if (has_tls && tbss_sz) {
        sh[ix_tbss].sh_name = sn_off[ix_tbss]; sh[ix_tbss].sh_type = SHT_NOBITS;
        sh[ix_tbss].sh_flags = SHF_ALLOC | SHF_WRITE | SHF_TLS;
        sh[ix_tbss].sh_addr = tdata_va + tdata_sz; sh[ix_tbss].sh_offset = tdata_end;
        sh[ix_tbss].sh_size = tbss_sz; sh[ix_tbss].sh_addralign = tls_align;
    }
    if (has_bss) {
        sh[ix_bss].sh_name = sn_off[ix_bss]; sh[ix_bss].sh_type = SHT_NOBITS;
        sh[ix_bss].sh_flags = SHF_ALLOC | SHF_WRITE; sh[ix_bss].sh_addr = bss_beg;
        sh[ix_bss].sh_offset = file_loaded_end; sh[ix_bss].sh_size = bss_end - bss_beg;
        sh[ix_bss].sh_addralign = 8;
    }
    sh[ix_str].sh_name = sn_off[ix_str]; sh[ix_str].sh_type = SHT_STRTAB;
    sh[ix_str].sh_offset = off_shstr; sh[ix_str].sh_size = sn_sz; sh[ix_str].sh_addralign = 1;

#ifdef OVMX_RMS_IO
    /* vms-b5a: route the output-image write through RMS (sys$create/$put/$close).
     * Byte-exact (mrs=0, per-put length) — see ovmx_link_rms_io.h. sys$create
     * mints a VMS version suffix, so the image lands on disk as "<out>;1". */
    if (ovmx_link_rms_write(out, img, file_sz) != 0)
        die("cannot create output image (RMS)");
#else
    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) die("cannot create output image");
    if (write(fd, img, file_sz) != (ssize_t)file_sz) die("short write output");
    close(fd);
#endif
    int totrel = 0; for (int i = 0; i < nobj; i++) totrel += objs[i].nreloc;
    /* ABS64 data pointers written = filled .vms$rel slots minus the resolved
     * GOT cells (GOT slots are pushed into rel_off first). */
    int abs_applied = nrel_filled;
    for (int i = 0; i < ngot; i++) if (got[i].value) abs_applied--;
    fprintf(stderr,
        "%%LINK-S-CREATED, %s: ET_DYN %s image, %d object%s, %d universal%s, "
        "%d reloc%s, %d GOT, %d TLS, %d ABS64-ptr, %d import%s, GSMATCH=%s,%u,%u\n",
        out, is_exec ? "executable" : "shareable",
        nobj, nobj==1?"":"s", nuniv, nuniv==1?"":"s", totrel, totrel==1?"":"s",
        ngot, ntls, abs_applied, nimp, nimp==1?"":"s",
        gk == OVMX_GSMATCH_ALWAYS ? "ALWAYS" :
        gk == OVMX_GSMATCH_EQUAL  ? "EQUAL"  : "LEQUAL", gmaj, gmin);
    if (nstrong)
        fprintf(stderr, "%%LINK-I-IMPORT, %d cross-image import%s bound to --use "
                "producer%s (%d PLT stub%s + import GOT; resolved at activation "
                "via .vms$imp)\n",
                nstrong, nstrong==1?"":"s", np==1?"":"s",
                nstrong, nstrong==1?"":"s");
    if (nweak)
        fprintf(stderr, "%%LINK-I-WEAKIMP, %d weak-by-name import%s "
                "(resolved by NAME against the loaded producer set at activation "
                "via .vms$wimp; absent -> 0, ELF weak-undef)\n",
                nweak, nweak==1?"":"s");
    if (g_deferred)
        fprintf(stderr, "%%LINK-I-DEFEXT, %ld external reference%s left unresolved "
                "(deferred imports — satisfied by the C RTL / a companion "
                "shareable at activation, vms-61f)\n",
                g_deferred, g_deferred == 1 ? "" : "s");
    free(rel_off); free(got); free(tls); free(imp); free(g_syms); g_syms = NULL;
    free(img);
}


/* Peek an input file's first bytes to tell an `ar` archive from a bare .o. */
static int file_is_archive(const char *path)
{
#ifdef OVMX_RMS_IO
    /* vms-b5a: sniff the magic through RMS (open/get/close) — same input path
     * as slurp(), just the first AR_MAGIC_LEN bytes. */
    char m[AR_MAGIC_LEN];
    int r = ovmx_link_rms_peek(path, m, AR_MAGIC_LEN);
    if (r < 0) die("cannot open input file (RMS)");
    return r == AR_MAGIC_LEN && memcmp(m, AR_MAGIC, AR_MAGIC_LEN) == 0;
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("cannot open input file");
    char m[AR_MAGIC_LEN];
    ssize_t r = read(fd, m, AR_MAGIC_LEN);
    close(fd);
    return r == AR_MAGIC_LEN && memcmp(m, AR_MAGIC, AR_MAGIC_LEN) == 0;
#endif
}

/* ==========================================================================
 * EVAX (Alpha/VMS) object -> VMS-standard image path (bead vms-cbe, slices 3+4)
 * ==========================================================================
 *
 * The OpenVMS GCC port (alpha-dec-vms) emits native VMS "EVAX" objects, not
 * ELF. src/vmslink/evax_read.{c,h} is the clean-room front end that parses one
 * into a struct evax_object (psects with materialized content, symbols with a
 * procedure descriptor value AND a code entry, and a flat relocation list).
 * This block maps that into a laid-out image, resolves the symbols, APPLIES the
 * relocations, and stamps the .vms$xfer transfer vector.
 *
 * WHY A DEDICATED PATH, NOT emit_shareable(): the ELF emitter above is
 * arch-locked to aarch64/x86_64 — its GOT/TLSDESC/PLT synthesis, crt0 stub, and
 * every reloc apply switch on the aarch64 / x86_64 R_* type numbers, and it
 * reads Elf64_* structures directly out of the input buffer. Alpha has a different
 * relocation and linkage model (the 2-quadword linkage pair, GP-relative calls,
 * procedure descriptors). Feeding EVAX through the ELF machinery would mean
 * synthesizing fake Elf64 structures and then still not having any Alpha reloc
 * apply — i.e. it would fake structure without faking correctness, the exact
 * anti-pattern the authenticity invariants forbid. So the EVAX object gets its
 * own honest, self-contained emit. The ELF path is untouched (no regression).
 * (Judgment call — flagged to the conductor for the co-design review.)
 *
 * FIRST-LIGHT SCOPE (what is fully applied vs conservative) — see each reloc.
 */

/* .vms$xfer transfer-vector section — the co-design contract with IMGACT.
 * mirrors ovmx_image.h from #720; switch to the include once it lands on main. */
#ifndef OVMX_XFER_SECTION
#define OVMX_XFER_SECTION ".vms$xfer"
#define OVMX_XFER_MAGIC   0x31465358u  /* 'XSF1' little-endian */
#define OVMX_ACT_VMS_STD  1u
struct ovmx_xfer_header {
    uint32_t magic;     /* OVMX_XFER_MAGIC */
    uint32_t flavor;    /* OVMX_ACT_VMS_STD = 1 */
    uint32_t count;     /* >= 1 */
    uint32_t reserved;  /* 0 */
};
#endif

#ifndef EM_ALPHA
#define EM_ALPHA 0x9026
#endif

/* One EVAX input object plus the image vaddr assigned to each of its psects. */
struct evax_input {
    struct evax_object obj;
    const char *name;
    uint64_t    sec_base[EVAX_MAX_SECTIONS];  /* placed image vaddr per psect */
};

static void putl32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
static void putl64(uint8_t *p, uint64_t v) { putl32(p, (uint32_t)v); putl32(p + 4, (uint32_t)(v >> 32)); }

/* Placement rank: $CODE$ | $DATA$ | $LINK$ | other-loaded | $BSS$ (nobits, last).
 * Psects of the same name are merged across objects into one output section,
 * exactly as the ELF path buckets sections. */
static int evax_rank(const char *n)
{
    if (!strcmp(n, "$CODE$")) return 0;
    if (!strcmp(n, "$DATA$")) return 1;
    if (!strcmp(n, "$LINK$")) return 2;
    if (!strcmp(n, "$BSS$"))  return 4;
    return 3;
}
static int evax_is_nobits(const char *n) { return !strcmp(n, "$BSS$"); }

/* Resolve a symbol NAME across all inputs to its defining symbol. Returns the
 * defining input index + symbol, or -1 if undefined. */
static int evax_find_sym(struct evax_input *in, int nin, const char *name,
                         int *out_i, const struct evax_symbol **out_s)
{
    for (int i = 0; i < nin; i++)
        for (int s = 0; s < in[i].obj.nsym; s++)
            if (in[i].obj.sym[s].defined && strcmp(in[i].obj.sym[s].name, name) == 0) {
                *out_i = i; *out_s = &in[i].obj.sym[s]; return 0;
            }
    return -1;
}

/* Resolved address of a symbol's VALUE (procedure descriptor for a procedure,
 * plain value otherwise) — section base + section-relative value. */
static uint64_t evax_sym_value_addr(struct evax_input *in, int di, const struct evax_symbol *s)
{
    return in[di].sec_base[s->psindx] + s->value;
}
/* Resolved address of a symbol's CODE ENTRY (procedure entry point). */
static uint64_t evax_sym_code_addr(struct evax_input *in, int di, const struct evax_symbol *s)
{
    return in[di].sec_base[s->code_psindx] + s->code_value;
}

/* Ensure a psect's content buffer exists (zeroed to alloc) so a relocation
 * store slot can be patched into it. */
static uint8_t *evax_ensure_content(struct evax_section *sec)
{
    if (!sec->content && sec->alloc) {
        sec->content = calloc(1, (size_t)sec->alloc);
        if (!sec->content) die("oom allocating psect content for relocation");
    }
    return sec->content;
}

/* Bind an undefined EVAX reference to a symbol EXPORTED by a --use'd producer
 * (its .vms$sv universal) as a cross-image import (bead vms-c179). LINK.EXE
 * leaves the site's fill slot 0 and records a .vms$imp entry
 * {producer, sv_index, patch_off = site}; IMGACT resolves the universal against
 * the loaded producer at activation (ovmx_sv_resolve) and writes the run-time
 * address into the slot — the SAME .vms$imp mechanism the ELF path uses. This is
 * the genuine cross-image binding, NOT a fabricated local target (INV-6): the
 * slot is filled by a real producer symbol-vector entry at activation.
 *
 * Cross-image relocation forms (clean-room: STC_LP_PSB SYMG semantics from
 * binutils-2.43 bfd/vms-alpha.c _bfd_vms_slurp_etir + the actual assembled call
 * sequence in the checked-in EVAX fixture):
 *   LINKAGE (STC_LP_PSB against a SYMG): the site is the 2-quadword linkage pair;
 *     the un-relaxed call `ldq $27,SYM($gp); jsr $26,($27)` loads quad[0] into
 *     R27 (the procedure value) and jumps there, so quad[0] IS the slot IMGACT
 *     fills with the imported routine's run-time value (the producer .vms$sv
 *     PROCEDURE entry). patch_off = quad[0] site. quad[1] (the descriptor half a
 *     LOCAL {code,PDSC} pair carries) has no meaning for an imported routine and
 *     is left 0 — this call sequence never reads it. NOTE: the LOCAL path fills
 *     quad[0]=code-entry, quad[1]=PDSC (evax_apply_reloc below, unchanged, vms-01d);
 *     the cross-image path fills only quad[0] at activation. Both leave R27 = the
 *     value the call sequence loads, so they stay consistent for THIS sequence.
 *   REFQUAD / CODEADDR: a data pointer to the imported symbol; the 8-byte site IS
 *     the slot IMGACT fills. patch_off = site.
 * A cross-image REFLONG (a 32-bit slot) cannot hold a 64-bit run-time address, so
 * it is rejected honestly rather than truncated. */
static void evax_add_ximport(struct evax_input *in, int ii,
                             struct evax_section *sec, const struct evax_reloc *r,
                             struct producer *producers, int pidx, uint32_t svidx,
                             struct import **imp, int *nimp, int *imp_cap,
                             int *n_ximport)
{
    uint8_t *c = evax_ensure_content(sec);
    if (!c) die("cross-image relocation into a zero-length psect");
    uint64_t site_va = in[ii].sec_base[r->psect] + r->address;  /* image-relative */
    switch (r->type) {
    case EVAX_R_LINKAGE:
        if (r->address + 16 > sec->alloc) die("cross-image LINKAGE site past psect end");
        putl64(c + r->address, 0);        /* quad[0]: IMGACT fills = imported value  */
        putl64(c + r->address + 8, 0);    /* quad[1]: unused by the call sequence     */
        break;
    case EVAX_R_REFQUAD:
    case EVAX_R_CODEADDR:
        if (r->address + 8 > sec->alloc) die("cross-image data-import site past psect end");
        putl64(c + r->address, 0);        /* IMGACT fills = imported symbol address    */
        break;
    case EVAX_R_REFLONG:
        die("cross-image REFLONG import unsupported (a 32-bit slot cannot hold a "
            "64-bit run-time address) — the alpha-dec-vms port imports via "
            "LINKAGE/REFQUAD");
        break;
    default:
        die("unexpected relocation type for a cross-image import");
    }
    if (*nimp >= *imp_cap) {
        *imp_cap = *imp_cap ? *imp_cap * 2 : 16;
        *imp = realloc(*imp, (size_t)*imp_cap * sizeof **imp);
        if (!*imp) die("oom recording EVAX cross-image imports");
    }
    struct import *e = &(*imp)[(*nimp)++];
    memset(e, 0, sizeof *e);
    snprintf(e->name, sizeof e->name, "%s", r->sym);
    e->pidx    = pidx;
    e->svidx   = svidx;
    e->got_va  = site_va;   /* patch_off emitted into .vms$imp (vms_imp_write) */
    e->is_data = (r->type != EVAX_R_LINKAGE);
    e->is_weak = 0;
    (*n_ximport)++;
    fprintf(stderr, "%%LINK-I-IMPORT, EVAX cross-image import '%s' bound to --use "
            "producer %s [sv#%u], IMGACT-filled at image-relative 0x%llx (%s)\n",
            r->sym, producers[pidx].name, svidx, (unsigned long long)site_va,
            r->type == EVAX_R_LINKAGE ? "LINKAGE quad[0]" : "data pointer");
}

/* Fold an undefined reference that a --use'd producer exports as a GLOBALVALUE
 * (kind OVMX_SV_GLOBALVALUE). A VMS globalvalue is an ABSOLUTE LINK-TIME CONSTANT
 * — its ADDRESS is the value — so VMS resolves it at link, writing the constant
 * straight into the reference site exactly as an in-object absolute symbol is
 * written: no psect base, no load bias, no .vms$imp import cell. This mirrors the
 * ELF path (collect_globalvalues/gval_find, link.c ~1930) for the EVAX/Alpha
 * cross-image path — the alpha-dec-vms crt0's `&C$_EXIT1` REFQUAD is this case.
 * A globalvalue is a data value, never a call/entry target, so a LINKAGE/CODEADDR
 * reloc against one errors honestly rather than being folded into a call. (vms-069) */
static void evax_fold_globalvalue(struct evax_input *in, int ii,
                                  struct evax_section *sec,
                                  const struct evax_reloc *r, uint64_t value)
{
    uint8_t *c = evax_ensure_content(sec);
    if (!c) die("globalvalue fold into a zero-length psect");
    uint64_t site_va = in[ii].sec_base[r->psect] + r->address;   /* image-relative */
    uint64_t folded  = value + (uint64_t)r->addend;
    switch (r->type) {
    case EVAX_R_REFQUAD:
        if (r->address + 8 > sec->alloc) die("globalvalue REFQUAD site past psect end");
        putl64(c + r->address, folded);        /* absolute constant, folded at link */
        break;
    case EVAX_R_REFLONG:
        if (r->address + 4 > sec->alloc) die("globalvalue REFLONG site past psect end");
        putl32(c + r->address, (uint32_t)folded);
        break;
    case EVAX_R_LINKAGE:
    case EVAX_R_CODEADDR:
        fprintf(stderr, "%%LINK-F-GVALCALL, EVAX: %s reloc targets globalvalue "
                "'%s' — a globalvalue is an absolute data constant, not a "
                "call/entry target\n",
                r->type == EVAX_R_LINKAGE ? "LINKAGE" : "CODEADDR", r->sym);
        exit(1);
    default:
        die("unexpected relocation type folding a globalvalue");
    }
    fprintf(stderr, "%%LINK-I-GVALFOLD, EVAX globalvalue '%s' folded to absolute "
            "0x%llx at image-relative 0x%llx (link-time constant, no import cell)\n",
            r->sym, (unsigned long long)folded, (unsigned long long)site_va);
}

/* Apply one relocation into its psect's content buffer. `site_va` is only used
 * for diagnostics; the write lands in the content buffer at r->address. An
 * undefined reference that a --use'd producer EXPORTS binds as a cross-image
 * import (evax_add_ximport); one it exports as a GLOBALVALUE is folded to its
 * absolute constant at the reference site (evax_fold_globalvalue); one no
 * producer defines stays an honest %LINK-F-UNDEF (INV-6). */
static void evax_apply_reloc(struct evax_input *in, int nin, int ii,
                             const struct evax_reloc *r, int allow_undef,
                             struct producer *producers, int np,
                             struct import **imp, int *nimp, int *imp_cap,
                             int *n_ximport,
                             long *deferred, int *n_linkage_applied,
                             int *n_callsite_conservative)
{
    struct evax_object *o = &in[ii].obj;
    if (r->psect < 0 || r->psect >= o->nsec) die("relocation names a bad psect index");
    struct evax_section *sec = &o->sec[r->psect];

    /* Call-site relaxation relocs (STC_{NOP,BSR,LDA,BOH}_GBL): FIRST-LIGHT
     * CONSERVATIVE. The object already assembled the un-relaxed indirect call
     * (ldq $27,proc($gp) / jsr) into the psect content; these relocs only OFFER
     * a faster direct form. Leaving the instruction word untouched keeps the
     * correct indirect path through the linkage pair — an honest, working first
     * light, NOT a fake. Instruction relaxation is a later slice. */
    if (r->type == EVAX_R_NOP || r->type == EVAX_R_BSR ||
        r->type == EVAX_R_LDA || r->type == EVAX_R_BOH) {
        (*n_callsite_conservative)++;
        return;
    }

    /* Resolve the target address. Section-relative target (to_section >= 0) or a
     * symbol target (sym set). */
    uint64_t S = 0, code_S = 0;
    int have_code = 0;
    if (r->to_section >= 0) {
        if (r->to_section >= o->nsec) die("relocation names a bad target section");
        S = in[ii].sec_base[r->to_section];   /* this object's own psect base */
    } else {
        int di; const struct evax_symbol *ds;
        if (evax_find_sym(in, nin, r->sym, &di, &ds) < 0) {
            /* Not defined by any input object — is it EXPORTED by a --use'd
             * producer? If so, bind it as a cross-image import (real .vms$imp
             * mechanism, IMGACT-filled at activation). Only if NO producer
             * exports it do we fall through to the honest undef path (INV-6). */
            int pidx; uint32_t svidx;
            if (np > 0 && find_universal(producers, np, r->sym, &pidx, &svidx)) {
                /* Determine the KIND of the producer's export. A GLOBALVALUE is
                 * an absolute link-time constant — fold it into the site (no
                 * import cell); only a PROCEDURE/DATA export binds as a
                 * cross-image import. (vms-069) */
                const struct ovmx_sv_entry *pe =
                    &ovmx_sv_entries(producers[pidx].sv)[svidx];
                if (pe->kind == OVMX_SV_GLOBALVALUE) {
                    evax_fold_globalvalue(in, ii, sec, r, pe->value);
                    return;
                }
                evax_add_ximport(in, ii, sec, r, producers, pidx, svidx,
                                 imp, nimp, imp_cap, n_ximport);
                return;
            }
            if (allow_undef) {
                (*deferred)++;
                fprintf(stderr, "%%LINK-W-UNDEF, EVAX reference to undefined symbol "
                        "'%s' left 0 (--allow-undefined)\n", r->sym);
                return;   /* leave the store slot 0 (ELF weak-undef semantics) */
            }
            fprintf(stderr, "%%LINK-F-UNDEF, EVAX: undefined symbol '%s' referenced "
                    "by %s\n", r->sym, in[ii].name);
            exit(1);
        }
        S = evax_sym_value_addr(in, di, ds);   /* procedure descriptor / value */
        code_S = evax_sym_code_addr(in, di, ds);
        have_code = 1;
    }

    uint8_t *c = evax_ensure_content(sec);
    if (!c) die("relocation into a zero-length psect");

    switch (r->type) {
    case EVAX_R_REFLONG:
        if (r->address + 4 > sec->alloc) die("REFLONG site past psect end");
        putl32(c + r->address, (uint32_t)(S + r->addend));
        break;
    case EVAX_R_REFQUAD:
        if (r->address + 8 > sec->alloc) die("REFQUAD site past psect end");
        putl64(c + r->address, S + r->addend);
        break;
    case EVAX_R_CODEADDR:
        /* Store the target's CODE ENTRY address (a procedure's entry point). */
        if (r->address + 8 > sec->alloc) die("CODEADDR site past psect end");
        putl64(c + r->address, (have_code ? code_S : S) + r->addend);
        break;
    case EVAX_R_LINKAGE: {
        /* Alpha linkage pair: two quadwords at the site. Per binutils-2.43
         * bfd/vms-alpha.c _bfd_vms_slurp_etir STC_LP_PSB (authoritative clean-
         * room reference): quad[0] = resolved CODE ENTRY, quad[1] = resolved
         * PROCEDURE DESCRIPTOR (the symbol value). A locally-resolvable target
         * (this fixture) is filled directly. An undefined/cross-image target is
         * the DECC$SHR case that needs IMGACT's symbol-vector + producer GP — a
         * later slice; here it errors honestly (handled above via the undef
         * path) rather than emitting a wrong pair. */
        if (r->address + 16 > sec->alloc) die("LINKAGE site past psect end");
        if (!have_code) die("LINKAGE target is not a procedure symbol");
        putl64(c + r->address,     code_S);   /* quad[0] = code entry           */
        putl64(c + r->address + 8, S);        /* quad[1] = procedure descriptor */
        (*n_linkage_applied)++;
        break;
    }
    default:
        die("unhandled EVAX relocation type");
    }
}

/* Link a set of EVAX objects into a VMS-standard ELF ET_DYN image. Undefined
 * references EXPORTED by a --use'd producer bind as cross-image imports emitted
 * in a .vms$imp table IMGACT resolves at activation (bead vms-c179). */
static void emit_evax_image(struct evax_input *in, int nin,
                            const char *transfer, int allow_undef,
                            struct producer *producers, int np, const char *out)
{
    if (!transfer)
        die("the EVAX/Alpha image needs --transfer SYMBOL (the main transfer "
            "address; crt0 __main for a GCC-port image)");

    /* ---- Layout: merge same-named psects across objects, identity-mapped
     * (file offset == image vaddr) so a section's bytes sit at their vaddr. --- */
    uint64_t hdr_end = ALIGN_UP(sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr), 16);
    uint64_t cur = hdr_end;

    /* Output sections we emit headers for: one per distinct (rank-ordered) psect
     * name actually present, plus .vms$xfer. Track each output section's range. */
    struct outsec { char name[EVAX_NAME_MAX]; uint64_t addr, size; int nobits; };
    struct outsec osec[EVAX_MAX_SECTIONS + 3];   /* + .vms$xfer + .vms$imp */
    int nos = 0;

    for (int rank = 0; rank <= 4; rank++) {
        /* Gather the distinct psect names at this rank in first-seen order. */
        for (int i = 0; i < nin; i++)
            for (int s = 0; s < in[i].obj.nsec; s++) {
                struct evax_section *sec = &in[i].obj.sec[s];
                if (evax_rank(sec->name) != rank || sec->alloc == 0) continue;
                /* find/create the output section for this name */
                int oi = -1;
                for (int k = 0; k < nos; k++) if (!strcmp(osec[k].name, sec->name)) { oi = k; break; }
                if (oi < 0) {
                    oi = nos++;
                    snprintf(osec[oi].name, sizeof osec[oi].name, "%s", sec->name);
                    osec[oi].addr = 0; osec[oi].size = 0;
                    osec[oi].nobits = evax_is_nobits(sec->name);
                }
            }
        /* Place every contribution to each output section of this rank. */
        for (int k = 0; k < nos; k++) {
            if (evax_rank(osec[k].name) != rank) continue;
            uint64_t beg = 0; int begset = 0;
            for (int i = 0; i < nin; i++)
                for (int s = 0; s < in[i].obj.nsec; s++) {
                    struct evax_section *sec = &in[i].obj.sec[s];
                    if (strcmp(sec->name, osec[k].name) != 0 || sec->alloc == 0) continue;
                    uint64_t al = (uint64_t)1 << sec->align;
                    if (al < 1) al = 1;
                    cur = ALIGN_UP(cur, al);
                    if (!begset) { beg = cur; begset = 1; }
                    in[i].sec_base[s] = cur;
                    cur += sec->alloc;
                }
            if (begset) { osec[k].addr = beg; osec[k].size = cur - beg; }
        }
    }

    /* .vms$xfer: 16-byte header + count image-relative u64 transfer addresses.
     * First light: count == 1 (no LIB$INITIALIZE handlers), the single entry is
     * the --transfer symbol's descriptor address (image-relative). */
    int di; const struct evax_symbol *ds;
    if (evax_find_sym(in, nin, transfer, &di, &ds) < 0)
        die("--transfer symbol is not defined by any input object");
    uint64_t transfer_va = evax_sym_value_addr(in, di, ds);

    uint32_t xfer_count = 1;
    uint64_t xfer_size = sizeof(struct ovmx_xfer_header) + (uint64_t)xfer_count * 8;
    cur = ALIGN_UP(cur, 8);
    uint64_t xfer_addr = cur;
    cur += xfer_size;
    {
        int oi = nos++;
        snprintf(osec[oi].name, sizeof osec[oi].name, "%s", OVMX_XFER_SECTION);
        osec[oi].addr = xfer_addr; osec[oi].size = xfer_size; osec[oi].nobits = 0;
    }

    /* ---- Apply all relocations into the psect content buffers. Cross-image
     * references (an undefined symbol a --use'd producer exports) are collected
     * as imports here; their .vms$imp table is laid out right after. ---- */
    long deferred = 0;
    int n_linkage = 0, n_callsite = 0, n_data = 0, n_ximport = 0;
    struct import *imp = NULL; int nimp = 0, imp_cap = 0;
    for (int i = 0; i < nin; i++)
        for (int r = 0; r < in[i].obj.nreloc; r++) {
            enum evax_reloc_type t = in[i].obj.reloc[r].type;
            evax_apply_reloc(in, nin, i, &in[i].obj.reloc[r], allow_undef,
                             producers, np, &imp, &nimp, &imp_cap, &n_ximport,
                             &deferred, &n_linkage, &n_callsite);
            if (t == EVAX_R_REFLONG || t == EVAX_R_REFQUAD || t == EVAX_R_CODEADDR)
                n_data++;
        }

    /* ---- .vms$imp: cross-image imports bound to --use producers. Each record
     * names {producer soname, symbol-vector index, patch_off}; IMGACT resolves
     * the universal against the loaded producer and writes the run-time address
     * into patch_off (the linkage-pair quad[0] or the data-pointer slot) at
     * activation — the SAME table + IMGACT contract the ELF path emits
     * (vms_imp_write). (bead vms-c179) ---- */
    uint64_t off_imp = 0, imp_size = 0;
    if (nimp > 0) {
        cur = ALIGN_UP(cur, 8);
        off_imp = cur;
        imp_size = vms_imp_size(nimp, imp, producers, np);
        cur += imp_size;
        int oi = nos++;
        snprintf(osec[oi].name, sizeof osec[oi].name, "%s", OVMX_IMP_SECTION);
        osec[oi].addr = off_imp; osec[oi].size = imp_size; osec[oi].nobits = 0;
    }

    uint64_t file_end = cur;           /* end of loadable file content          */
    /* (nobits $BSS$ already got vaddrs above and extends memsz past file_end;
     * for first light our fixtures carry no $BSS$, so memsz == file_end.) */
    uint64_t mem_end = cur;

    /* ---- Build the shstrtab. ---- */
    char shstr[4096]; size_t shlen = 0;
    shstr[shlen++] = '\0';
    uint32_t sh_name_off[EVAX_MAX_SECTIONS + 4];
    for (int k = 0; k < nos; k++) {
        sh_name_off[k] = (uint32_t)shlen;
        size_t l = strlen(osec[k].name) + 1;
        if (shlen + l > sizeof shstr) die("shstrtab overflow");
        memcpy(shstr + shlen, osec[k].name, l); shlen += l;
    }
    uint32_t sh_shstr_name = (uint32_t)shlen;
    memcpy(shstr + shlen, ".shstrtab", 10); shlen += 10;

    uint64_t off_shstr = ALIGN_UP(file_end, 8);
    uint64_t off_shdr  = ALIGN_UP(off_shstr + shlen, 8);
    int nshdr = 1 /*NULL*/ + nos + 1 /*.shstrtab*/;
    uint64_t total = off_shdr + (uint64_t)nshdr * sizeof(Elf64_Shdr);

    uint8_t *img = calloc(1, (size_t)total);
    if (!img) die("oom building EVAX image");

    /* ELF header. */
    Elf64_Ehdr *eh = (Elf64_Ehdr *)img;
    memcpy(eh->e_ident, ELFMAG, SELFMAG);
    eh->e_ident[EI_CLASS]   = ELFCLASS64;
    eh->e_ident[EI_DATA]    = ELFDATA2LSB;
    eh->e_ident[EI_VERSION] = EV_CURRENT;
    eh->e_type    = ET_DYN;
    eh->e_machine = EM_ALPHA;
    eh->e_version = EV_CURRENT;
    eh->e_entry   = transfer_va;
    eh->e_phoff   = sizeof(Elf64_Ehdr);
    eh->e_shoff   = off_shdr;
    eh->e_ehsize  = sizeof(Elf64_Ehdr);
    eh->e_phentsize = sizeof(Elf64_Phdr);
    eh->e_phnum   = 1;
    eh->e_shentsize = sizeof(Elf64_Shdr);
    eh->e_shnum   = nshdr;
    eh->e_shstrndx = nshdr - 1;

    /* One PT_LOAD over the whole image (RWX — first light; refinement later). */
    Elf64_Phdr *ph = (Elf64_Phdr *)(img + sizeof(Elf64_Ehdr));
    ph->p_type = PT_LOAD; ph->p_flags = PF_R | PF_W | PF_X;
    ph->p_offset = 0; ph->p_vaddr = 0; ph->p_paddr = 0;
    ph->p_filesz = file_end; ph->p_memsz = mem_end; ph->p_align = 0x1000;

    /* Copy each psect's (relocated) content to its identity-mapped file offset. */
    for (int i = 0; i < nin; i++)
        for (int s = 0; s < in[i].obj.nsec; s++) {
            struct evax_section *sec = &in[i].obj.sec[s];
            if (sec->alloc == 0 || evax_is_nobits(sec->name)) continue;
            uint64_t base = in[i].sec_base[s];
            if (base + sec->alloc > file_end) die("psect content past file image");
            if (sec->content) memcpy(img + base, sec->content, (size_t)sec->alloc);
            /* NULL content == all zero, already zeroed by calloc. */
        }

    /* Stamp .vms$xfer. */
    struct ovmx_xfer_header xh;
    xh.magic = OVMX_XFER_MAGIC; xh.flavor = OVMX_ACT_VMS_STD;
    xh.count = xfer_count; xh.reserved = 0;
    putl32(img + xfer_addr + 0,  xh.magic);
    putl32(img + xfer_addr + 4,  xh.flavor);
    putl32(img + xfer_addr + 8,  xh.count);
    putl32(img + xfer_addr + 12, xh.reserved);
    putl64(img + xfer_addr + 16, transfer_va);   /* entry[0] = main transfer   */

    /* Stamp .vms$imp (cross-image imports). patch_off = imp[i].got_va (the site
     * image-relative address), producer soname + sv index per record. */
    if (nimp > 0)
        vms_imp_write(img, off_imp, imp, nimp, producers, np);

    /* shstrtab + section headers. */
    memcpy(img + off_shstr, shstr, shlen);
    Elf64_Shdr *sh = (Elf64_Shdr *)(img + off_shdr);
    /* sh[0] = NULL (zeroed). */
    for (int k = 0; k < nos; k++) {
        Elf64_Shdr *s = &sh[1 + k];
        s->sh_name = sh_name_off[k];
        s->sh_type = osec[k].nobits ? SHT_NOBITS : SHT_PROGBITS;
        s->sh_flags = SHF_ALLOC | (strcmp(osec[k].name, "$CODE$") == 0 ? SHF_EXECINSTR : SHF_WRITE);
        s->sh_addr = osec[k].addr;
        s->sh_offset = osec[k].nobits ? file_end : osec[k].addr;  /* identity map */
        s->sh_size = osec[k].size;
        s->sh_addralign = 16;
    }
    Elf64_Shdr *sstr = &sh[nshdr - 1];
    sstr->sh_name = sh_shstr_name; sstr->sh_type = SHT_STRTAB;
    sstr->sh_offset = off_shstr; sstr->sh_size = shlen; sstr->sh_addralign = 1;

    /* Write the image. */
    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) die("cannot create output image");
    size_t w = 0;
    while (w < total) {
        ssize_t n = write(fd, img + w, (size_t)total - w);
        if (n <= 0) die("short write output image");
        w += (size_t)n;
    }
    close(fd);

    fprintf(stderr,
        "%%LINK-S-CREATED, %s: EVAX/Alpha ET_DYN image, %d object%s, "
        "%d data reloc%s applied, %d linkage pair%s applied, "
        "%d call-site reloc%s kept indirect (first-light), .vms$xfer count=%u\n",
        out, nin, nin == 1 ? "" : "s",
        n_data, n_data == 1 ? "" : "s",
        n_linkage, n_linkage == 1 ? "" : "s",
        n_callsite, n_callsite == 1 ? "" : "s", xfer_count);
    /* Layout map (stderr) — aids hand-tracing + the integration test. */
    for (int k = 0; k < nos; k++)
        fprintf(stderr, "%%LINK-I-SECT, %-10s addr=0x%llx size=0x%llx\n",
                osec[k].name, (unsigned long long)osec[k].addr,
                (unsigned long long)osec[k].size);
    fprintf(stderr, "%%LINK-I-XFER, transfer '%s' -> image-relative 0x%llx\n",
            transfer, (unsigned long long)transfer_va);
    if (n_ximport)
        fprintf(stderr, "%%LINK-I-IMPORT, %d EVAX cross-image import%s bound to "
                "--use producer%s (.vms$imp at 0x%llx, resolved at activation)\n",
                n_ximport, n_ximport == 1 ? "" : "s", np == 1 ? "" : "s",
                (unsigned long long)off_imp);
    if (deferred)
        fprintf(stderr, "%%LINK-W-DEFERRED, %ld EVAX reference%s left undefined\n",
                deferred, deferred == 1 ? "" : "s");
    free(imp);
}

/* Sniff a file's object format from its first bytes (input already on disk). */
enum { EVFMT_ELF, EVFMT_EVAX, EVFMT_OTHER };
static int evax_sniff(const uint8_t *buf, size_t n)
{
    if (n >= SELFMAG && memcmp(buf, ELFMAG, SELFMAG) == 0) return EVFMT_ELF;
    if (evax_is_object(buf, n)) return EVFMT_EVAX;
    return EVFMT_OTHER;
}

/* Classify an input's object format from a byte-exact RMS header PEEK — never a
 * whole-file slurp. Slurping to classify would read the file a SECOND time
 * through the RMS trace (the ELF path re-reads it in load_obj), doubling the
 * self-link's read-total and breaking run_link_native.sh's byte-exact
 * read-total assertion (vms-cbe). The peek reads only the first bytes via the
 * same open/get/close seam file_is_archive uses, so it is NOT counted by that
 * per-file sys$get-loop total. evax_sniff needs SELFMAG (4) for ELF and 6 bytes
 * for the EMH check; 16 is a safe margin. Archives/.OLB take the ELF path. */
static int sniff_format(const char *path)
{
    if (file_is_archive(path) || file_is_olb(path)) return EVFMT_ELF;
    uint8_t hdr[16];
    int n;
#ifdef OVMX_RMS_IO
    n = ovmx_link_rms_peek(path, hdr, (int)sizeof hdr);
    if (n < 0) die("cannot open input file (RMS)");
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("cannot open input file");
    ssize_t r = read(fd, hdr, sizeof hdr);
    close(fd);
    n = (r < 0) ? 0 : (int)r;
#endif
    return evax_sniff(hdr, (size_t)n);
}

int main(int argc, char **argv)
{
    const char *out = NULL;
    const char **ins = calloc((size_t)argc, sizeof *ins);  /* <= argc inputs */
    int nin = 0;
    static struct univ uv[MAX_UNIV];   /* static (BSS): MAX_UNIV*sizeof(univ) is
                                        * ~1MB with the R1b decc$ vector — off the stack. */
    int nuniv = 0;
    int shareable = 0, executable = 0, allow_undef = 0;
    struct producer *producers = calloc((size_t)argc, sizeof *producers);
    int np = 0;
    const char *transfer = NULL;   /* EVAX/Alpha main transfer symbol (vms-cbe) */
    uint32_t gk = OVMX_GSMATCH_EQUAL, gmaj = 0, gmin = 0;
    if (!ins || !producers) die("oom parsing arguments");
    memset(uv, 0, sizeof uv);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(argv[i], "--shareable") == 0) {
            shareable = 1;
        } else if (strcmp(argv[i], "--executable") == 0) {
            executable = 1;
        } else if (strcmp(argv[i], "--allow-undefined") == 0) {
            allow_undef = 1;
        } else if (strcmp(argv[i], "--use") == 0 && i + 1 < argc) {
            load_producer(argv[++i], &producers[np++]);
        } else if (strcmp(argv[i], "--symbol-vector") == 0 && i + 1 < argc) {
            nuniv = parse_symbol_vector(argv[++i], uv);
        } else if (strcmp(argv[i], "--gsmatch") == 0 && i + 1 < argc) {
            parse_gsmatch(argv[++i], &gk, &gmaj, &gmin);
        } else if (strcmp(argv[i], "--transfer") == 0 && i + 1 < argc) {
            transfer = argv[++i];   /* EVAX/Alpha main transfer address (vms-cbe) */
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "%%LINK-W-IGNORED, unknown option %s\n", argv[i]);
        } else {
            ins[nin++] = argv[i];
        }
    }
    if (nin == 0) die("no input (usage: LINK.EXE --shareable "
                  "--symbol-vector \"f=PROCEDURE\" --gsmatch LEQUAL,1,0 [--allow-undefined] "
                  "-o X.EXE a.o [b.o | lib.a ...] "
                  "| LINK.EXE --executable --use LIB$SHR.EXE -o PROG.EXE prog.o)");
    if (!out) die("no -o output");

    /* ---- Format dispatch (bead vms-cbe). An EVAX (Alpha/VMS) object — first
     * record is an EMH, never ELF magic — takes the dedicated Alpha path
     * (emit_evax_image); an ELF object set takes emit_shareable below. The two
     * are never mixed in one link. The EVAX path does not use
     * --shareable/--executable/--symbol-vector, so it dispatches BEFORE those
     * checks. ---- */
    if (sniff_format(ins[0]) == EVFMT_EVAX) {
        /* EVAX/Alpha path. Slurp each input ONCE here (no byte-exact read-total
         * gate on this path), sniff it from the full buffer, and hand it to
         * evax_read. The ELF path falls through and does its single traced
         * read per input in load_obj — so classifying ins[0] above uses the
         * non-counted header peek, not a slurp (vms-cbe). */
        struct evax_input *ein = calloc((size_t)nin, sizeof *ein);
        if (!ein) die("oom allocating EVAX input table");
        for (int i = 0; i < nin; i++) {
            size_t sz; uint8_t *b = slurp(ins[i], &sz);
            if (file_is_archive(ins[i]) || file_is_olb(ins[i]) ||
                evax_sniff(b, sz) != EVFMT_EVAX)
                die("the EVAX/Alpha link takes plain EVAX objects only "
                    "(mixed formats / EVAX archives are a later slice)");
            ein[i].name = ins[i];
            if (evax_read(b, sz, &ein[i].obj) != 0) {
                fprintf(stderr, "%%LINK-F-EVAX, %s: %s\n", ins[i], evax_last_error());
                exit(1);
            }
        }
        emit_evax_image(ein, nin, transfer, allow_undef, producers, np, out);
        return 0;
    }
    /* ELF object set: fall through to emit_shareable. load_obj does the single
     * byte-exact RMS read per input; no slurp happened above. */

    if (shareable == executable)
        die("specify exactly one of --shareable / --executable");

    if (executable && np == 0)
        die("--executable needs at least one --use producer image (DECC$SHR)");
    if (!executable && nuniv == 0)
        die("a shareable image needs --symbol-vector");

    /* ---- One or more objects and/or whole `.a` archives ----
     * Inputs grow dynamically; an archive expands to all of its object members
     * in-process (whole-archive, no `ld -r`). Both --shareable and --executable
     * ingest inputs identically and run the SAME emit path (emit_shareable with
     * is_exec); an executable additionally synthesizes crt0 + PT_INTERP. (vms-004,
     * vms-ba1) */
    struct obj *objs = NULL; int nobj = 0, cap = 0;
    struct olb_pool *pools = calloc((size_t)nin, sizeof *pools);
    int npool = 0;
    if (nin && !pools) die("oom tracking object libraries");
    for (int i = 0; i < nin; i++) {
        if (file_is_olb(ins[i]))
            load_olb_pool(ins[i], &pools[npool++]);     /* selective (.OLB) */
        else if (file_is_archive(ins[i]))
            load_archive(ins[i], &objs, &nobj, &cap);   /* whole-archive (.a) */
        else
            load_obj(ins[i], push_obj(&objs, &nobj, &cap));
    }
    /* Search object libraries after the mandatory objects/archives, pulling only
     * the members that resolve outstanding references (vms-ca9). The selective
     * search is rooted at both the root objects' undefined refs AND the
     * --symbol-vector universals, so a /SHAREABLE link whose only roots are its
     * symbol vector (the VMS way — no explicit TU list) pulls the defining
     * modules from the .OLB alone (design-vms-native-shareable-build.md C.4.1). */
    if (npool)
        resolve_olbs(&objs, &nobj, &cap, pools, npool, uv, nuniv);
    free(pools);
    if (nobj == 0) die("no object members found in inputs");
    emit_shareable(objs, nobj, uv, nuniv, gk, gmaj, gmin, allow_undef,
                   producers, np, out, executable);
    return 0;
}

/* --------------------------------------------------------------------------
 * vms-cbe: the EVAX (Alpha/VMS) object front end is compiled AS PART OF this
 * translation unit. link.c is built by ~40 sites (every mk_*.sh / run_*.sh that
 * produces a LINK.EXE, plus the CMake `vmslink` target); pulling evax_read.c in
 * here means none of them needs a new source-list entry — sidestepping the
 * new-TU enumeration trap that has repeatedly reddened a stray CI leg. There is
 * no double definition: no build both links this TU and a separate evax_read.o
 * (the standalone reader unit test, run_evax_read.sh, compiles evax_read.c on
 * its own and links only evax_read_test.o with it). The declarations are already
 * visible via the "evax_read.h" include near the top.
 * -------------------------------------------------------------------------- */
#include "evax_read.c"
