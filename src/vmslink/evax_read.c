/*
 * evax_read.c — Alpha/VMS (EVAX) object reader front end (bead vms-cbe).
 *
 * See evax_read.h for scope + the clean-room note. This slice reads the record
 * framing, the module header (EMH/MHD), and the global symbol directory (EGSD:
 * PSC psects + SYM symbols). Field layouts are the public binutils include/vms
 * structures; every offset here is checked against a real EVAX object produced
 * by binutils-2.43 `alpha-dec-vms-as` (see test/evax-fixtures/sample.s|.obj and
 * test/run_evax_read.sh).
 *
 * On-disk framing (confirmed against the fixture bytes): a native VMS object is
 * a sequence of records, each preceded by a 2-byte little-endian RMS length
 * prefix, then the record proper `[u16 rectyp][u16 rec_size][payload]`. rec_size
 * counts the record proper (rectyp+size+payload); the next record starts at
 * (this record's start + 2 + rec_size), rounded up to an even offset. (This is
 * BFD's FF_FOREIGN framing; cf. maybe_adjust_record_pointer_for_object.)
 */
#include "evax_read.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- record types (include/vms/eobjrec.h) ---- */
#define EOBJ__C_EMH   8
#define EOBJ__C_EEOM  9
#define EOBJ__C_EGSD  10
#define EOBJ__C_ETIR  11
#define EOBJ__C_EDBG  12
#define EOBJ__C_ETBT  13

/* ---- EMH subtypes (include/vms/emh.h) ---- */
#define EMH__C_MHD    0

/* ---- EGSD entry types (include/vms/egsd.h) ---- */
#define EGSD__C_PSC   0
#define EGSD__C_SYM   1

/* ---- EGSY symbol flags (include/vms/egsy.h) ---- */
#define EGSY__V_WEAK  0x0001
#define EGSY__V_DEF   0x0002
#define EGSY__V_REL   0x0008
#define EGSY__V_NORM  0x0040

/* ---- fixed field offsets within an EGSD entry, from the include/vms structs.
 * PSC (vms_egps): gsdtyp[2] gsdsiz[2] align[1] temp[1] flags[2] alloc[4]
 *                 namlng[1] name[].
 * SYM (vms_egsy header = 8 bytes): gsdtyp[2] gsdsiz[2] datyp[1] temp[1] flags[2].
 *   defined  (vms_esdf): value[8] code_address[8] ca_psindx[4] psindx[4]
 *                        namlng[1] name[]   -> namlng at offset 32 (ESDF__B_NAMLNG)
 *   undef    (vms_esrf): namlng[1] name[]   -> namlng at offset 8  (ESRF__B_NAMLNG) */
#define PSC_ALIGN_OFF   4
#define PSC_FLAGS_OFF   6
#define PSC_ALLOC_OFF   8
#define PSC_NAMLNG_OFF  12
#define SYM_FLAGS_OFF   6
#define ESDF_VALUE_OFF    8   /* symbol value (procedure descriptor for a proc) */
#define ESDF_CODEADDR_OFF 16  /* code entry offset (code_address, normal procs) */
#define ESDF_CAPSINDX_OFF 24  /* psect of the code entry (ca_psindx)            */
#define ESDF_PSINDX_OFF   28  /* psect of the value/descriptor                  */
#define ESDF_NAMLNG_OFF   32
#define ESRF_NAMLNG_OFF   8

