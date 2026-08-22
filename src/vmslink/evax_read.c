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
#define ESDF_VALUE_OFF  8
#define ESDF_PSINDX_OFF 28
#define ESDF_NAMLNG_OFF 32
#define ESRF_NAMLNG_OFF 8

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
                uint8_t nl = e[ESDF_NAMLNG_OFF];
                if ((size_t)ESDF_NAMLNG_OFF + 1 + nl > esiz) { set_err("esdf name overruns"); return -1; }
                copy_name(y->name, e + ESDF_NAMLNG_OFF + 1, nl);
            } else {
                if (esiz < ESRF_NAMLNG_OFF + 1) { set_err("truncated esrf entry"); return -1; }
                y->value = 0;
                y->psindx = 0;
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

int evax_read(const uint8_t *buf, size_t len, struct evax_object *out)
{
    memset(out, 0, sizeof *out);
    if (!evax_is_object(buf, len)) { set_err("not an EVAX object (first record is not EMH)"); return -1; }

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
        case EOBJ__C_EEOM: return 0;                 /* end of module */
        case EOBJ__C_ETIR: /* text + relocations: a following slice */ break;
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
