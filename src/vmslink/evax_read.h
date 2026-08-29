/*
 * evax_read.h — reader for the Alpha/VMS (EVAX) object format (bead vms-cbe).
 *
 * The OpenVMS GCC port (`alpha-dec-vms`) emits its object files in the native
 * VMS "EVAX" object format, NOT ELF — that format is intrinsic to the target
 * (the VMS calling standard + __INITIAL_POINTER_SIZE are the target's identity;
 * see gcc/config/alpha/vms.h "Alpha/VMS object format is not really Elf"). The
 * faithful do-it-like-VMS rung is therefore for LINK.EXE to READ that native
 * output unchanged, rather than force the port to emit ELF (which would mean
 * hacking the port's identity). This is that reader's front end.
 *
 * CLEAN-ROOM NOTE (CLAUDE.md Rule 8): the EVAX record/relocation format read
 * here is derived ONLY from public sources — the field layouts in the GPL
 * binutils include/vms headers and the public OpenVMS Linker/object
 * documentation. No VSI/HPE source or binary was disassembled or copied.
 *
 * SCOPE: slice 1 parses the records (framing), the module header (EMH/MHD),
 * and the global symbol directory (EGSD) — psects + symbols. Slice 2 adds the
 * ETIR relocation RECOGNIZER: the STA/STO/OPR/CTL/STC command stack-machine
 * that yields a list of relocations (offset, type, target, addend), recognizing
 * every reloc TYPE including the Alpha linkage ones. Slice 3 (this revision)
 * additionally MATERIALIZES psect CONTENT — replaying the STO_IMM immediate
 * bytes into a per-psect buffer (struct evax_section.content) so the linker has
 * image bytes to relocate into — and extracts a normal procedure's CODE ENTRY
 * (code_value/code_psindx) alongside its procedure-descriptor value, which the
 * LINKAGE-pair store needs. APPLYING the relocations (slice 4) and the format
 * dispatch that feeds this front end live in link.c's object loader.
 */
#ifndef OVMX_EVAX_READ_H
#define OVMX_EVAX_READ_H

#include <stddef.h>
#include <stdint.h>

#define EVAX_NAME_MAX     32     /* VMS symbol/psect names are <= 31 chars + NUL */
#define EVAX_MAX_SECTIONS 64
#define EVAX_MAX_SYMBOLS  1024
#define EVAX_MAX_RELOCS   4096

/* A psect (program section) definition from an EGSD PSC entry. `flags` are the
 * EGPS__V_* bits (REL/EXE/WRT/RD/…); `alloc` is this object's contribution size;
 * `align` is the power-of-two alignment exponent.
 *
 * `content` (slice 3) is the initialized image bytes of the psect, `alloc` long,
 * materialized by replaying the ETIR STO_IMM commands (which carry the literal
 * instruction/data bytes) into a per-psect buffer. It is NULL when the psect
 * emitted no immediate bytes (a bytes-are-all-zero psect, e.g. $BSS$, or a psect
 * whose every byte is a relocation-store slot): a NULL content means "all zero",
 * and the linker treats it as `alloc` zero bytes. The buffer is malloc'd and
 * lives until process exit (LINK.EXE, like the ELF path, never frees). */
struct evax_section {
    char     name[EVAX_NAME_MAX];
    uint32_t flags;
    uint64_t alloc;
    uint8_t  align;
    uint8_t *content;      /* [alloc] initialized bytes, or NULL == all-zero */
};

/* A symbol from an EGSD SYM entry. `defined` distinguishes a definition (esdf:
 * carries value + psindx) from an undefined external reference (esrf). `flags`
 * are the EGSY__V_* bits; `is_proc` mirrors EGSY__V_NORM (a normal procedure
 * definition); `psindx` is the 0-based index into `sec[]` of the defining
 * psect (defined symbols only).
 *
 * `value`/`psindx` locate the symbol's VALUE — for a normal procedure this is
 * its PROCEDURE DESCRIPTOR (the .pdesc, its procedure value / PV). A normal
 * procedure ALSO carries a distinct CODE ENTRY point: `code_value`/`code_psindx`
 * (esdf code_address + ca_psindx). Both are section-relative (section base is
 * added at layout). The Alpha LINKAGE pair stores <code entry, PDSC>, so BOTH
 * are needed to resolve one (slice 4). For a non-procedure defined symbol the
 * code fields mirror value/psindx. (Field layout: include/vms/esdf.h.) */