/* ---- ETIR command opcodes (include/vms/etir.h) ---- */
#define ETIR__C_STA_GBL     0    /* push global symbol                        */
#define ETIR__C_STA_LW      1    /* push longword literal (addend)            */
#define ETIR__C_STA_QW      2    /* push quadword literal (addend)            */
#define ETIR__C_STA_PQ      3    /* push psect base + offset                  */
#define ETIR__C_STO_LW      52   /* store longword  -> REFLONG                */
#define ETIR__C_STO_QW      53   /* store quadword  -> REFQUAD                */
#define ETIR__C_STO_GBL     55   /* store global    -> REFQUAD vs symbol      */
#define ETIR__C_STO_CA      56   /* store code addr -> CODEADDR               */
#define ETIR__C_STO_OFF     59   /* store psect off -> REFQUAD, section-rel   */
#define ETIR__C_STO_IMM     61   /* store immediate raw bytes (advance vaddr) */
#define ETIR__C_STO_GBL_LW  62   /* store global lw -> REFLONG vs symbol      */
#define ETIR__C_OPR_ADD     101  /* fold stacked base + addend                */
#define ETIR__C_CTL_SETRB   150  /* set relocation base (cur_psect + vaddr)   */
#define ETIR__C_STC_LP_PSB  201  /* store-conditional linkage pair -> LINKAGE */
#define ETIR__C_STC_NOP_GBL 205  /* call-site relaxation -> NOP               */
#define ETIR__C_STC_BSR_GBL 207  /* call-site relaxation -> BSR               */
#define ETIR__C_STC_LDA_GBL 209  /* call-site relaxation -> LDA               */
#define ETIR__C_STC_BOH_GBL 211  /* call-site relaxation -> BOH               */
/* [OVMX] vms-4ed (component C2 of vms-5f5): OVMX-private ETIR command for the
 * OVMX GP-displacement relocation. EVAX/VSI publishes NO GP-displacement
 * encoding, so OVMX defines its own and reserves the private ETIR command range
 * 0xEF00-0xEFFF for OVMX use. This value is OUTSIDE VSI's own opcode space
 * (which tops out near ETIR__C_MAXSTCCOD = 214), so a real VSI object can never
 * collide with it, and it is NOT a VSI-authentic command. Must match the
 * assembler's define (tools/cross-alpha-vms/patches/0006-vms-4ed-…, which adds
 * the identical ETIR__C_OVMX_GPDISP to binutils include/vms/etir.h). See
 * docs/design-alpha-per-image-gp.md §2.2. */
#define ETIR__C_OVMX_GPDISP 0xEF01

static char g_err[256];
static void set_err(const char *m) { snprintf(g_err, sizeof g_err, "%s", m); }
const char *evax_last_error(void) { return g_err; }

/* Little-endian field accessors. */
static uint16_t getl16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t getl32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t getl64(const uint8_t *p)
{
    return (uint64_t)getl32(p) | ((uint64_t)getl32(p + 4) << 32);
}

/* Copy a counted VMS name (namlng byte at name[-1]) into a NUL-terminated buf,
 * clamped to EVAX_NAME_MAX-1. `nl` bytes start at `s`. */
static void copy_name(char *dst, const uint8_t *s, uint8_t nl)
{
    if (nl >= EVAX_NAME_MAX) nl = EVAX_NAME_MAX - 1;
    memcpy(dst, s, nl);
    dst[nl] = '\0';
}

int evax_is_object(const uint8_t *buf, size_t len)
{
    /* First record: [u16 rms_len][u16 rectyp=EMH]. An ELF object begins with
     * 0x7f 'E' 'L' 'F' (rectyp bytes would be 'E''L' = 0x4c45 != EMH). */
    if (len < 6) { set_err("too short for an object record"); return 0; }
    return getl16(buf + 2) == EOBJ__C_EMH;
}

/* Parse an EMH record's payload (record proper at `rec`, rec_size `rsz`). Only
 * the MHD subtype carries the module name; other EMH subtypes are ignored. */
static void parse_emh(const uint8_t *rec, uint16_t rsz, struct evax_object *out)
{
    if (rsz < 6) return;
    if (getl16(rec + 4) != EMH__C_MHD) return;   /* subtype at rec+4 */
    /* MHD body at rec+6: strlvl[1] temp[1] arch1[4] arch2[4] recsiz[4] then the
     * module name as a counted (ASCIC) string -> name length at rec+20. */
    if (rsz < 21) return;
    uint8_t nl = rec[20];
    if (21 + (size_t)nl > rsz) return;
    copy_name(out->module, rec + 21, nl);
}

/* Parse an EGSD record (record proper at `rec`, size `rsz`) — its PSC + SYM
 * entries. Returns 0 on success, -1 on a malformed entry. */
