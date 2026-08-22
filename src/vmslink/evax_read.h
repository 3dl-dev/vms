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
 * and the global symbol directory (EGSD) — psects + symbols. Slice 2 (this
 * revision) adds the ETIR relocation RECOGNIZER: the STA/STO/OPR/CTL/STC
 * command stack-machine that yields a list of relocations (offset, type,
 * target, addend). Slice 2 recognizes every reloc TYPE, including the Alpha
 * linkage/GP ones; APPLYING the hard linkage/GP relocs (the two-quadword
 * linkage-pair store + link-time instruction patching) is a following slice,
 * as is wiring the front end into link.c's object loader.
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
 * `align` is the power-of-two alignment exponent. */
struct evax_section {
    char     name[EVAX_NAME_MAX];
    uint32_t flags;
    uint64_t alloc;
    uint8_t  align;
};

/* A symbol from an EGSD SYM entry. `defined` distinguishes a definition (esdf:
 * carries value + psindx) from an undefined external reference (esrf). `flags`
 * are the EGSY__V_* bits; `is_proc` mirrors EGSY__V_NORM (a normal procedure
 * definition); `psindx` is the 0-based index into `sec[]` of the defining
 * psect (defined symbols only). */
struct evax_symbol {
    char     name[EVAX_NAME_MAX];
    int      defined;
    int      is_proc;
    uint16_t flags;
    uint64_t value;
    uint32_t psindx;
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