struct evax_symbol {
    char     name[EVAX_NAME_MAX];
    int      defined;
    int      is_proc;
    uint16_t flags;
    uint64_t value;
    uint32_t psindx;
    uint64_t code_value;   /* procedure code-entry offset (esdf code_address) */
    uint32_t code_psindx;  /* psect of the code entry (esdf ca_psindx)        */
};

/* Relocation kind, mapped from the recognized ETIR command sequence. The names
 * mirror the BFD_RELOC_* / BFD_RELOC_ALPHA_* the binutils recognizer emits. */
enum evax_reloc_type {
    EVAX_R_REFLONG,   /* 32-bit direct (STO_LW / STO_GBL_LW)          */
    EVAX_R_REFQUAD,   /* 64-bit direct (STO_QW / STO_GBL / STO_OFF)   */
    EVAX_R_CODEADDR,  /* code-address (STO_CA)                        */
    EVAX_R_LINKAGE,   /* Alpha linkage pair (STC_LP_PSB)              */
    EVAX_R_NOP,       /* call-site relaxation (STC_NOP_GBL)           */
    EVAX_R_BSR,       /* call-site relaxation (STC_BSR_GBL)           */
    EVAX_R_LDA,       /* call-site relaxation (STC_LDA_GBL)           */
    EVAX_R_BOH,       /* call-site relaxation (STC_BOH_GBL)           */
    /* [OVMX] vms-4ed (component C2 of vms-5f5): the OVMX-private
     * GP-displacement relocation. Marks a callee's ldah/lda GP-establish
     * prologue pair; the OVMX linker patches the signed -K immediate, where
     * K = the enclosing procedure's PDSC offset within its module linkage
     * section (C1's evax_gp_entry table). NOT a VMS-authentic reloc — EVAX
     * publishes no GP-displacement encoding (OSF/Alpha uses ldgp/GPDISP), so
     * this is OVMX's analogue with an OVMX wire form. See
     * docs/design-alpha-per-image-gp.md §2.1/§2.2. For a reloc of this type the
     * struct evax_reloc fields carry: `psect` = the code psect index of the
     * pair, `address` = the ldah word's psect-relative offset (ldah_vaddr),
     * `addend` = the lda delta (ldah_vaddr + addend locates the lda word,
     * normally 4), `sym` = the enclosing procedure name (the evax_gp_entry key),
     * `to_section` = -1. */
    EVAX_R_OVMX_GPDISP,
};

/* One relocation: patch `address` bytes within psect index `psect`. The target
 * is a symbol (`sym[0]` set, `to_section < 0`) or a psect (`to_section >= 0`,
 * `sym[0]==0` — a section-relative reference, e.g. STA_PQ + STO_OFF). */
struct evax_reloc {
    enum evax_reloc_type type;
    int      psect;        /* section the location lives in (cur_psect)  */
    uint64_t address;      /* byte offset within that section            */
    int      to_section;   /* target psect index, or -1 for a symbol     */
    char     sym[EVAX_NAME_MAX];
    uint64_t addend;
    /* [OVMX] vms-095 (C3 of vms-5f5): for EVAX_R_OVMX_GPDISP only — the enclosing
     * procedure's PDSC offset within its $LINK$ psect (the gas operand's first
     * u32, formerly the unused lkidx_or_0).  The linker resolves K =
     * placed_PDSC - $LINK$ base from this, so a LOCAL/static procedure (which has
     * no global symbol to key a name lookup on) resolves just like a global. */
    uint64_t gp_pdsc_off;
};

struct evax_object {
    char                module[EVAX_NAME_MAX];
    int                 nsec;
    struct evax_section sec[EVAX_MAX_SECTIONS];
    int                 nsym;
    struct evax_symbol  sym[EVAX_MAX_SYMBOLS];
    int                 nreloc;
    struct evax_reloc   reloc[EVAX_MAX_RELOCS];
};

/* 1 if `buf` (length `len`) looks like an EVAX object: its first record is an
 * EVAX module header (EMH). Used to dispatch between the ELF and EVAX object
 * front ends. An ELF object starts with 0x7f 'E' 'L' 'F', which never matches. */
int evax_is_object(const uint8_t *buf, size_t len);

/* Parse the EMH (module name) + EGSD (sections + symbols) of an EVAX object
 * into `*out`. ETIR/EDBG/ETBT records are skipped in this slice. Returns 0 on
 * success, -1 on a malformed object (details via evax_last_error()). */
int evax_read(const uint8_t *buf, size_t len, struct evax_object *out);

/* The last error message set by a failed evax_read() / evax_is_object(). */
const char *evax_last_error(void);

#endif /* OVMX_EVAX_READ_H */