static int parse_egsd(const uint8_t *rec, uint16_t rsz, struct evax_object *out)
{
    /* header rectyp[2] recsiz[2] + alignlw[4] padding -> entries start at +8. */
    size_t p = 8;
    while (p + 4 <= rsz) {
        const uint8_t *e = rec + p;
        uint16_t etyp = getl16(e);
        uint16_t esiz = getl16(e + 2);
        if (esiz < 4 || p + esiz > rsz) { set_err("EGSD entry overruns record"); return -1; }

        if (etyp == EGSD__C_PSC) {
            if (out->nsec >= EVAX_MAX_SECTIONS) { set_err("too many psects"); return -1; }
            if (esiz < PSC_NAMLNG_OFF + 1) { set_err("truncated PSC entry"); return -1; }
            struct evax_section *s = &out->sec[out->nsec];
            s->align = e[PSC_ALIGN_OFF];
            s->flags = getl16(e + PSC_FLAGS_OFF);
            s->alloc = getl32(e + PSC_ALLOC_OFF);
            uint8_t nl = e[PSC_NAMLNG_OFF];
            if ((size_t)PSC_NAMLNG_OFF + 1 + nl > esiz) { set_err("PSC name overruns entry"); return -1; }
            copy_name(s->name, e + PSC_NAMLNG_OFF + 1, nl);
            out->nsec++;
        } else if (etyp == EGSD__C_SYM) {
            if (out->nsym >= EVAX_MAX_SYMBOLS) { set_err("too many symbols"); return -1; }
            if (esiz < SYM_FLAGS_OFF + 2) { set_err("truncated SYM entry"); return -1; }
            struct evax_symbol *y = &out->sym[out->nsym];
            uint16_t flags = getl16(e + SYM_FLAGS_OFF);
            y->flags = flags;
            y->defined = (flags & EGSY__V_DEF) ? 1 : 0;
            y->is_proc = (flags & EGSY__V_NORM) ? 1 : 0;
            if (y->defined) {
                if (esiz < ESDF_NAMLNG_OFF + 1) { set_err("truncated esdf entry"); return -1; }
                y->value  = getl64(e + ESDF_VALUE_OFF);
                y->psindx = getl32(e + ESDF_PSINDX_OFF);
                if (y->is_proc) {
                    /* A normal procedure carries a distinct code entry point
                     * (code_address + ca_psindx) in addition to its descriptor
                     * value; the LINKAGE pair stores <code entry, descriptor>. */
                    y->code_value  = getl64(e + ESDF_CODEADDR_OFF);
                    y->code_psindx = getl32(e + ESDF_CAPSINDX_OFF);
                } else {
                    y->code_value  = y->value;   /* non-proc: code addr == value */
                    y->code_psindx = y->psindx;
                }
                uint8_t nl = e[ESDF_NAMLNG_OFF];
                if ((size_t)ESDF_NAMLNG_OFF + 1 + nl > esiz) { set_err("esdf name overruns"); return -1; }
                copy_name(y->name, e + ESDF_NAMLNG_OFF + 1, nl);
            } else {
                if (esiz < ESRF_NAMLNG_OFF + 1) { set_err("truncated esrf entry"); return -1; }
                y->value = 0;
                y->psindx = 0;
                y->code_value = 0;
                y->code_psindx = 0;
                uint8_t nl = e[ESRF_NAMLNG_OFF];
                if ((size_t)ESRF_NAMLNG_OFF + 1 + nl > esiz) { set_err("esrf name overruns"); return -1; }
                copy_name(y->name, e + ESRF_NAMLNG_OFF + 1, nl);
            }
            out->nsym++;
        }
        /* else: SPSC/IDC/SYMG/… — not needed to link a bare object; skip. */

        p += esiz;   /* gsdsiz includes the entry's own header + any padding */
    }
    return 0;
}

/* ---- ETIR relocation recognizer -------------------------------------------
 * ETIR records carry a stack-machine command stream that both emits the image
 * bytes and describes relocations. We recognize the relocation-producing
 * command sequences (the algorithm of binutils' alpha_vms_slurp_relocs,
 * reimplemented from the public GPL source) into a flat reloc list.
 *
 * State persists ACROSS ETIR records (notably cur_psect — the section a run of
 * relocations targets, set by CTL_SETRB and left in force until the next one).
 * `vaddr` is the running byte cursor within cur_psect; each command samples it
 * BEFORE its own advance to get the relocation's offset. Stacking commands
 * (the STA family, OPR_ADD, CTL_SETRB, STO_IMM) update state and emit nothing;
 * store commands emit one relocation and then reset the per-relocation state. */
struct etir_state {
    uint64_t       vaddr;      /* running byte offset within cur_psect       */
    int            cur_psect;  /* target section index (persists; -1 = none) */
    int            cur_psidx;  /* pending psect index from STA_PQ (-1 = none) */
    const uint8_t *cur_sym;    /* pending counted symbol name, or NULL       */
    uint64_t       cur_addend;
    int            prev_cmd;
};

/* Emit one relocation of `type` at `address` within cur_psect. Target is the
 * pending symbol (cur_sym) or, failing that, the pending section (cur_psidx).
 * `limit` bounds a counted symbol-name read (record end). */
static int etir_emit(struct evax_object *out, struct etir_state *st,
                     enum evax_reloc_type type, uint64_t address,
                     const uint8_t *limit)
{
    if (out->nreloc >= EVAX_MAX_RELOCS) { set_err("too many relocations"); return -1; }
    if (st->cur_psect < 0 || st->cur_psect >= out->nsec) {
        set_err("relocation before CTL_SETRB or bad psect index"); return -1;
    }
    struct evax_reloc *r = &out->reloc[out->nreloc];
    memset(r, 0, sizeof *r);
    r->type       = type;
    r->psect      = st->cur_psect;
    r->address    = address;
    r->addend     = st->cur_addend;
    r->to_section = -1;
    if (st->cur_sym) {
        uint8_t nl = st->cur_sym[0];
        const uint8_t *s = st->cur_sym + 1;
        if (s + nl > limit) nl = (uint8_t)(limit > s ? limit - s : 0);
        copy_name(r->sym, s, nl);
    } else if (st->cur_psidx >= 0) {
        r->to_section = st->cur_psidx;   /* section-relative (STA_PQ + STO_OFF) */
    }
    out->nreloc++;
    return 0;
}

/* Materialize `n` immediate bytes from a STO_IMM command into the current
 * psect's content buffer at the running vaddr, allocating the buffer (zeroed to
 * `alloc`) on first write. Relocation-store slots left untouched stay zero and
 * are filled by the linker when it applies the relocation (slice 4). */
static int etir_write_content(struct evax_object *out, struct etir_state *st,
                              const uint8_t *data, uint32_t n)
{
    if (st->cur_psect < 0 || st->cur_psect >= out->nsec) {
        set_err("STO_IMM before CTL_SETRB or bad psect index"); return -1;
    }
    struct evax_section *s = &out->sec[st->cur_psect];
    if (st->vaddr + n > s->alloc) {
        set_err("STO_IMM writes past psect allocation"); return -1;
    }
    if (n == 0) return 0;
    if (!s->content) {
        s->content = calloc(1, s->alloc ? (size_t)s->alloc : 1);
        if (!s->content) { set_err("oom allocating psect content"); return -1; }
    }
    memcpy(s->content + st->vaddr, data, n);
    return 0;
}

/* Parse one ETIR record (record proper at `rec`, size `rsz`) advancing `st`. */
static int parse_etir(const uint8_t *rec, uint16_t rsz, struct evax_object *out,
                      struct etir_state *st)
{
    const uint8_t *end = rec + rsz;
    const uint8_t *ptr = rec + 4;             /* skip rectyp[2] + size[2]      */
    while (ptr + 4 <= end) {
        uint16_t cmd    = getl16(ptr);
        uint16_t length = getl16(ptr + 2);    /* includes the 4-byte header    */
        if (length < 4 || ptr + length > end) { set_err("corrupt ETIR command"); return -1; }
        const uint8_t *op = ptr + 4;
        uint64_t addr = st->vaddr;            /* sampled before this command's advance */
        int emitted = 0;

        switch (cmd) {
        /* --- stacking: set state, emit nothing --- */
        case ETIR__C_STA_GBL: st->cur_sym = op; st->prev_cmd = cmd; break;
        case ETIR__C_STA_LW:
            if (length < 8)  { set_err("truncated STA_LW"); return -1; }
            st->cur_addend = getl32(op); st->prev_cmd = cmd; break;
        case ETIR__C_STA_QW:
            if (length < 12) { set_err("truncated STA_QW"); return -1; }
            st->cur_addend = getl64(op); st->prev_cmd = cmd; break;
        case ETIR__C_STA_PQ:
            if (length < 16) { set_err("truncated STA_PQ"); return -1; }
            st->cur_psidx = (int)getl32(op); st->cur_addend = getl64(op + 4);
            st->prev_cmd = cmd; break;
        case ETIR__C_OPR_ADD: st->prev_cmd = cmd; break;
        case ETIR__C_CTL_SETRB:
            st->cur_psect = st->cur_psidx; st->vaddr = st->cur_addend;
            st->cur_psidx = -1; st->cur_addend = 0; st->prev_cmd = cmd; break;
        case ETIR__C_STO_IMM: {
            if (length < 8) { set_err("truncated STO_IMM"); return -1; }
            uint32_t n = getl32(op);
            if ((size_t)8 + n > length) { set_err("STO_IMM data overruns command"); return -1; }
            if (etir_write_content(out, st, op + 4, n) < 0) return -1;
            st->vaddr += n; st->prev_cmd = cmd; break;
        }

        /* --- stores: emit one relocation --- */
        case ETIR__C_STO_LW:
            if (etir_emit(out, st, EVAX_R_REFLONG, addr, end) < 0) return -1;
            st->vaddr += 4; emitted = 1; break;
        case ETIR__C_STO_QW:
            if (etir_emit(out, st, EVAX_R_REFQUAD, addr, end) < 0) return -1;
            st->vaddr += 8; emitted = 1; break;
        case ETIR__C_STO_OFF:
            if (etir_emit(out, st, EVAX_R_REFQUAD, addr, end) < 0) return -1;
            st->vaddr += 8; emitted = 1; break;
        case ETIR__C_STO_GBL:
            st->cur_sym = op;
            if (etir_emit(out, st, EVAX_R_REFQUAD, addr, end) < 0) return -1;
            st->vaddr += 8; emitted = 1; break;
        case ETIR__C_STO_GBL_LW:
            st->cur_sym = op;
            if (etir_emit(out, st, EVAX_R_REFLONG, addr, end) < 0) return -1;
            st->vaddr += 4; emitted = 1; break;
        case ETIR__C_STO_CA:
            st->cur_sym = op;
            if (etir_emit(out, st, EVAX_R_CODEADDR, addr, end) < 0) return -1;
            st->vaddr += 8; emitted = 1; break;
        case ETIR__C_STC_LP_PSB:
            /* [u32 linkage_index][u8 namelen][name][u8 signature=0]; the reloc
             * targets the counted procedure name at op+4, addend 0. */
            if (length < 9) { set_err("truncated STC_LP_PSB"); return -1; }
            st->cur_sym = op + 4; st->cur_addend = 0;
            if (etir_emit(out, st, EVAX_R_LINKAGE, addr, end) < 0) return -1;
            st->vaddr += 16; emitted = 1; break;
        case ETIR__C_STC_NOP_GBL:
        case ETIR__C_STC_BSR_GBL:
        case ETIR__C_STC_LDA_GBL:
        case ETIR__C_STC_BOH_GBL: {
            /* 32-byte payload: [u32 lkindex][u32 psect][u64 address][u32 insn]
             * [u32 psect][u64 addend][u8 namelen][name]. address + addend come
             * from the payload (NOT vaddr); the reloc does not advance vaddr. */
            if (length < 36) { set_err("truncated STC_*_GBL"); return -1; }
            enum evax_reloc_type t =
                cmd == ETIR__C_STC_NOP_GBL ? EVAX_R_NOP :
                cmd == ETIR__C_STC_BSR_GBL ? EVAX_R_BSR :
                cmd == ETIR__C_STC_LDA_GBL ? EVAX_R_LDA : EVAX_R_BOH;
            uint64_t payload_addr = getl64(op + 8);
            st->cur_addend = getl64(op + 24);
            st->cur_sym    = op + 32;
            if (etir_emit(out, st, t, payload_addr, end) < 0) return -1;
            emitted = 1; break;
        }
        case ETIR__C_OVMX_GPDISP: {
            /* [OVMX] vms-4ed — the OVMX-private GP-displacement command. Operand
             * (all little-endian, evax_read.c framing: length includes the
             * 4-byte header, trailing bytes may be align padding and are ignored):
             *   [u32 lkidx_or_0][u32 psect][u64 ldah_vaddr][u32 lda_delta]
             *   [u8 namelen][name]  = 20 fixed + 1 + namelen bytes.
             * The command does NOT advance vaddr: the ldah/lda instruction words
             * are ordinary STO_IMM content that the OVMX linker patches in place
             * (link.c evax_apply_reloc). `name` is the enclosing procedure whose
             * PDSC offset K the linker looks up; the pair carries -K signed-split.
             * NOT VMS-authentic (EVAX has no GPDISP); see
             * docs/design-alpha-per-image-gp.md §2.2. */
            if (length < 4 + 20 + 1) { set_err("truncated OVMX_GPDISP"); return -1; }
            uint32_t psect      = getl32(op + 4);
            uint64_t ldah_vaddr = getl64(op + 8);
            uint32_t lda_delta  = getl32(op + 16);
            uint8_t  namelen    = op[20];
            const uint8_t *name = op + 21;
            if (name + namelen > ptr + length) {
                set_err("OVMX_GPDISP name overruns command"); return -1;
            }
            if (out->nreloc >= EVAX_MAX_RELOCS) {
                set_err("too many relocations"); return -1;
            }
            if ((int)psect >= out->nsec) {
                set_err("OVMX_GPDISP names a bad psect index"); return -1;
            }
            struct evax_reloc *rr = &out->reloc[out->nreloc];
            memset(rr, 0, sizeof *rr);
            rr->type       = EVAX_R_OVMX_GPDISP;
            rr->psect      = (int)psect;
            rr->address    = ldah_vaddr;
            rr->addend     = lda_delta;    /* ldah_vaddr + addend = the lda word */
            rr->to_section = -1;
            copy_name(rr->sym, name, namelen);
            out->nreloc++;
            /* No vaddr advance, no per-relocation state reset needed (this case
             * uses none of the STA/OPR stacking state). */
            break;
        }
        default:
            set_err("unknown ETIR command"); return -1;
        }

        if (emitted) {   /* reset the per-relocation state after an emit */
            st->cur_addend = 0; st->prev_cmd = -1;
            st->cur_sym = NULL; st->cur_psidx = -1;
        }
        ptr += length;
    }
    return 0;
}

int evax_read(const uint8_t *buf, size_t len, struct evax_object *out)
{
    memset(out, 0, sizeof *out);
    if (!evax_is_object(buf, len)) { set_err("not an EVAX object (first record is not EMH)"); return -1; }

    /* ETIR state persists across records (cur_psect stays in force until the
     * next CTL_SETRB), so it lives here, not inside parse_etir. */
    struct etir_state et = { 0, -1, -1, NULL, 0, -1 };

    size_t off = 0;
    while (off + 6 <= len) {
        uint16_t rtyp = getl16(buf + off + 2);
        uint16_t rsz  = getl16(buf + off + 4);
        if (rsz < 4) { set_err("record size too small"); return -1; }
        /* record proper spans [off+2, off+2+rsz); ensure it is inside the file */
        if (off + 2 + (size_t)rsz > len) { set_err("record overruns file"); return -1; }
        const uint8_t *rec = buf + off + 2;

        switch (rtyp) {
        case EOBJ__C_EMH:  parse_emh(rec, rsz, out); break;
        case EOBJ__C_EGSD: if (parse_egsd(rec, rsz, out) < 0) return -1; break;
        case EOBJ__C_ETIR: if (parse_etir(rec, rsz, out, &et) < 0) return -1; break;
        case EOBJ__C_EEOM: return 0;                 /* end of module */
        default:           /* EDBG/ETBT/… skip */    break;
        }

        size_t adv = 2 + (size_t)rsz;   /* 2-byte RMS prefix + the record proper */
        if (adv & 1) adv++;             /* records are padded to an even offset */
        off += adv;
    }
    /* A well-formed object ends at EEOM; running off the end without one is
     * tolerated (we return what we parsed) but flagged for the caller's log. */
    set_err("no EEOM record (object may be truncated)");
    return 0;
}
