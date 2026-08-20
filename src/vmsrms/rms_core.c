/*
 * rms_core.c - RMS Core File Operations
 *
 * Implements $OPEN, $CLOSE, $CREATE, $ERASE, $CONNECT, $DISCONNECT,
 * $DISPLAY, $REWIND, and $FLUSH system services.
 *
 * File metadata (record format, organization, etc.) is stored in
 * companion .rms_meta sidecar files alongside the data files.
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * Each service below works on the caller's own FAB/RAB. Since vms-bc7 the file
 * and block layer IS the executive's: file access rides the Files-11 ODS-2 ACP
 * (channel + $QIO over /dev/vms, FAB._rms_file), not a per-process POSIX fd.
 * What is still missing is cross-process RECORD locking / shared-access
 * arbitration -- RMS runs in the caller's process (as it does on VMS too), but
 * the RAB$M_ lock options do not yet reach the executive lock manager.
 *
 * THEY CITED vms-5b4 UNTIL vms-fab -- the item that BUILT this register, closed
 * when the register landed, owning none of the facades in it. vms-407 owns them
 * now, together with rms_record.c and rms_search.c. Note what it is NOT: RMS
 * running in the caller's process is what OpenVMS does too. What is missing is
 * the arbitration the process context is supposed to CALL, and vms-ci.7 already
 * built the lock manager it should call.
 *
 * FILE ACCESS NOW REACHES THE EXECUTIVE THROUGH THE Files-11 ODS-2 ACP
 * (vms-bc7, epic vms-208). sys$open/$create/$close/$erase/$extend $ASSIGN an
 * executive channel to the mounted SYS$DISK, resolve the file by name to a FID,
 * and issue the ACP $QIO file operations (IO$_ACCESS / IO$_CREATE / IO$_DEACCESS
 * / IO$_DELETE / IO$_MODIFY) over /dev/vms -- there is no longer a per-process
 * POSIX fd (FAB._linux_fd is retired). What stays this process's is the RMS
 * bookkeeping: record-attribute defaults carried on the FAB, the RAB cursor,
 * the IFI/ISI counters. So these are PARTIAL. (The netbsd-vax standalone cross
 * keeps a POSIX backend until VAX's own ACP re-target, vms-d5d.)
 *
 * OVMX-PARTIAL: sys$open (vms-bc7) -- exec: $ASSIGN the volume + IO$_ACCESS
 *     resolves the filespec by name to a FID and builds the file's VBN->LBN
 *     window (rms_acp_open_file).
 * OVMX-LOCAL: sys$open -- the FAB record-format fields and IFI are set in this
 *     process; the FAB share/access fields do not reach any arbitrator.
 * OVMX-PARTIAL: sys$create (vms-bc7) -- exec: IO$_CREATE mints a real FID from
 *     INDEXF.SYS, enters a versioned directory record, and builds a write window.
 * OVMX-LOCAL: sys$create -- the record-format/organization attributes are
 *     carried on the caller's FAB; no executive record lock is taken.
 * OVMX-PARTIAL: sys$close (vms-bc7) -- exec: IO$_DEACCESS tears down the file
 *     window and $DASSGN releases the executive channel.
 * OVMX-LOCAL: sys$close -- frees the RMS handle and clears the IFI in this
 *     process.
 * OVMX-PARTIAL: sys$erase (vms-bc7) -- exec: $ASSIGN + IO$_DELETE removes the
 *     directory entry and deallocates the file's header and blocks.
 * OVMX-LOCAL: sys$erase -- resolves the filespec fields in this process; no
 *     interlock against another accessor is taken here.
 * OVMX-PARTIAL: sys$extend (vms-bc7) -- exec: IO$_MODIFY allocates fab$l_alq
 *     more blocks (BITMAP.SYS + FH2 retrieval-pointer append) without moving EOF.
 * OVMX-LOCAL: sys$extend -- validates the caller's own FAB before the request.
 * OVMX-USERSPACE: sys$connect (vms-407) -- initializes the stream-position
 *     fields (_current_offset/_eof/_last_rec_offset/_last_rec_size/rab$w_isi)
 *     directly inside the caller's own RAB. Measured: it never allocates;
 *     the RAB's rab->_rms_stream pointer sys$disconnect frees is never set
 *     by this function or anywhere else in the tree.
 * OVMX-USERSPACE: sys$disconnect (vms-407) -- resets those same fields and
 *     frees rab->_rms_stream if non-NULL, which measurement shows is always
 *     NULL -- no code path in the tree ever assigns it.
 * OVMX-USERSPACE: sys$display (vms-407) -- fills XAB fields from FAB/handle
 *     state in this process; it issues no ACP $QIO.
 * OVMX-USERSPACE: sys$rewind (vms-407) -- repositions the RAB byte cursor
 *     (rms_io_lseek is pure cursor arithmetic; no $QIO).
 * OVMX-USERSPACE: sys$flush (vms-407) -- IO$_WRITEVBLK is already write-through,
 *     so the flush issues no $QIO (rms_io_fsync is a no-op on the ACP backend).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <time.h>
#include "rms/rms.h"
#include "rms_internal.h"
#include "rms_io.h"
#include "rms_prolog3.h"
#include "vmsfs/filespec.h"
#include "vmsfs/device.h"      /* vms-0044: compose directory/concealed logicals -> ODS-2 candidates */
#include "vmsfs/version.h"
#include "ovmx_layout.h"
#include "ssdef.h"
#include "lib$routines.h"   /* lib$cvt_vectim: Unix->VMS binary time (vms-3dd) */
/* ODS2_FK_* file-kind selectors: needed on EVERY target, not just Linux --
 * rms_open_named_handle() passes ODS2_FK_DATA (the RFM=VAR back-compat default)
 * unconditionally, and this header cross-compiles cleanly (it is the same
 * vmsfs/ods2.h the netbsd-vax module build already consumes). Keeping the
 * include Linux-only left ODS2_FK_DATA undeclared on the VAX cross-build. */
#include "vmsfs/ods2.h"     /* ODS2_FK_* file-kind selectors for IO$_CREATE    */
#if defined(__linux__)
#include "vms_kif.h"        /* vms-bc7: the Files-11 ODS-2 ACP over /dev/vms   */
#endif

/*
 * unix_time_to_vms - Convert a Unix time_t (seconds since 1970-01-01 UTC) to a
 * VMS 64-bit binary time: the count of 100-nanosecond intervals since the VMS
 * base date, 17-NOV-1858 00:00:00 (the Smithsonian Modified Julian Day epoch).
 *
 * VMS reference: VSI OpenVMS Programming Concepts Manual, Vol. I, "System Time
 * Format" -- absolute time is a signed quadword of 100ns ticks past the base
 * date 17-NOV-1858. The conversion is not done here with a hand-written magic
 * offset; it is delegated to the existing RTL converter lib$cvt_vectim
 * (src/libvms/rtl/lib_datetime.c), which owns the documented VMS_EPOCH_OFFSET
 * constant. We break the absolute Unix instant down to its UTC calendar fields
 * (gmtime_r) and feed lib$cvt_vectim a numeric time vector; lib$cvt_vectim's
 * timegm() inverts gmtime() exactly, so the same wall-clock instant round-trips.
 *
 * Returns the VMS quadword, or 0 if the instant cannot be represented (which a
 * VMS tool reads as "no date", the honest empty value).
 */
static uint64_t unix_time_to_vms(time_t t)
{
    struct tm tmv;
    if (!gmtime_r(&t, &tmv))
        return 0;

    uint16_t timvec[7];
    timvec[0] = (uint16_t)(tmv.tm_year + 1900);   /* year   */
    timvec[1] = (uint16_t)(tmv.tm_mon + 1);       /* month  */
    timvec[2] = (uint16_t)tmv.tm_mday;            /* day    */
    timvec[3] = (uint16_t)tmv.tm_hour;            /* hour   */
    timvec[4] = (uint16_t)tmv.tm_min;             /* minute */
    timvec[5] = (uint16_t)(tmv.tm_sec > 59 ? 59 : tmv.tm_sec); /* sec (drop leap) */
    timvec[6] = 0;                                /* hundredths (stat: 1s res) */

    uint64_t vms_time = 0;
    if (lib$cvt_vectim(timvec, &vms_time) != SS$_NORMAL)
        return 0;
    return vms_time;
}

/* Mutex protecting internal file/stream identifier counters */
static pthread_mutex_t rms_id_lock = PTHREAD_MUTEX_INITIALIZER;

/* Shared IFI counter for both sys$open and sys$create */
static uint16_t next_ifi = 1;

/* Protection functions from vmsfs */
extern uint16_t vmsfs_mode_to_protection(mode_t mode);
extern mode_t   vmsfs_protection_to_mode(uint16_t vms_prot);

/* Indexed-file B-tree persistence (rms_idx.c). rms_idx_cleanup saves the
 * in-memory index before freeing it so sys$close does not drop records added
 * since the last periodic save (vms-5c6d). */
extern uint32_t rms_idx_cleanup(struct FAB *fab);

/* rms_read_exact / rms_write_exact now in rms_util.c */

/* RMS metadata sidecar filename suffix */
#define RMS_SIDECAR_SUFFIX ".rms_meta"

/* Index sidecar suffix for indexed files */
#define RMS_INDEX_SUFFIX   ".rms_idx"

/*
 * Internal: RMS file metadata stored in sidecar file.
 * This persists the record format and file organization
 * so that subsequent opens can restore the correct behavior.
 */
#define RMS_META_MAGIC  0x524D5331  /* "RMS1" */
#define RMS_META_VERSION 1

/* Forward decl: rms_impl_open falls through to rms_impl_create on FAB$M_CIF,
 * which is defined later in this file. */
static uint32_t rms_impl_create(void *fab_ptr);

/* vms-5f0 legacy POSIX bodies. These are the pre-flip $OPEN/$CREATE/$ERASE
 * implementations (a Linux fd + RMS metadata sidecar over the /vms passthrough).
 * They are the sole record backend on the netbsd-vax standalone cross, and on
 * __linux__ they back the atomic-flip defer: rms_impl_open/create/erase probe
 * the executive and, only when /dev/vms is absent (SS$_NOSUCHDEV -- host ctest
 * and the plain-container self-host/link/activation gates), fall back to these,
 * exactly as IMGACT's imgsrc_open() defers (commit f2817d31). When /dev/vms IS
 * present the runtime never reaches them and RMS stays ACP-only, failing honest
 * (CLAUDE.md Rule 9 / INV-6). */
static uint32_t rms_posix_open(struct FAB *fab);
static uint32_t rms_posix_create(struct FAB *fab);
static uint32_t rms_posix_erase(struct FAB *fab);
static void rms_posix_close(struct FAB *fab, int deleting, uint32_t *close_sts);
static int rms_resolve_version(const char *path, char *out, size_t outlen);
/* rms_resolve_spec (filespec + default merge) is defined further down; the ACP
 * lifecycle helpers below use it to apply fab$l_dna defaults. */
static int rms_resolve_spec(const char *spec, const char *default_spec,
                            char *out, size_t outlen);

#if defined(__linux__)
/* ======================================================================
 * vms-bc7: Files-11 (ODS-2) ACP file lifecycle -- the product runtime.
 *
 * $OPEN/$CREATE $ASSIGN an executive channel to the mounted volume, resolve
 * the directory FID, and IO$_ACCESS / IO$_CREATE the file (building the
 * VBN->LBN window RMS record I/O then rides via rms_io_*). $CLOSE IO$_DEACCESS
 * + $DASSGN; $ERASE IO$_DELETE; $EXTEND IO$_MODIFY. No POSIX file I/O, no
 * sidecar, no vmsfs_to_linux_path -- fail-honest against a real /dev/vms
 * (SS$_NOSUCHDEV / SS$_NOSUCHFILE / SS$_NOPRIV), never a silent local success
 * (CLAUDE.md Rule 9 / INV-6).
 * ====================================================================== */

/* RMS_ACP_DEFAULT_DEV (the boot volume unit RMS $ASSIGNs when the filespec
 * names no device -- DKA0: until the discovered-SYS$DISK logical is bound) is
 * shared with rms_search.c via rms_internal.h. */

struct rms_acp_spec {
    char     devnam[16];             /* "DKA0:" (mounted unit to $ASSIGN)   */
    char     dirpath[256];           /* raw "A.B.C" inside [] (no brackets) */
    char     name[VMS_ACP_NAME_SIZE];/* "NAME.TYP" upcased                  */
    uint16_t version;                /* 0 => highest (open) / highest+1 (create) */
};

/* Number of ODS-2 search-list candidates RMS composes for one filespec
 * (SYS$SYSTEM: fans out to the node member + the SYSCOMMON member; deeper
 * concealed chains a couple more). Matches LNM_MAX_SEARCHLIST (8). */
#define RMS_ACP_MAX_CANDS 8

/* Compose the effective VMS filespec from fab$l_fna (+ fab$l_dna defaults)
 * WITHOUT resolving logical names. Returns 0 on success, -1 on empty. */
static int rms_acp_effective_spec(struct FAB *fab, char *spec, size_t speclen)
{
    if (!fab->fab$l_fna || fab->fab$b_fns == 0)
        return -1;

    {
        size_t len = fab->fab$b_fns;
        if (len >= speclen) len = speclen - 1;
        memcpy(spec, fab->fab$l_fna, len);
        spec[len] = '\0';
    }
    /* Apply a default filespec (fab$l_dna) for any missing name/type. */
    if (fab->fab$l_dna && fab->fab$b_dns > 0) {
        char dflt[1024] = "";
        char combined[1024];
        size_t dlen = fab->fab$b_dns;
        if (dlen >= sizeof(dflt)) dlen = sizeof(dflt) - 1;
        memcpy(dflt, fab->fab$l_dna, dlen);
        dflt[dlen] = '\0';
        if (rms_resolve_spec(spec, dflt, combined, sizeof(combined)) == 0) {
            strncpy(spec, combined, speclen - 1);
            spec[speclen - 1] = '\0';
        }
    }
    return 0;
}

/* Split a fully-composed VMS filespec string into device / directory /
 * name.type / version. ODS-2 is case-insensitive; the name is upcased so it
 * matches the on-disk directory records the codec decodes. A spec with no
 * device gets the DKA0: default. Returns 0 on success, -1 on a malformed /
 * empty spec. */
static int rms_acp_spec_parse(const char *spec_in, struct rms_acp_spec *s)
{
    const char *p;
    const char *lb, *rb, *colon;
    const char *spec = spec_in;

    if (!spec || !spec[0])
        return -1;

    memset(s, 0, sizeof(*s));
    strncpy(s->devnam, RMS_ACP_DEFAULT_DEV, sizeof(s->devnam) - 1);
    s->version = 0;

    p = spec;

    /* DEVICE: text before the FIRST ':' that precedes any '[' (a logical name
     * or physical unit). Store WITH the trailing ':' as $ASSIGN expects. */
    lb = strchr(spec, '[');
    colon = strchr(spec, ':');
    if (colon && (!lb || colon < lb)) {
        size_t dl = (size_t)(colon - spec) + 1;   /* include ':' */
        if (dl < sizeof(s->devnam)) {
            memcpy(s->devnam, spec, dl);
            s->devnam[dl] = '\0';
        }
        p = colon + 1;
    }

    /* DIRECTORY: text inside [ ]. */
    lb = strchr(p, '[');
    rb = lb ? strchr(lb, ']') : NULL;
    if (lb && rb && rb > lb + 1) {
        size_t dl = (size_t)(rb - lb - 1);
        if (dl >= sizeof(s->dirpath)) dl = sizeof(s->dirpath) - 1;
        memcpy(s->dirpath, lb + 1, dl);
        s->dirpath[dl] = '\0';
        p = rb + 1;
    }

    /* NAME.TYP;VER: the remainder. Split off ';version' first. */
    {
        char rest[VMS_ACP_NAME_SIZE + 16] = "";
        const char *semi;
        size_t rl;
        strncpy(rest, p, sizeof(rest) - 1);
        rest[sizeof(rest) - 1] = '\0';
        semi = strchr(rest, ';');
        if (semi) {
            long v = 0;
            const char *q = semi + 1;
            /* ';' with no digits, or ';0', => highest; else the literal N. */
            for (; *q >= '0' && *q <= '9'; q++)
                v = v * 10 + (*q - '0');
            s->version = (uint16_t)v;
            rl = (size_t)(semi - rest);
        } else {
            rl = strlen(rest);
        }
        if (rl == 0)
            return -1;                       /* no filename component */
        if (rl >= sizeof(s->name)) rl = sizeof(s->name) - 1;
        for (size_t i = 0; i < rl; i++) {
            char c = rest[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            s->name[i] = c;
        }
        s->name[rl] = '\0';
    }
    return 0;
}

/*
 * rms_acp_specs_from_fab - vms-5f0 (epic vms-208 atomic flip).
 *
 * Compose the effective filespec from the FAB, then resolve any directory /
 * concealed-rooted logical in its device field (SYS$STARTUP:, SYS$SYSTEM:,
 * SYS$LIBRARY: ...) into one or more FULLY-COMPOSED on-volume ODS-2 candidate
 * specs via vmsfs_compose_ods2_candidates() -- in search-list order (node
 * member first, SYSCOMMON member second). Each candidate is split into an
 * rms_acp_spec the ACP directory walk consumes. The RMS open/create/erase path
 * tries them in order; the first the ACP resolves wins.
 *
 * A device-less spec (no logical to resolve) has no candidates to compose, so
 * we fall back to the single naive parse with the DKA0: default -- preserving
 * the pre-logical behaviour for plain "NAME.TYP" and "DKA0:[DIR]NAME.TYP".
 *
 * Fail-honest (INV-6): an unresolvable logical chain yields no candidates and
 * returns 0 here, so the caller reports the honest RMS error (never a fabricated
 * path, never a POSIX fallback). Returns the candidate count, or -1 on a
 * malformed/empty spec.
 */
static int rms_acp_specs_from_fab(struct FAB *fab, struct rms_acp_spec *specs,
                                  int max)
{
    char spec[1024];
    if (rms_acp_effective_spec(fab, spec, sizeof(spec)) < 0)
        return -1;
    if (max <= 0)
        return -1;

    char cands[RMS_ACP_MAX_CANDS][VMSFS_MAX_FILESPEC];
    int n = vmsfs_compose_ods2_candidates(spec, cands, RMS_ACP_MAX_CANDS);

    int out = 0;
    for (int i = 0; i < n && out < max; i++) {
        if (rms_acp_spec_parse(cands[i], &specs[out]) == 0)
            out++;
    }
    if (out > 0)
        return out;

    /* No device logical composed (device-less spec, or no LNM manager):
     * parse the effective spec directly with the DKA0: default. If that spec
     * DID carry a device that simply is not a logical, rms_acp_spec_parse
     * keeps it verbatim -- same physical unit compose would have emitted. */
    if (rms_acp_spec_parse(spec, &specs[0]) == 0)
        return 1;
    return -1;
}

/* Resolve the directory named by s->dirpath to its FID by walking each
 * "COMP" as "COMP.DIR" from the MFD ([000000] == FID 4, addressed by an
 * all-zero DID). Empty/[000000] dirpath => the MFD itself (all-zero DID). */
uint32_t rms_acp_resolve_did(uint32_t chan, const char *dirpath,
                             uint16_t *dn, uint16_t *ds,
                             uint8_t *dr, uint8_t *dx)
{
    char work[256];
    char *save = NULL, *tok;

    *dn = 0; *ds = 0; *dr = 0; *dx = 0;   /* MFD */

    if (!dirpath || dirpath[0] == '\0')
        return SS$_NORMAL;
    strncpy(work, dirpath, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';
    if (strcmp(work, "000000") == 0)
        return SS$_NORMAL;

    for (tok = strtok_r(work, ".", &save); tok;
         tok = strtok_r(NULL, ".", &save)) {
        struct vms_acp_access_args a;
        uint32_t st;
        size_t tl = strlen(tok);
        char nm[VMS_ACP_NAME_SIZE];

        if (tl == 0)
            continue;
        if (tl > VMS_ACP_NAME_SIZE - 5) tl = VMS_ACP_NAME_SIZE - 5;
        for (size_t i = 0; i < tl; i++) {
            char c = tok[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            nm[i] = c;
        }
        nm[tl] = '\0';
        strncat(nm, ".DIR", sizeof(nm) - strlen(nm) - 1);

        memset(&a, 0, sizeof(a));
        a.chan    = chan;
        a.did_num = *dn; a.did_seq = *ds; a.did_rvn = *dr; a.did_nmx = *dx;
        a.version = 1;                       /* directories are version ;1 */
        strncpy(a.name, nm, VMS_ACP_NAME_SIZE - 1);

        st = vms_kif_acp_access(&a);
        if (!$VMS_STATUS_SUCCESS(st))
            return st;                        /* SS$_NOSUCHFILE etc -- honest */
        /* Chain: this directory's FID is the DID for the next component. */
        *dn = a.fid_num; *ds = a.fid_seq; *dr = a.fid_rvn; *dx = a.fid_nmx;
        /* Release the transient directory access; we only wanted its FID. */
        vms_kif_acp_deaccess(chan);
    }
    return SS$_NORMAL;
}

/* Map an ACP SS$_ status from the file-open path to the RMS $OPEN status. */
static uint32_t rms_acp_open_status(uint32_t ss)
{
    switch (ss) {
        case SS$_NOSUCHFILE: return RMS$_FNF;
        case SS$_NOPRIV:     return RMS$_PRV;
        /* No SYS$DISK mounted / no /dev/vms: RMS$_ACC (file access error, ACP)
         * -- fail-honest, never a silent local success (INV-6). */
        case SS$_NOSUCHDEV:  return RMS$_ACC;
        case SS$_DEVNOTMOUNT:return RMS$_ACC;
        default:             return RMS$_ACC;
    }
}

/* Seed the handle's cached EOF/HIBLK from an IO$_ACCESS/IO$_CREATE attr block. */
static void rms_acp_seed_handle(rms_file_t *h, const struct vms_acp_fileattr *at)
{
    uint32_t efblk = at->efblk;
    h->eof = (uint64_t)(efblk ? (efblk - 1u) : 0) * 512u + at->ffbyte;
    h->hiblk = at->hiblk;
}

/* $ASSIGN + resolve DID + IO$_ACCESS a file by name; fills *hp with a fresh
 * handle on success. want_write selects read vs write access. */
static uint32_t rms_acp_open_file(struct rms_acp_spec *s, int want_write,
                                  rms_file_t **hp)
{
    rms_file_t *h;
    struct vms_acp_access_args a;
    uint32_t chan = 0, st;

    st = vms_kif_acp_assign(s->devnam, &chan);
    if (!$VMS_STATUS_SUCCESS(st))
        return st;

    h = calloc(1, sizeof(*h));
    if (!h) { vms_kif_dassgn(chan); return SS$_INSFMEM; }
    h->chan = chan; h->assigned = 1; h->fd = -1;

    memset(&a, 0, sizeof(a));
    a.chan = chan;
    if (want_write) a.acctl = VMS_ACP_ACCTL_WRITE;
    st = rms_acp_resolve_did(chan, s->dirpath,
                             &a.did_num, &a.did_seq, &a.did_rvn, &a.did_nmx);
    if (!$VMS_STATUS_SUCCESS(st)) { free(h); vms_kif_dassgn(chan); return st; }
    a.version = s->version;
    strncpy(a.name, s->name, VMS_ACP_NAME_SIZE - 1);

    st = vms_kif_acp_access(&a);
    if (!$VMS_STATUS_SUCCESS(st)) { free(h); vms_kif_dassgn(chan); return st; }

    h->accessed = 1; h->writable = want_write ? 1 : 0;
    h->fid_num = a.fid_num; h->fid_seq = a.fid_seq;
    h->fid_rvn = a.fid_rvn; h->fid_nmx = a.fid_nmx;
    h->version = a.out_version;    /* resolved version (WRITE ACTIVE reports it) */
    rms_acp_seed_handle(h, &a.attr);
    *hp = h;
    return SS$_NORMAL;
}

/* IO$_DEACCESS + $DASSGN + free. */
static void rms_acp_close_handle(rms_file_t *h)
{
    if (!h) return;
    if (h->accessed)  vms_kif_acp_deaccess(h->chan);
    if (h->assigned)  vms_kif_dassgn(h->chan);
    free(h);
}

/*
 * rms_acp_absent - vms-5f0 executive-presence probe. A cheap $ASSIGN that comes
 * back SS$_NOSUCHDEV -- and ONLY that status -- means /dev/vms / the Files-11 ACP
 * is unreachable (host ctest, plain-container self-host/link gates); RMS then
 * defers $OPEN/$CREATE/$ERASE to its legacy POSIX bodies, exactly as IMGACT's
 * imgsrc_open() defers. Any other status means the executive IS present, so the
 * runtime stays ACP-only with no POSIX fallback (Rule 9/INV-6).
 *
 * CRITICAL (vms-03b): SS$_NOSUCHDEV is emitted here EXCLUSIVELY by the userspace
 * KIF (vms_kif_acp_assign / acp_bind_ok) when /dev/vms cannot be opened. Once
 * inside the executive, an $ASSIGN of a unit that exists but has no volume
 * mounted -- or of a unit that is not the probe's hardcoded DKA0: -- comes back
 * SS$_DEVNOTMOUNT, which falls through to "present" below. That distinction is
 * load-bearing: were an unmounted/non-DKA0: unit to report SS$_NOSUCHDEV, this
 * probe would wrongly declare the executive absent and RMS would silently read
 * the /vms POSIX passthrough -- the exact INV-6 masquerade the atomic flip
 * exists to kill. So the probe reflects /dev/vms PRESENCE, not DKA0:'s mount
 * state or existence. (The hardcoded RMS_ACP_DEFAULT_DEV as the probe unit is a
 * vms-47d device-native-naming follow-up; it is harmless HERE precisely because
 * DEVNOTMOUNT != NOSUCHDEV, but the OPEN path's DKA0: default is the real
 * vms-47d item.)
 *
 * Probed up front -- BEFORE rms_acp_specs_from_fab, whose ODS-2 candidate walk
 * needs the mounted volume and cannot resolve without the executive.
 */
static int rms_acp_absent(void)
{
    uint32_t chan = 0;
    uint32_t st = vms_kif_acp_assign(RMS_ACP_DEFAULT_DEV, &chan);
    if (st == SS$_NOSUCHDEV)     /* ONLY executive-absent defers to POSIX (vms-03b) */
        return 1;
    if ($VMS_STATUS_SUCCESS(st))
        vms_kif_dassgn(chan);
    return 0;                    /* present: success OR SS$_DEVNOTMOUNT -> ACP path */
}
#endif /* __linux__ ACP lifecycle helpers */

/*
 * rms_executive_absent - PUBLIC executive-presence probe (vms-5f0), the single
 * source DCL's host-defer shares with RMS's own $OPEN/$SEARCH defers. Returns 1
 * when /dev/vms / the Files-11 ACP is unreachable (host ctest, plain-container
 * self-host/link gates) -- DCL file commands then run their LEGACY resolver, the
 * same fall-back RMS (rms_acp_absent) and IMGACT (imgsrc_open) already take.
 * Returns 0 when the executive is present, so DCL stays ACP-only (Rule 9/INV-6).
 * On the netbsd-vax cross there is no ACP yet (vms-d5d): report absent so DCL
 * keeps its POSIX legacy path, matching RMS's own #else branches.
 */
int rms_executive_absent(void)
{
#if defined(__linux__)
    return rms_acp_absent();
#else
    return 1;
#endif
}

/*
 * rms_open_named_handle / rms_close_named_handle (vms-5f0) -- see rms_io.h.
 * RAW handle open for the binary indexed engines: ACP window when the executive
 * is present, POSIX-wrap of the resolved on-volume path when it is absent. The
 * standard FAB/RAB $OPEN cannot serve this because its host defer routes an
 * indexed org to the legacy .rms_idx sidecar (rms_impl_open), not the Prolog-3
 * file; this helper binds the genuine Prolog-3 substrate on BOTH paths.
 */
rms_file_t *rms_open_named_handle(const char *vms_spec, int want_write,
                                  int create, uint32_t *st_out)
{
    /* Back-compat default: RFM=VAR, the format every pre-vms-3a8 create used. */
    return rms_open_named_handle_kind(vms_spec, want_write, create,
                                      ODS2_FK_DATA, st_out);
}

rms_file_t *rms_open_named_handle_kind(const char *vms_spec, int want_write,
                                       int create, unsigned kind,
                                       uint32_t *st_out)
{
    uint32_t st;
    if (st_out) *st_out = RMS$_FAB;
    if (!vms_spec || !*vms_spec)
        return NULL;

#if defined(__linux__)
    if (!rms_acp_absent()) {
        struct FAB fab;
        struct rms_acp_spec specs[RMS_ACP_MAX_CANDS];
        rms_file_t *h = NULL;
        int ncand, i;

        memset(&fab, 0, sizeof(fab));
        fab.fab$b_bid = FAB$C_BID;
        fab.fab$l_fna = (char *)vms_spec;
        fab.fab$b_fns = (uint8_t)strlen(vms_spec);
        ncand = rms_acp_specs_from_fab(&fab, specs, RMS_ACP_MAX_CANDS);
        if (ncand < 0) { if (st_out) *st_out = RMS$_SYN; return NULL; }

        if (!create) {
            st = SS$_NOSUCHFILE;
            for (i = 0; i < ncand; i++) {
                st = rms_acp_open_file(&specs[i], want_write, &h);
                if ($VMS_STATUS_SUCCESS(st))
                    break;
            }
            if (!$VMS_STATUS_SUCCESS(st)) {
                if (st_out) *st_out = rms_acp_open_status(st);
                return NULL;
            }
            if (st_out) *st_out = RMS$_NORMAL;
            return h;
        }

        /* create/supersede: IO$_CREATE(+ACCESS) a fresh versioned data file over
         * the ACP; the caller (sysuaf_rms_create) authors the Prolog-3 image on
         * top. (rms_impl_create rejects org==IDX up front, but a plain-data
         * IO$_CREATE + an on-top Prolog-3 author is exactly how a keyed file is
         * born on real RMS.)
         *
         * vms-5f0: try EACH ODS-2 candidate directory, exactly as the read loop
         * above does -- a concealed-rooted logical (SYS$SYSTEM:, SYS$STARTUP:,
         * ...) expands to several {device, dirpath} candidates, and only one
         * resolves on the mounted volume. The old code tried only specs[0], so
         * SYSGEN WRITE CURRENT of SYS$SYSTEM:OVMXVMSSYS.PAR failed RMS$_DNF even
         * though the read of the same spec resolved. Create in the FIRST
         * candidate whose directory resolves -- the same order the read walks,
         * so a new version lands in the directory the existing versions live in. */
        {
            struct vms_acp_fileop_args fop;
            uint32_t chan = 0;
            uint32_t last = SS$_NOSUCHFILE;
            int ci;

            h = NULL;
            for (ci = 0; ci < ncand; ci++) {
                struct rms_acp_spec *sp = &specs[ci];
                uint32_t d_num = 0, d_seq = 0;
                uint8_t  d_rvn = 0, d_nmx = 0;

                st = vms_kif_acp_assign(sp->devnam, &chan);
                if (!$VMS_STATUS_SUCCESS(st)) { last = st; chan = 0; continue; }

                st = rms_acp_resolve_did(chan, sp->dirpath, &d_num, &d_seq,
                                         &d_rvn, &d_nmx);
                if (!$VMS_STATUS_SUCCESS(st)) {
                    vms_kif_dassgn(chan); chan = 0; last = st; continue;
                }

                h = calloc(1, sizeof(*h));
                if (!h) { vms_kif_dassgn(chan); if (st_out) *st_out = RMS$_DME; return NULL; }
                h->chan = chan; h->assigned = 1; h->fd = -1;

                memset(&fop, 0, sizeof(fop));
                fop.chan      = chan;
                fop.func      = VMS_ACP_FOP_CREATE;
                fop.modifiers = VMS_ACP_M_CREATE | VMS_ACP_M_ACCESS;
                fop.acctl     = VMS_ACP_ACCTL_WRITE;
                fop.kind      = kind;       /* vms-3a8: caller-chosen RFM (VAR default) */
                fop.did_num = d_num; fop.did_seq = d_seq;
                fop.did_rvn = d_rvn; fop.did_nmx = d_nmx;
                fop.version = 0;            /* highest existing + 1 */
                strncpy(fop.name, sp->name, VMS_ACP_NAME_SIZE - 1);

                st = vms_kif_acp_fileop(&fop);
                if (!$VMS_STATUS_SUCCESS(st)) {
                    free(h); h = NULL; vms_kif_dassgn(chan); chan = 0; last = st;
                    continue;
                }

                h->accessed = 1; h->writable = 1;
                h->fid_num = fop.fid_num; h->fid_seq = fop.fid_seq;
                h->fid_rvn = fop.fid_rvn; h->fid_nmx = fop.fid_nmx;
                h->version = fop.out_version;   /* the version the ACP minted */
                h->eof   = (uint64_t)(fop.new_efblk ? (fop.new_efblk - 1u) : 0) * 512u
                           + fop.new_ffbyte;
                h->hiblk = fop.new_hiblk;
                if (st_out) *st_out = RMS$_CREATED;
                return h;
            }

            if (st_out)
                *st_out = $VMS_STATUS_SUCCESS(last) ? RMS$_DNF
                                                   : rms_acp_open_status(last);
            return NULL;
        }
    }
#endif /* __linux__ */

    /* POSIX defer (executive absent / non-linux). NOT the runtime path
     * (Rule 9/INV-6): reached only when /dev/vms is unreachable. */
    {
        char linux_path[1024];
        int flags, fd;
        rms_file_t *h;

        if (!$VMS_STATUS_SUCCESS(vmsfs_to_linux_path(vms_spec, linux_path,
                                                     sizeof(linux_path)))) {
            if (st_out) *st_out = RMS$_SYN;
            return NULL;
        }
        if (create)          flags = O_RDWR | O_CREAT | O_TRUNC;
        else if (want_write) flags = O_RDWR;
        else                 flags = O_RDONLY;
        fd = open(linux_path, flags, 0600);
        if (fd < 0) {
            if (st_out) *st_out = (errno == ENOENT) ? RMS$_FNF : RMS$_ACC;
            return NULL;
        }
        h = rms_io_posix_wrap(fd);
        if (!h) { close(fd); if (st_out) *st_out = RMS$_DME; return NULL; }
        if (st_out) *st_out = create ? RMS$_CREATED : RMS$_NORMAL;
        return h;
    }
}

void rms_close_named_handle(rms_file_t *h)
{
    if (!h)
        return;
#if defined(__linux__)
    if (h->fd < 0) {              /* ACP handle: deaccess + dassgn + free */
        rms_acp_close_handle(h);
        return;
    }
#endif
    rms_io_posix_unwrap(h);       /* POSIX handle: close(fd) + free */
}

/*
 * rms_posix_file_attr - legacy passthrough attribute lookup (vms-5f0): stat()
 * the resolved Linux path. The netbsd-vax cross's sole implementation, and the
 * __linux__ executive-absent defer. No genuine FID; record format is not on
 * disk here. A VMS directory "DEV:[p]C.DIR" maps (via vmsfs_to_linux_path) to
 * the Linux directory that backs it, so is_directory is reported correctly.
 */
static uint32_t rms_posix_file_attr(const char *vmsspec, struct rms_fileattr *out)
{
    char linux_path[1024];
    struct stat sbuf;
    if (!$VMS_STATUS_SUCCESS(vmsfs_to_linux_path(vmsspec, linux_path,
                                                 sizeof(linux_path))))
        return RMS$_SYN;

    /* Resolve the VMS version against what is on disk (vms-5f0). The /vms
     * passthrough stores each version as a literal "name.type;N" file, so a
     * version-less spec (e.g. RUN's HELLO.EXE probe) maps via vmsfs_to_linux_path
     * to a bare "name.type" that does NOT exist -- the real file is
     * "name.type;1". Without this, RUN of a freshly LINKed image failed
     * %DCL-E-IVIMAGE even though the image was right there under its ";1" name
     * (BUILD.COM S3.2). Only as a FALLBACK when the exact path is absent, and
     * never for a "name.dir" directory probe (whose own suffix-strip retry runs
     * below): find the highest existing "name.type;N" and stat that instead.
     * rms_resolve_version() honours an explicit ";N" and otherwise picks the
     * highest. */
    if (stat(linux_path, &sbuf) != 0) {
        size_t ln = strlen(linux_path);
        int is_dirfile = (ln > 4 && (strcmp(linux_path + ln - 4, ".dir") == 0 ||
                                     strcmp(linux_path + ln - 4, ".DIR") == 0));
        if (!is_dirfile) {
            char vresolved[1024];
            if (rms_resolve_version(linux_path, vresolved, sizeof(vresolved)) == 0 &&
                stat(vresolved, &sbuf) == 0) {
                strncpy(linux_path, vresolved, sizeof(linux_path) - 1);
                linux_path[sizeof(linux_path) - 1] = '\0';
            }
        }
    }

    if (stat(linux_path, &sbuf) != 0) {
        /*
         * A VMS directory "DEV:[p]C.DIR" translates to the FILENAME
         * ".../c.dir", but on the passthrough a VMS directory IS a real Linux
         * directory ".../c" (no .dir suffix). When the .dir file itself is
         * absent, retry the suffix-stripped path as a directory so SET DEFAULT's
         * dir-existence probe (set_default_dir_exists -> rms_file_attr on the
         * .DIR file) resolves exactly as it did pre-flip (vms-5f0).
         */
        size_t n = strlen(linux_path);
        if (n > 4 && (strcmp(linux_path + n - 4, ".dir") == 0 ||
                      strcmp(linux_path + n - 4, ".DIR") == 0)) {
            char dirpath[1024];
            memcpy(dirpath, linux_path, n - 4);
            dirpath[n - 4] = '\0';
            if (stat(dirpath, &sbuf) == 0 && S_ISDIR(sbuf.st_mode)) {
                out->is_directory = 1;
                out->rfm = FAB$C_STMLF;
                return RMS$_NORMAL;
            }
        }
        return RMS$_FNF;
    }
    out->efblk = (uint32_t)((sbuf.st_size + 511) / 512) + 1u;
    out->hiblk = (uint32_t)((sbuf.st_size + 511) / 512);
    out->ffbyte = (uint16_t)(sbuf.st_size % 512);
    out->is_directory = S_ISDIR(sbuf.st_mode) ? 1 : 0;
    out->rfm = FAB$C_STMLF;
    return RMS$_NORMAL;
}

/*
 * rms_file_attr - genuine ODS-2 header attributes for a filespec (vms-481).
 * The DCL DIRECTORY /FULL + F$FILE_ATTRIBUTES source of truth: it reaches the
 * on-disk header through the ACP (IO$_ACCESS reads the ATR list), never stat().
 */
uint32_t rms_file_attr(const char *vmsspec, struct rms_fileattr *out)
{
    if (!vmsspec || !out)
        return RMS$_FAB;
    memset(out, 0, sizeof(*out));

#if defined(__linux__)
    /* ATOMIC-FLIP DEFER (vms-5f0): executive absent => legacy POSIX stat. */
    if (rms_acp_absent())
        return rms_posix_file_attr(vmsspec, out);

    struct FAB tfab = cc$rms_fab;
    struct rms_acp_spec specs[RMS_ACP_MAX_CANDS];
    struct vms_acp_access_args a;
    uint32_t chan = 0, st = SS$_NOSUCHFILE;
    int ncand, got = 0;

    tfab.fab$l_fna = (char *)vmsspec;
    tfab.fab$b_fns = (uint8_t)strlen(vmsspec);
    /* vms-5f0: resolve directory/concealed logicals (SYS$SYSTEM:X for a
     * DIRECTORY/FULL or F$FILE_ATTRIBUTES) and read the header from the first
     * search-list member the ACP resolves. */
    ncand = rms_acp_specs_from_fab(&tfab, specs, RMS_ACP_MAX_CANDS);
    if (ncand < 0)
        return RMS$_SYN;

    for (int i = 0; i < ncand && !got; i++) {
        struct rms_acp_spec *sp = &specs[i];
        chan = 0;
        st = vms_kif_acp_assign(sp->devnam, &chan);
        if (!$VMS_STATUS_SUCCESS(st))
            continue;
        memset(&a, 0, sizeof(a));
        a.chan = chan;                   /* read access (acctl 0) */
        st = rms_acp_resolve_did(chan, sp->dirpath,
                                 &a.did_num, &a.did_seq, &a.did_rvn, &a.did_nmx);
        if (!$VMS_STATUS_SUCCESS(st)) { vms_kif_dassgn(chan); continue; }
        a.version = sp->version;
        strncpy(a.name, sp->name, VMS_ACP_NAME_SIZE - 1);

        st = vms_kif_acp_access(&a);
        if (!$VMS_STATUS_SUCCESS(st)) { vms_kif_dassgn(chan); continue; }
        got = 1;
    }
    if (!got)
        return rms_acp_open_status(st);

    out->fid_num = a.fid_num; out->fid_seq = a.fid_seq;
    out->fid_rvn = a.fid_rvn; out->fid_nmx = a.fid_nmx;
    out->version = a.out_version;
    out->efblk   = a.attr.efblk;
    out->hiblk   = a.attr.hiblk;
    out->ffbyte  = a.attr.ffbyte;
    out->fileprot   = a.attr.fileprot;
    out->uic_group  = a.attr.uic_group;
    out->uic_member = a.attr.uic_member;
    out->is_directory = (a.attr.filechar & 0x2000u) ? 1 : 0;   /* FCH$V_DIRECTORY */
    memcpy(out->credate, a.attr.credate, 8);
    memcpy(out->revdate, a.attr.revdate, 8);
    /* Record format from the FAT (ATR$C_RECATTR, verbatim). fat_rtype's small
     * integers ARE the FAB$C_* record-format codes (1=FIX..6=STMCR). */
    {
        const struct ods2_recattr *fat = (const struct ods2_recattr *)a.attr.recattr;
        out->rfm = fat->fat_rtype;
        out->rat = fat->fat_rattrib;
        out->mrs = fat->fat_rsize;
    }

    vms_kif_acp_deaccess(chan);
    vms_kif_dassgn(chan);
    return RMS$_NORMAL;
#else
    /* netbsd-vax standalone cross: no ACP yet (vms-d5d) -- passthrough stat. */
    return rms_posix_file_attr(vmsspec, out);
#endif
}

struct rms_metadata {
    uint32_t magic;         /* RMS_META_MAGIC */
    uint8_t  version;       /* Metadata format version */
    uint8_t  org;           /* File organization */
    uint8_t  rfm;           /* Record format */
    uint8_t  rat;           /* Record attributes */
    uint16_t mrs;           /* Maximum record size */
    uint8_t  fsz;           /* Fixed control area size (VFC) */
    uint8_t  reserved1;
    uint32_t mrn;           /* Maximum record number (relative) */
    uint8_t  num_keys;      /* Number of keys (indexed) */
    uint8_t  reserved2[3];
    /* Key definitions follow for indexed files */
};

/* Per-key metadata stored in the sidecar */
struct rms_key_meta {
    uint8_t  ref;           /* Key of reference */
    uint8_t  dtp;           /* Data type */
    uint16_t flg;           /* Flags */
    uint8_t  nseg;          /* Number of segments */
    uint8_t  reserved[3];
    uint16_t pos[8];        /* Segment positions */
    uint8_t  siz[8];        /* Segment sizes */
};

/*
 * Local helper functions bridging RMS needs to vmsfs APIs.
 */

/* Strip version (";N") suffix from a filename */
static int rms_strip_version(const char *filename, char *out, size_t outlen)
{
    const char *semi = strrchr(filename, ';');
    size_t len = semi ? (size_t)(semi - filename) : strlen(filename);
    if (len >= outlen) len = outlen - 1;
    memcpy(out, filename, len);
    out[len] = '\0';
    return 0;
}

/*
 * rms_validate_path_boundary - Ensure a resolved path stays within VMS root.
 *
 * Canonicalizes the path with realpath() and verifies it starts with
 * SYSDISK_MOUNT ("/vms"). For paths to files that don't yet exist,
 * canonicalizes the parent directory instead.
 *
 * Returns 0 if the path is within the VMS root, -1 if it escapes.
 */
/*
 * rms_sysdisk_root - the boundary root a resolved path must stay within.
 *
 * It is wherever SYSDISK (DKA0) is ACTUALLY mounted -- the same device-table
 * entry vmsfs_to_linux_path() translates through -- so a caller that remaps
 * DKA0: to a private root (a test's mkdtemp namespace) is honoured exactly as
 * the runtime's /vms is. Falls back to the compile-time SYSDISK_MOUNT when the
 * device is not registered (before the device table is bootstrapped). This is
 * NOT a weakening: the path is still confined to the one registered SYSDISK
 * mount; it just reads that mount from the table instead of hardcoding /vms.
 */
static void rms_sysdisk_root(char *out, size_t outsz)
{
    if (!$VMS_STATUS_SUCCESS(vmsfs_device_resolve(SYSDISK_DEVICE, out, outsz)) ||
        out[0] == '\0') {
        strncpy(out, SYSDISK_MOUNT, outsz - 1);
        out[outsz - 1] = '\0';
    }
}

static int rms_validate_path_boundary(const char *path)
{
    if (!path || !path[0])
        return -1;

    /* Only validate absolute paths under the SYSDISK root */
    if (path[0] != '/')
        return 0;

    char root[PATH_MAX];
    rms_sysdisk_root(root, sizeof(root));
    size_t root_len = strlen(root);

    char resolved[PATH_MAX];
    if (realpath(path, resolved) != NULL) {
        /* File exists — check the canonical path */
        if (strncmp(resolved, root, root_len) != 0)
            return -1;
        /* Ensure it's actually under <root> and not just <root>XYZ */
        if (resolved[root_len] != '\0' && resolved[root_len] != '/')
            return -1;
        return 0;
    }

    /*
     * File doesn't exist yet (e.g., $CREATE) — canonicalize the parent
     * directory and verify it's within the SYSDISK root.
     */
    char pathcopy[PATH_MAX];
    strncpy(pathcopy, path, sizeof(pathcopy) - 1);
    pathcopy[sizeof(pathcopy) - 1] = '\0';

    /* Find last slash to get parent directory */
    char *last_slash = strrchr(pathcopy, '/');
    if (!last_slash || last_slash == pathcopy) {
        /* Root-level path or no slash — not under the SYSDISK root */
        return -1;
    }
    *last_slash = '\0';

    if (realpath(pathcopy, resolved) != NULL) {
        if (strncmp(resolved, root, root_len) != 0)
            return -1;
        if (resolved[root_len] != '\0' && resolved[root_len] != '/')
            return -1;
        return 0;
    }

    /* Parent doesn't exist either — reject */
    return -1;
}

/* Get next version number for a file in a directory */
static int rms_next_version(const char *dir, const char *basename_noversion)
{
    /* Split basename into name and ext for vmsfs API */
    char name[256], ext[64] = "";
    strncpy(name, basename_noversion, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    char *dot = strrchr(name, '.');
    if (dot) {
        /* Strip leading dot — vmsfs API expects ext without dot */
        const char *ext_no_dot = (dot[1] != '\0') ? dot + 1 : "";
        strncpy(ext, ext_no_dot, sizeof(ext) - 1);
        ext[sizeof(ext) - 1] = '\0';
        *dot = '\0';
    }
    int highest = vmsfs_get_highest_version(dir, name, ext);
    return (highest > 0) ? highest + 1 : 1;
}

/* Resolve version of a full path: find highest existing version.
 * If found, write resolved path to out buffer. Returns 0 on success. */
static int rms_resolve_version(const char *path, char *out, size_t outlen)
{
    char dir[1024] = ".";
    char base[256];
    const char *slash = strrchr(path, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - path);
        if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
        memcpy(dir, path, dlen);
        dir[dlen] = '\0';
        strncpy(base, slash + 1, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
    } else {
        strncpy(base, path, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
    }

    /* Honor an EXPLICIT positive version ";N" (vms-5f0). The caller asked for a
     * specific version -- e.g. TYPE FOO.TXT;1 -- so it must NOT be overridden
     * with the highest existing version below. Only a bare name, ";", or ";0"
     * means "highest". Strictly additive: fires only when a positive ";N" is
     * present AND that exact on-disk file exists (the /vms passthrough stores
     * each version as a literal "name;N" file); otherwise the highest-version
     * resolution runs unchanged. */
    {
        const char *semi = strrchr(base, ';');
        if (semi && semi[1]) {
            char *endp = NULL;
            long ev = strtol(semi + 1, &endp, 10);
            if (endp && *endp == '\0' && ev >= 1) {
                struct stat est;
                if (stat(path, &est) == 0) {
                    strncpy(out, path, outlen - 1);
                    out[outlen - 1] = '\0';
                    return 0;
                }
            }
        }
    }

    /* Strip existing version to get name.ext */
    char noversion[256];
    rms_strip_version(base, noversion, sizeof(noversion));
    char name[256], ext[64] = "";
    strncpy(name, noversion, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    char *dot = strrchr(name, '.');
    if (dot) {
        /* Strip leading dot — vmsfs API expects ext without dot */
        const char *ext_no_dot = (dot[1] != '\0') ? dot + 1 : "";
        strncpy(ext, ext_no_dot, sizeof(ext) - 1);
        ext[sizeof(ext) - 1] = '\0';
        *dot = '\0';
    }
    int ver = vmsfs_get_highest_version(dir, name, ext);
    if (ver < 1) ver = 1;
    char candidate[1024];
    if (ext[0]) {
        snprintf(candidate, sizeof(candidate), "%s/%s.%s;%d", dir, name, ext, ver);
    } else {
        snprintf(candidate, sizeof(candidate), "%s/%s;%d", dir, name, ver);
    }

    /* OVMX (vms-4ba.5): vmsfs_get_highest_version() treats a plain,
     * unversioned on-disk file as "version 1" (see its own doc comment), but
     * that only tells us the VERSION NUMBER to use — it does not mean a file
     * literally named "name;1" exists. Every file sys$create() itself
     * produces DOES carry a literal ";N" suffix on disk (see sys$create
     * below), so this candidate always resolves correctly for RMS-created
     * files. But for a file that was authored OUTSIDE RMS with no version
     * syntax at all (e.g. a .c source someone just wrote), the synthesized
     * "name;1" candidate does not exist and open() would wrongly fail with
     * RMS$_FNF even though the file is right there under its plain name.
     * Surfaced by vms-4ba.5 (TCC.EXE reading its own hello.c via RMS) — a
     * general RMS gap, not tcc-specific. Fix: if the versioned candidate
     * isn't real but the plain unversioned name is, resolve to the plain
     * name instead of a synthesized version that doesn't exist. This is
     * strictly additive: RMS-created files (which always have both a real
     * ";N" candidate AND, at version 1, no separate unversioned copy) are
     * unaffected — the candidate stat() succeeds and this branch is
     * skipped. */
    struct stat st;
    if (stat(candidate, &st) != 0) {
        char plain[1024];
        if (ext[0]) {
            snprintf(plain, sizeof(plain), "%s/%s.%s", dir, name, ext);
        } else {
            snprintf(plain, sizeof(plain), "%s/%s", dir, name);
        }
        if (stat(plain, &st) == 0) {
            strncpy(out, plain, outlen - 1);
            out[outlen - 1] = '\0';
            return 0;
        }
    }

    strncpy(out, candidate, outlen - 1);
    out[outlen - 1] = '\0';
    return 0;
}

/* Merge a filespec with a default spec (simple: just use the spec as-is
 * if it's complete, otherwise prefix with default_spec's directory). */
static int rms_resolve_spec(const char *spec, const char *default_spec,
                            char *result, size_t resultlen)
{
    (void)default_spec;
    strncpy(result, spec, resultlen - 1);
    result[resultlen - 1] = '\0';
    return 0;
}

/*
 * rms_check_protection() -- DELETED, NOT REPLACED (vms-2b8, operator ruling
 * 2026-07-31). It called vms$check_access(), a userspace second opinion on
 * file protection computed from the same st_mode the real enforcer already
 * decides from. It could only produce a FALSE denial (it has no notion of
 * SYSPRV/BYPASS/READALL, so it could refuse what the executive would grant)
 * and could never produce a false grant that mattered, because on the
 * bootable runtime the actual decision is made by
 * src/kernel/vmsfs/vmsfs_blkdev.c through the Linux DAC bits vmsfs derives
 * from the VMS protection mask -- measured directly: rebuilding this
 * function to return 1 unconditionally changed nothing about the failure it
 * was thought to gate, and chmod 0777 over the whole [SYS0] tree at the
 * Linux layer did not let an unprivileged account write SYS$SYSTEM: on the
 * real runtime either. See sys_security.c's comment at the deleted
 * vms$check_access() for the full account.
 *
 * The three call sites below (sys$open's read/write pre-check and sys$erase's
 * delete pre-check) are removed with it. What enforces protection now is the
 * open()/unlink() a few lines later in each function, exactly as it already
 * did for every DCL command (COPY/TYPE/DELETE) that bypasses RMS via
 * fopen()/unlink() directly -- RMS and DCL now share one enforcer instead of
 * RMS adding a second, weaker one in front of it. EACCES/EPERM from that
 * real syscall already maps to RMS$_PRV a few lines below in both functions;
 * no new error path is needed.
 */

/*
 * rms_get_default_protection - Get default protection for new files.
 * Reads VMS_DEFAULT_PROTECTION env var, or returns S:RWED,O:RWED (0xFF00).
 */
static uint16_t rms_get_default_protection(void)
{
    /* Default: S:RWED,O:RWED,G:,W: = system and owner full, group and world none
     * In VMS bit encoding: S=0x00 (all allowed), O=0x00, G=0x0F (all denied), W=0x0F
     * = 0xFF00 */
    return 0xFF00;
}

/*
 * resolve_filename - Extract and resolve the filename from a FAB.
 *
 * Parses the filename from fab$l_fna, resolves VMS filespecs
 * to Linux paths, and resolves file versions. The result is
 * stored in fab->_resolved_path.
 *
 * Returns 0 on success, -1 on failure.
 */
static int resolve_filename(struct FAB *fab)
{
    char spec[1024] = "";

    if (fab->fab$l_fna && fab->fab$b_fns > 0) {
        size_t len = fab->fab$b_fns;
        if (len >= sizeof(spec)) len = sizeof(spec) - 1;
        memcpy(spec, fab->fab$l_fna, len);
        spec[len] = '\0';
    } else {
        return -1;
    }

    /* Apply defaults from fab$l_dna if present */
    if (fab->fab$l_dna && fab->fab$b_dns > 0) {
        char default_spec[1024] = "";
        size_t dlen = fab->fab$b_dns;
        if (dlen >= sizeof(default_spec)) dlen = sizeof(default_spec) - 1;
        memcpy(default_spec, fab->fab$l_dna, dlen);
        default_spec[dlen] = '\0';

        char combined[1024];
        if (rms_resolve_spec(spec, default_spec, combined, sizeof(combined)) == 0) {
            strncpy(spec, combined, sizeof(spec) - 1);
            spec[sizeof(spec) - 1] = '\0';
        }
    }

    /* Check if it's a VMS filespec (contains : or [ — true VMS indicators).
     * A lone ';' (version number) is handled by the VMS translation path
     * but does NOT indicate a user-supplied VMS filespec for security purposes. */
    int is_vms_spec = (strchr(spec, ':') != NULL || strchr(spec, '[') != NULL);
    if (is_vms_spec || strchr(spec, ';')) {
        char linux_path[1024];
        /*
         * vmsfs_to_linux_path() returns a VMS status code (SS$_NORMAL = 1 on
         * success, per its own doc comment and $VMS_STATUS_SUCCESS: odd =
         * success) -- NOT 0-on-success. This check used to read `== 0`,
         * which is an EVEN (failure) value that the function never actually
         * returns for a real error either (its failures are also VMS status
         * codes, e.g. SS$_BADPARAM/SS$_NOSUCHDEV, all even). So this branch
         * ALWAYS took the "not translated" fallback below and treated the
         * raw VMS spec string ("SYS$SCRATCH:FOO.DAT") as a literal Linux
         * path -- relative, since it does not start with '/' -- resolving
         * against the process's cwd instead of the real /vms/... location.
         * For a SYSTEM session whose cwd is not writable, every VMS-spec
         * candidate (SYS$SCRATCH:, SYS$LOGIN:, DKA0:[USERS], SYS$SYSDEVICE:
         * [SYSTMP]) failed open() with EACCES for that reason -- not a
         * directory-permission problem at all -- which is exactly why
         * PARTS's sys$create() kept landing on its last, Unix-path fallback
         * candidate ("/tmp/PARTS.DAT") instead of any VMS filespec (vms-221).
         * A plain open() call using vmsfs_to_linux_path() directly (as
         * vms-e5c's own positive control does) never went through this
         * broken check, which is why it always succeeded while sys$create()
         * did not. sys$open() shares this same resolve_filename() and had
         * the identical defect.
         */
        if ($VMS_STATUS_SUCCESS(vmsfs_to_linux_path(spec, linux_path, sizeof(linux_path)))) {
            strncpy(fab->_resolved_path, linux_path,
                    sizeof(fab->_resolved_path) - 1);
            fab->_resolved_path[sizeof(fab->_resolved_path) - 1] = '\0';
        } else {
            strncpy(fab->_resolved_path, spec,
                    sizeof(fab->_resolved_path) - 1);
            fab->_resolved_path[sizeof(fab->_resolved_path) - 1] = '\0';
        }
        /* Security: verify VMS-translated paths stay within VMS root.
         * Only enforce boundary for true VMS filespecs (with : or [),
         * not for paths that just have a version number (;N). */
        if (is_vms_spec && fab->_resolved_path[0] == '/' &&
            rms_validate_path_boundary(fab->_resolved_path) != 0) {
            fab->_resolved_path[0] = '\0';
            return -1;
        }
    } else {
        /* Raw Linux path (no VMS translation) — pass through without
         * boundary check since this is a direct path, not user-supplied
         * VMS filespec that could contain traversal tricks. */
        strncpy(fab->_resolved_path, spec,
                sizeof(fab->_resolved_path) - 1);
        fab->_resolved_path[sizeof(fab->_resolved_path) - 1] = '\0';
    }

    return 0;
}

/*
 * resolve_for_open - Resolve the filename for open, including version.
 * Returns 0 on success.
 */
static int resolve_for_open(struct FAB *fab)
{
    if (resolve_filename(fab) < 0) return -1;

    /* Resolve version (;0 or unspecified -> highest existing) */
    char resolved[1024];
    if (rms_resolve_version(fab->_resolved_path, resolved,
                              sizeof(resolved)) == 0) {
        strncpy(fab->_resolved_path, resolved,
                sizeof(fab->_resolved_path) - 1);
        fab->_resolved_path[sizeof(fab->_resolved_path) - 1] = '\0';
    }

    return 0;
}

/*
 * load_metadata - Load RMS metadata from the sidecar file.
 *
 * If the sidecar exists, loads record format, organization,
 * MRS, etc. into the FAB. If no sidecar exists, the FAB
 * retains its current settings (which may be defaults).
 */
static void load_metadata(struct FAB *fab)
{
    char sidecar[1088];
    snprintf(sidecar, sizeof(sidecar), "%s%s",
             fab->_resolved_path, RMS_SIDECAR_SUFFIX);

    FILE *f = fopen(sidecar, "rb");
    if (!f) return;

    struct rms_metadata meta;
    if (fread(&meta, sizeof(meta), 1, f) == 1) {
        if (meta.magic == RMS_META_MAGIC && meta.version == RMS_META_VERSION) {
            fab->fab$b_org = meta.org;
            fab->fab$b_rfm = meta.rfm;
            fab->fab$w_mrs = meta.mrs;
            fab->fab$l_mrn = meta.mrn;
            fab->fab$b_rat = meta.rat;
            fab->fab$b_fsz = meta.fsz;

            /* Load key definitions into XAB chain if indexed file */
            if (meta.org == FAB$C_IDX && meta.num_keys > 0 &&
                fab->fab$l_xab != NULL) {
                struct XABKEY *xab = fab->fab$l_xab;
                for (uint8_t k = 0; k < meta.num_keys && xab; k++) {
                    if (xab->xab$b_cod != XAB$C_KEY) break;
                    struct rms_key_meta kmeta;
                    if (fread(&kmeta, sizeof(kmeta), 1, f) != 1) break;
                    xab->xab$b_ref = kmeta.ref;
                    xab->xab$b_dtp = kmeta.dtp;
                    xab->xab$w_flg = kmeta.flg;
                    xab->xab$b_nseg = kmeta.nseg;
                    xab->xab$w_pos0 = kmeta.pos[0];
                    xab->xab$w_pos1 = kmeta.pos[1];
                    xab->xab$w_pos2 = kmeta.pos[2];
                    xab->xab$w_pos3 = kmeta.pos[3];
                    xab->xab$w_pos4 = kmeta.pos[4];
                    xab->xab$w_pos5 = kmeta.pos[5];
                    xab->xab$w_pos6 = kmeta.pos[6];
                    xab->xab$w_pos7 = kmeta.pos[7];
                    xab->xab$b_siz0 = kmeta.siz[0];
                    xab->xab$b_siz1 = kmeta.siz[1];
                    xab->xab$b_siz2 = kmeta.siz[2];
                    xab->xab$b_siz3 = kmeta.siz[3];
                    xab->xab$b_siz4 = kmeta.siz[4];
                    xab->xab$b_siz5 = kmeta.siz[5];
                    xab->xab$b_siz6 = kmeta.siz[6];
                    xab->xab$b_siz7 = kmeta.siz[7];
                    /* Compute total key size */
                    uint16_t tks = 0;
                    const uint8_t *szp = &kmeta.siz[0];
                    for (uint8_t s = 0; s < kmeta.nseg && s < 8; s++) {
                        tks += szp[s];
                    }
                    xab->xab$w_tks = tks;
                    xab = (struct XABKEY *)xab->xab$l_nxt;
                }
            }
        }
    }
    fclose(f);
}

/*
 * save_metadata - Save RMS metadata to the sidecar file.
 *
 * Writes the current FAB settings to the companion .rms_meta file.
 * For indexed files, also writes key definitions from the XAB chain.
 */
static void save_metadata(struct FAB *fab)
{
    char sidecar[1088];
    snprintf(sidecar, sizeof(sidecar), "%s%s",
             fab->_resolved_path, RMS_SIDECAR_SUFFIX);

    FILE *f = fopen(sidecar, "wb");
    if (!f) return;

    /* Count keys in XAB chain */
    uint8_t num_keys = 0;
    if (fab->fab$b_org == FAB$C_IDX) {
        struct XABKEY *xab = fab->fab$l_xab;
        while (xab) {
            if (xab->xab$b_cod == XAB$C_KEY) num_keys++;
            xab = (struct XABKEY *)xab->xab$l_nxt;
        }
    }

    struct rms_metadata meta = {
        .magic   = RMS_META_MAGIC,
        .version = RMS_META_VERSION,
        .org     = fab->fab$b_org,
        .rfm     = fab->fab$b_rfm,
        .rat     = fab->fab$b_rat,
        .mrs     = fab->fab$w_mrs,
        .fsz     = fab->fab$b_fsz,
        .mrn     = fab->fab$l_mrn,
        .num_keys = num_keys
    };

    if (fwrite(&meta, sizeof(meta), 1, f) != 1) {
        fclose(f);
        return;
    }

    /* Write key definitions for indexed files */
    if (num_keys > 0) {
        struct XABKEY *xab = fab->fab$l_xab;
        while (xab) {
            if (xab->xab$b_cod == XAB$C_KEY) {
                struct rms_key_meta kmeta;
                memset(&kmeta, 0, sizeof(kmeta));
                kmeta.ref = xab->xab$b_ref;
                kmeta.dtp = xab->xab$b_dtp;
                kmeta.flg = xab->xab$w_flg;
                kmeta.nseg = xab->xab$b_nseg;
                kmeta.pos[0] = xab->xab$w_pos0;
                kmeta.pos[1] = xab->xab$w_pos1;
                kmeta.pos[2] = xab->xab$w_pos2;
                kmeta.pos[3] = xab->xab$w_pos3;
                kmeta.pos[4] = xab->xab$w_pos4;
                kmeta.pos[5] = xab->xab$w_pos5;
                kmeta.pos[6] = xab->xab$w_pos6;
                kmeta.pos[7] = xab->xab$w_pos7;
                kmeta.siz[0] = xab->xab$b_siz0;
                kmeta.siz[1] = xab->xab$b_siz1;
                kmeta.siz[2] = xab->xab$b_siz2;
                kmeta.siz[3] = xab->xab$b_siz3;
                kmeta.siz[4] = xab->xab$b_siz4;
                kmeta.siz[5] = xab->xab$b_siz5;
                kmeta.siz[6] = xab->xab$b_siz6;
                kmeta.siz[7] = xab->xab$b_siz7;
                if (fwrite(&kmeta, sizeof(kmeta), 1, f) != 1) break;
            }
            xab = (struct XABKEY *)xab->xab$l_nxt;
        }
    }

    fclose(f);
}

/*
 * sys$open - Open an existing file.
 *
 * Resolves the filespec from fab$l_fna, opens the underlying
 * Linux file, and loads any stored RMS metadata from the sidecar.
 *
 * On success: fab$l_sts = RMS$_NORMAL, returns RMS$_NORMAL
 * On failure: fab$l_sts set to appropriate error code.
 */
static uint32_t rms_impl_open(void *fab_ptr)
{
    struct FAB *fab = (struct FAB *)fab_ptr;
    if (!fab || fab->fab$b_bid != FAB$C_BID) {
        return RMS$_FAB;
    }

#if defined(__linux__)
    /* --- Files-11 ODS-2 ACP path (vms-bc7): $ASSIGN + IO$_ACCESS --- */
    {
        struct rms_acp_spec specs[RMS_ACP_MAX_CANDS];
        rms_file_t *h = NULL;
        uint32_t st = SS$_NOSUCHFILE;
        int ncand;
        int need_write = ((fab->fab$b_fac & FAB$M_PUT) ||
                          (fab->fab$b_fac & FAB$M_UPD) ||
                          (fab->fab$b_fac & FAB$M_DEL) ||
                          (fab->fab$b_fac & FAB$M_TRN));

        /* ATOMIC-FLIP DEFER (vms-5f0): executive absent => legacy POSIX body for
         * EVERY org, indexed included (its .rms_idx sidecar is the pre-flip
         * home). Probed BEFORE the ODS-2 candidate walk, which cannot resolve
         * without the mounted volume. */
        if (rms_acp_absent())
            return rms_posix_open(fab);

        /* vms-5f0: resolve directory/concealed logicals (SYS$STARTUP:, etc.)
         * to on-volume ODS-2 candidates and try each in search-list order. */
        ncand = rms_acp_specs_from_fab(fab, specs, RMS_ACP_MAX_CANDS);
        if (ncand < 0) {
            fab->fab$l_sts = RMS$_SYN;
            fab->fab$l_stv = 0;
            return RMS$_SYN;
        }
        strncpy(fab->_resolved_path, specs[0].name,
                sizeof(fab->_resolved_path) - 1);
        fab->_resolved_path[sizeof(fab->_resolved_path) - 1] = '\0';

        for (int i = 0; i < ncand; i++) {
            st = rms_acp_open_file(&specs[i], need_write, &h);
            if ($VMS_STATUS_SUCCESS(st))
                break;
        }
        if (!$VMS_STATUS_SUCCESS(st)) {
            if (st == SS$_NOSUCHFILE && (fab->fab$l_fop & FAB$M_CIF))
                return rms_impl_create(fab_ptr);
            fab->fab$l_stv = st;
            fab->fab$l_sts = rms_acp_open_status(st);
            return fab->fab$l_sts;
        }
        fab->_rms_file = h;

        /* Indexed-over-ACP (vms-5f0, epic vms-d0c): the data fork is ACCESSed;
         * bind the genuine Prolog-3 prologue (VBN 1 fixed prolog + key
         * descriptors) read via IO$_READVBLK. This SUPERSEDES the old
         * RMS$_ORG rejection -- a real indexed open over the mounted volume now
         * reads records by key from the on-disk bucket tree (rms_prolog3.c),
         * NOT from a private .rms_idx sidecar. A malformed / non-Prolog-3 /
         * compression-bearing prologue fails honestly (RMS$_PLG), never a
         * silent success (INV-6). */
        if (fab->fab$b_org == FAB$C_IDX) {
            p3_ctx_t *p3 = NULL;
            uint32_t pst = rms_p3_bind(h, &p3);
            if (!$VMS_STATUS_SUCCESS(pst)) {
                rms_acp_close_handle(h);
                fab->_rms_file = NULL;
                fab->fab$l_stv = 0;
                fab->fab$l_sts = pst;
                return pst;
            }
            fab->_rms_state = p3;
        }

        pthread_mutex_lock(&rms_id_lock);
        fab->fab$w_ifi = next_ifi++;
        if (next_ifi == 0) next_ifi = 1;
        pthread_mutex_unlock(&rms_id_lock);

        fab->fab$l_sts = RMS$_NORMAL;
        fab->fab$l_stv = 0;
        return RMS$_NORMAL;
    }
#else
    return rms_posix_open(fab);
#endif /* __linux__ */
}

/*
 * rms_posix_open - the pre-flip legacy $OPEN body (POSIX fd + metadata sidecar
 * over the /vms passthrough). The netbsd-vax cross's record backend, and the
 * __linux__ executive-absent defer target (vms-5f0). See the forward decl.
 */
static uint32_t rms_posix_open(struct FAB *fab)
{
    if (resolve_for_open(fab) < 0) {
        fab->fab$l_sts = RMS$_SYN;
        fab->fab$l_stv = 0;
        return RMS$_SYN;
    }

    /* Determine open flags from file access mode */
    int flags = O_RDONLY;
    if ((fab->fab$b_fac & FAB$M_PUT) || (fab->fab$b_fac & FAB$M_UPD) ||
        (fab->fab$b_fac & FAB$M_DEL) || (fab->fab$b_fac & FAB$M_TRN)) {
        flags = O_RDWR;
    }

    /*
     * NO PRE-CHECK HERE (vms-2b8, operator ruling 2026-07-31): see the
     * deleted rms_check_protection() above. open() below is the only
     * protection decision now; EACCES/EPERM maps to RMS$_PRV just below.
     */
    int fd = open(fab->_resolved_path, flags);
    if (fd < 0) {
        fab->fab$l_stv = (uint32_t)errno;
        switch (errno) {
            case ENOENT:
                /* If CIF (create-if) option set, try to create */
                if (fab->fab$l_fop & FAB$M_CIF) {
                    return rms_posix_create(fab);
                }
                fab->fab$l_sts = RMS$_FNF;
                return RMS$_FNF;
            case EACCES:
            case EPERM:
                fab->fab$l_sts = RMS$_PRV;
                return RMS$_PRV;
            case EISDIR:
                fab->fab$l_sts = RMS$_DIR;
                return RMS$_DIR;
            default:
                fab->fab$l_sts = RMS$_ACC;
                return RMS$_ACC;
        }
    }

    fab->_rms_file = rms_io_posix_wrap(fd);
    if (!fab->_rms_file) { close(fd); fab->fab$l_sts = RMS$_DME; return RMS$_DME; }
    load_metadata(fab);

    /* Assign an internal file identifier */
    pthread_mutex_lock(&rms_id_lock);
    fab->fab$w_ifi = next_ifi++;
    if (next_ifi == 0) next_ifi = 1;
    pthread_mutex_unlock(&rms_id_lock);

    fab->fab$l_sts = RMS$_NORMAL;
    fab->fab$l_stv = 0;
    return RMS$_NORMAL;
}

/*
 * rms_p3_params_from_xab - derive Prolog-3 key/bucket geometry from one XABKEY
 * (vms-401). key_size = total key size (all segments), seg-0 position/size locate
 * the embedded key within the record, and the bucket size is chosen to hold at
 * least two max-size records (so a data-bucket split always makes progress) and
 * the widest index entry. Returns RMS$_NORMAL, or RMS$_KEY on a missing/invalid
 * key definition.
 */
static uint32_t rms_p3_params_from_xab(const struct XABKEY *xab, uint16_t mrs,
                                       p3_create_params_t *p)
{
    uint16_t ksz;
    uint32_t need, ineed;
    uint8_t  blocks;

    if (!xab || xab->xab$b_cod != XAB$C_KEY)
        return RMS$_KEY;
    ksz = xab->xab$w_tks ? xab->xab$w_tks : xab->xab$b_siz0;
    if (ksz == 0 || ksz > 255)
        return RMS$_KEY;
    if (xab->xab$b_siz0 == 0 || xab->xab$b_siz0 > ksz)
        return RMS$_KEY;

    memset(p, 0, sizeof(*p));
    p->key_size  = ksz;
    p->seg0_pos  = xab->xab$w_pos0;
    p->seg0_siz  = xab->xab$b_siz0;
    p->dtp       = xab->xab$b_dtp;
    p->allow_dup = (xab->xab$w_flg & XAB$M_DUP) ? 1u : 0u;

    if (mrs == 0) mrs = 512u;
    need  = (uint32_t)P3_BKT_HDR_SIZE + 2u * ((uint32_t)P3_DR_HDR_SIZE + mrs);
    ineed = (uint32_t)P3_BKT_HDR_SIZE + P3_IDXREC_PTR_SIZE + ksz;
    if (ineed > need) need = ineed;
    blocks = (uint8_t)((need + P3_BLK - 1u) / P3_BLK);
    if (blocks < 1u) blocks = 1u;
    if (blocks > P3_MAX_BKT_BLOCKS) blocks = P3_MAX_BKT_BLOCKS;
    p->bkt_blocks = blocks;
    return RMS$_NORMAL;
}

/*
 * rms_idx_author_p3 - author a genuine, EMPTY Files-11 Prolog-3 indexed file over
 * the just-CREATEd/ACCESSed ACP write window `h` (vms-401). Writes the prologue,
 * the primary key's root index + first data bucket, and every secondary key from
 * the XABKEY chain (defined on the still-empty file). The returned WRITABLE ctx
 * is stored on fab->_rms_state (tagged P3_CTX_MAGIC) so rms_idx_put/update/delete
 * route to the real engine instead of the retired .rms_idx B-tree sidecar. On any
 * failure the partially-authored ctx is freed and the VMS status returned.
 */
static uint32_t rms_idx_author_p3(struct FAB *fab, rms_file_t *h, p3_ctx_t **out)
{
    p3_create_params_t p;
    p3_ctx_t *ctx = NULL;
    const struct XABKEY *xab = (const struct XABKEY *)fab->fab$l_xab;
    uint32_t st;

    *out = NULL;
    st = rms_p3_params_from_xab(xab, fab->fab$w_mrs, &p);
    if (!$VMS_STATUS_SUCCESS(st))
        return st;
    st = rms_p3_create(h, &p, &ctx);
    if (st != RMS$_CREATED)
        return st;

    /* Secondary keys from the XABKEY chain, defined on the empty file. */
    for (xab = (const struct XABKEY *)xab->xab$l_nxt; xab;
         xab = (const struct XABKEY *)xab->xab$l_nxt) {
        p3_create_params_t sp;
        if (xab->xab$b_cod != XAB$C_KEY)
            continue;
        st = rms_p3_params_from_xab(xab, fab->fab$w_mrs, &sp);
        if (!$VMS_STATUS_SUCCESS(st)) { rms_p3_free(ctx); return st; }
        st = rms_p3_add_secondary_key(ctx, &sp);
        if (!$VMS_STATUS_SUCCESS(st)) { rms_p3_free(ctx); return st; }
    }

    *out = ctx;
    return RMS$_CREATED;
}

/*
 * sys$create - Create a new file.
 *
 * Creates the file with automatic version numbering. Writes
 * the RMS metadata sidecar. Handles FAB$M_SUP (supersede) and
 * FAB$M_MXV (maximize version).
 */
static uint32_t rms_impl_create(void *fab_ptr)
{
    struct FAB *fab = (struct FAB *)fab_ptr;
    if (!fab || fab->fab$b_bid != FAB$C_BID) {
        return RMS$_FAB;
    }

#if defined(__linux__)
    /* --- Files-11 ODS-2 ACP path (vms-bc7): $ASSIGN + IO$_CREATE(+ACCESS) --- */
    {
        struct rms_acp_spec sp;
        struct rms_acp_spec specs[RMS_ACP_MAX_CANDS];
        rms_file_t *h;
        struct vms_acp_fileop_args fop;
        uint32_t chan = 0, st;
        int ncand;

        /* ATOMIC-FLIP DEFER (vms-5f0): executive absent => legacy POSIX create,
         * every org (see rms_impl_open). Probed before the ODS-2 candidate walk. */
        if (rms_acp_absent())
            return rms_posix_create(fab);

        /* vms-5f0: compose the target through any directory/concealed logical
         * (SYS$SYSTEM:, SYS$MANAGER:, ...) and create in the PRIMARY search-list
         * member -- exactly where VMS places a new file for a search list. */
        ncand = rms_acp_specs_from_fab(fab, specs, RMS_ACP_MAX_CANDS);
        if (ncand < 0) {
            fab->fab$l_sts = RMS$_SYN;
            return RMS$_SYN;
        }
        sp = specs[0];
        strncpy(fab->_resolved_path, sp.name, sizeof(fab->_resolved_path) - 1);
        fab->_resolved_path[sizeof(fab->_resolved_path) - 1] = '\0';

        st = vms_kif_acp_assign(sp.devnam, &chan);
        if (!$VMS_STATUS_SUCCESS(st)) {
            fab->fab$l_stv = st;
            fab->fab$l_sts = rms_acp_open_status(st);
            return fab->fab$l_sts;
        }
        h = calloc(1, sizeof(*h));
        if (!h) { vms_kif_dassgn(chan); fab->fab$l_sts = RMS$_DME; return RMS$_DME; }
        h->chan = chan; h->assigned = 1; h->fd = -1;

        memset(&fop, 0, sizeof(fop));
        fop.chan      = chan;
        fop.func      = VMS_ACP_FOP_CREATE;
        /* IO$M_CREATE enters a versioned directory entry; IO$M_ACCESS builds
         * the write window on this channel so record $PUTs ride it directly. */
        fop.modifiers = VMS_ACP_M_CREATE | VMS_ACP_M_ACCESS;
        fop.acctl     = VMS_ACP_ACCTL_WRITE;
        /* Indexed files carry a variable-length Prolog-3 block structure, never
         * a fixed-record ODS-2 kind, even when the user records are fixed. */
        fop.kind      = (fab->fab$b_org != FAB$C_IDX &&
                         fab->fab$b_rfm == FAB$C_FIX) ? ODS2_FK_DATA_FIX
                                                      : ODS2_FK_DATA;
        st = rms_acp_resolve_did(chan, sp.dirpath, &fop.did_num, &fop.did_seq,
                                 &fop.did_rvn, &fop.did_nmx);
        if (!$VMS_STATUS_SUCCESS(st)) {
            free(h); vms_kif_dassgn(chan);
            fab->fab$l_stv = st; fab->fab$l_sts = RMS$_DNF;
            return RMS$_DNF;
        }
        fop.version = 0;                     /* highest existing + 1 */
        strncpy(fop.name, sp.name, VMS_ACP_NAME_SIZE - 1);

        st = vms_kif_acp_fileop(&fop);
        if (!$VMS_STATUS_SUCCESS(st)) {
            free(h); vms_kif_dassgn(chan);
            fab->fab$l_stv = st; fab->fab$l_sts = RMS$_CRE;
            return RMS$_CRE;
        }
        h->accessed = 1; h->writable = 1;
        h->fid_num = fop.fid_num; h->fid_seq = fop.fid_seq;
        h->fid_rvn = fop.fid_rvn; h->fid_nmx = fop.fid_nmx;
        h->eof   = (uint64_t)(fop.new_efblk ? (fop.new_efblk - 1u) : 0) * 512u
                   + fop.new_ffbyte;
        h->hiblk = fop.new_hiblk;
        fab->_rms_file = h;

        /* Indexed-over-ACP (vms-401, epic vms-d0c): author the genuine Files-11
         * Prolog-3 image on the fresh data fork -- real prologue, root index +
         * data buckets over IO$_WRITEVBLK -- and bind the WRITABLE ctx into
         * _rms_state so record $PUTs ride rms_p3_put (rms_idx.c), NOT the retired
         * .rms_idx B-tree sidecar. A bad key definition / write failure fails
         * honestly (RMS$_KEY/RMS$_WPL), deaccessing the just-created file. */
        if (fab->fab$b_org == FAB$C_IDX) {
            p3_ctx_t *p3 = NULL;
            uint32_t pst = rms_idx_author_p3(fab, h, &p3);
            if (pst != RMS$_CREATED) {
                rms_acp_close_handle(h);
                fab->_rms_file = NULL;
                fab->fab$l_stv = 0;
                fab->fab$l_sts = pst;
                return pst;
            }
            fab->_rms_state = p3;
        }

        /* Relative files pre-allocate their fixed cells (IO$_MODIFY extend via
         * rms_io_ftruncate); best-effort, exactly as the old POSIX path. */
        if (fab->fab$b_org == FAB$C_REL && fab->fab$l_mrn > 0 &&
            fab->fab$w_mrs > 0) {
            size_t cell = (size_t)fab->fab$w_mrs + 1;
            if (fab->fab$l_mrn <= SIZE_MAX / cell)
                (void)rms_io_ftruncate(h, (off_t)(cell * fab->fab$l_mrn));
        }

        pthread_mutex_lock(&rms_id_lock);
        fab->fab$w_ifi = next_ifi++;
        if (next_ifi == 0) next_ifi = 1;
        pthread_mutex_unlock(&rms_id_lock);
        fab->fab$l_sts = RMS$_CREATED;
        fab->fab$l_stv = 0;
        return RMS$_NORMAL;
    }
#else
    return rms_posix_create(fab);
#endif /* __linux__ */
}

/*
 * rms_posix_create - the pre-flip legacy $CREATE body (versioned POSIX file +
 * metadata sidecar). netbsd-vax record backend / __linux__ defer target
 * (vms-5f0). See the forward decl.
 */
static uint32_t rms_posix_create(struct FAB *fab)
{
    if (resolve_filename(fab) < 0) {
        fab->fab$l_sts = RMS$_SYN;
        return RMS$_SYN;
    }

    /* Split path into directory and base filename */
    char dir[1024] = ".";
    char base[256];
    const char *last_slash = strrchr(fab->_resolved_path, '/');
    if (last_slash) {
        size_t dlen = (size_t)(last_slash - fab->_resolved_path);
        if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
        memcpy(dir, fab->_resolved_path, dlen);
        dir[dlen] = '\0';
        strncpy(base, last_slash + 1, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
    } else {
        strncpy(base, fab->_resolved_path, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
    }

    /* Strip any existing version number */
    char base_noversion[256];
    rms_strip_version(base, base_noversion, sizeof(base_noversion));

    /* Determine the version number for the new file */
    int next_ver = rms_next_version(dir, base_noversion);
    if (next_ver < 1) next_ver = 1;

    /* Build the versioned filename */
    char versioned[1024];
    snprintf(versioned, sizeof(versioned), "%s/%s;%d",
             dir, base_noversion, next_ver);
    strncpy(fab->_resolved_path, versioned,
            sizeof(fab->_resolved_path) - 1);
    fab->_resolved_path[sizeof(fab->_resolved_path) - 1] = '\0';

    /* Determine creation flags */
    int flags = O_CREAT | O_RDWR;
    if (fab->fab$l_fop & FAB$M_SUP) {
        flags |= O_TRUNC;
    }

    /* Ensure parent directory exists */
    struct stat st;
    if (stat(dir, &st) < 0) {
        fab->fab$l_sts = RMS$_DNF;
        fab->fab$l_stv = (uint32_t)errno;
        return RMS$_DNF;
    }

    /* Use VMS default protection converted to mode_t instead of hardcoded 0644 */
    uint16_t default_prot = rms_get_default_protection();
    mode_t create_mode = vmsfs_protection_to_mode(default_prot);
    int fd = open(fab->_resolved_path, flags, create_mode);
    if (fd < 0) {
        fab->fab$l_sts = RMS$_CRE;
        fab->fab$l_stv = (uint32_t)errno;
        return RMS$_CRE;
    }

    fab->_rms_file = rms_io_posix_wrap(fd);
    if (!fab->_rms_file) { close(fd); fab->fab$l_sts = RMS$_DME; return RMS$_DME; }

    /* Pre-allocate space for relative files */
    if (fab->fab$b_org == FAB$C_REL && fab->fab$l_mrn > 0 &&
        fab->fab$w_mrs > 0) {
        size_t cell_size = (size_t)fab->fab$w_mrs + 1;  /* +1 for status */
        /* Overflow check: ensure cell_size * mrn doesn't wrap */
        if (fab->fab$l_mrn <= SIZE_MAX / cell_size) {
            size_t total = cell_size * fab->fab$l_mrn;
            if (ftruncate(fd, (off_t)total) < 0) {
                /* Non-fatal: allocation is best-effort */
            }
        }
    }

    save_metadata(fab);

    /* Assign IFI (shared counter with sys$open) */
    pthread_mutex_lock(&rms_id_lock);
    fab->fab$w_ifi = next_ifi++;
    if (next_ifi == 0) next_ifi = 1;
    pthread_mutex_unlock(&rms_id_lock);

    fab->fab$l_sts = RMS$_CREATED;
    fab->fab$l_stv = 0;
    return RMS$_NORMAL;
}

/*
 * rms_posix_close - POSIX teardown for a legacy-defer / netbsd-vax handle:
 * fsync+close the fd, and on delete-on-close (DLT/TMD) unlink the data file and
 * its metadata/index sidecars (vms-5f0). A failed fsync surfaces as RMS$_WER.
 */
static void rms_posix_close(struct FAB *fab, int deleting, uint32_t *close_sts)
{
    if (fab->_rms_file) {
        int fd = rms_io_posix_fd((rms_file_t *)fab->_rms_file);
        if (fd >= 0 && fsync(fd) < 0)
            *close_sts = RMS$_WER;
        rms_io_posix_unwrap((rms_file_t *)fab->_rms_file);
        fab->_rms_file = NULL;
    }
    if (deleting && fab->_resolved_path[0]) {
        unlink(fab->_resolved_path);
        char sidecar[1088];
        snprintf(sidecar, sizeof(sidecar), "%s%s",
                 fab->_resolved_path, RMS_SIDECAR_SUFFIX);
        unlink(sidecar);
        char idxfile[1088];
        snprintf(idxfile, sizeof(idxfile), "%s%s",
                 fab->_resolved_path, RMS_INDEX_SUFFIX);
        unlink(idxfile);
    }
}

/*
 * sys$close - Close an open file.
 *
 * Flushes any pending data, closes the file descriptor,
 * and cleans up internal state. Handles TMD (temp delete on close)
 * and DLT (delete on close) options.
 */
static uint32_t rms_impl_close(void *fab_ptr)
{
    struct FAB *fab = (struct FAB *)fab_ptr;
    if (!fab || fab->fab$b_bid != FAB$C_BID) {
        return RMS$_FAB;
    }

    uint32_t close_sts = RMS$_NORMAL;
    int deleting = (fab->fab$l_fop & FAB$M_DLT) || (fab->fab$l_fop & FAB$M_TMD);

#if defined(__linux__)
    if (fab->_rms_file && ((rms_file_t *)fab->_rms_file)->fd >= 0) {
        /* vms-5f0 legacy-defer handle: POSIX teardown (fd close + sidecar
         * unlink on DLT/TMD), identical to the netbsd-vax path below. */
        rms_posix_close(fab, deleting, &close_sts);
    } else if (fab->_rms_file) {
        /* --- ACP path (vms-bc7): IO$_DEACCESS + $DASSGN (+ IO$_DELETE on DLT/TMD) --- */
        rms_file_t *h = (rms_file_t *)fab->_rms_file;
        rms_io_fsync(h);                       /* WRITEVBLK is write-through */
        if (deleting) {
            /* Release the window, then deallocate the file by FID. */
            struct vms_acp_fileop_args fop;
            if (h->accessed) { vms_kif_acp_deaccess(h->chan); h->accessed = 0; }
            memset(&fop, 0, sizeof(fop));
            fop.chan      = h->chan;
            fop.func      = VMS_ACP_FOP_DELETE;
            fop.modifiers = VMS_ACP_M_DELETE;
            fop.fidmode   = 1;
            fop.fid_num   = h->fid_num; fop.fid_seq = h->fid_seq;
            fop.fid_rvn   = h->fid_rvn; fop.fid_nmx = h->fid_nmx;
            (void)vms_kif_acp_fileop(&fop);
        }
        rms_acp_close_handle(h);               /* deaccess (if any) + dassgn + free */
        fab->_rms_file = NULL;
    }
#else
    rms_posix_close(fab, deleting, &close_sts);
#endif

    /*
     * Clean up internal state.
     *
     * For an indexed file, fab->_rms_state holds the in-memory B-tree. Because
     * rms_idx_put only persists the tree every 100 inserts, the tree normally
     * carries records added since the last periodic save; freeing it raw would
     * silently drop them while sys$close still returned success (vms-5c6d).
     * rms_idx_cleanup() runs btree_save() before freeing so the index reaches
     * disk, and reports a write failure as RMS$_WER rather than faking success
     * (INV-6). Sequential and relative files never allocate _rms_state, so
     * their close path is unchanged (the org guard is belt-and-suspenders).
     *
     * When the file is being deleted on close we must NOT re-persist the index
     * -- the delete block above already unlinked the .rms_idx sidecar, and a
     * save here would resurrect it as an orphan -- so just free the tree.
     */
    if (fab->_rms_state) {
        if (fab->fab$b_org == FAB$C_IDX && !deleting) {
            uint32_t idx_sts = rms_idx_cleanup(fab);  /* btree_save() + free */
            if (!(idx_sts & 1u) && (close_sts & 1u)) {
                close_sts = idx_sts;  /* surface the index write error */
            }
        } else {
            free(fab->_rms_state);
            fab->_rms_state = NULL;
        }
    }

    fab->fab$w_ifi = 0;
    fab->fab$l_sts = close_sts;
    fab->fab$l_stv = 0;
    return close_sts;
}

/*
 * sys$erase - Delete a file.
 *
 * Resolves the filespec and deletes the file along with
 * its metadata sidecar and index sidecar.
 */
static uint32_t rms_impl_erase(void *fab_ptr)
{
    struct FAB *fab = (struct FAB *)fab_ptr;
    if (!fab || fab->fab$b_bid != FAB$C_BID) {
        return RMS$_FAB;
    }

#if defined(__linux__)
    /* --- ACP path (vms-bc7): $ASSIGN + IO$_DELETE (remove entry + dealloc) --- */
    {
        struct rms_acp_spec specs[RMS_ACP_MAX_CANDS];
        struct vms_acp_fileop_args fop;
        uint32_t chan = 0, st = SS$_NOSUCHFILE;
        int ncand, done = 0;

        /* ATOMIC-FLIP DEFER (vms-5f0): executive absent => legacy POSIX unlink
         * (see rms_impl_open). Probed before the ODS-2 candidate walk. */
        if (rms_acp_absent())
            return rms_posix_erase(fab);

        /* vms-5f0: resolve directory/concealed logicals and try each search-
         * list candidate in order; the first that resolves + deletes wins. */
        ncand = rms_acp_specs_from_fab(fab, specs, RMS_ACP_MAX_CANDS);
        if (ncand < 0) {
            fab->fab$l_sts = RMS$_SYN;
            return RMS$_SYN;
        }
        for (int i = 0; i < ncand && !done; i++) {
            struct rms_acp_spec *sp = &specs[i];
            chan = 0;
            st = vms_kif_acp_assign(sp->devnam, &chan);
            if (!$VMS_STATUS_SUCCESS(st))
                continue;                    /* try the next member */
            memset(&fop, 0, sizeof(fop));
            fop.chan      = chan;
            fop.func      = VMS_ACP_FOP_DELETE;
            fop.modifiers = VMS_ACP_M_DELETE;  /* remove entry AND deallocate */
            st = rms_acp_resolve_did(chan, sp->dirpath, &fop.did_num,
                                     &fop.did_seq, &fop.did_rvn, &fop.did_nmx);
            if (!$VMS_STATUS_SUCCESS(st)) {
                vms_kif_dassgn(chan);
                continue;                    /* directory absent in this member */
            }
            fop.version = sp->version;       /* 0 => all versions */
            strncpy(fop.name, sp->name, VMS_ACP_NAME_SIZE - 1);

            st = vms_kif_acp_fileop(&fop);
            vms_kif_dassgn(chan);
            if ($VMS_STATUS_SUCCESS(st))
                done = 1;
        }
        if (!done) {
            fab->fab$l_stv = st;
            fab->fab$l_sts = (st == SS$_NOSUCHFILE) ? RMS$_FNF
                           : (st == SS$_NOPRIV)     ? RMS$_PRV
                                                    : RMS$_ACC;
            return fab->fab$l_sts;
        }
        fab->fab$l_sts = RMS$_NORMAL;
        fab->fab$l_stv = 0;
        return RMS$_NORMAL;
    }
#else
    return rms_posix_erase(fab);
#endif /* __linux__ */
}

/*
 * rms_posix_erase - the pre-flip legacy $ERASE body (unlink data + sidecars).
 * netbsd-vax record backend / __linux__ defer target (vms-5f0). See fwd decl.
 */
static uint32_t rms_posix_erase(struct FAB *fab)
{
    if (resolve_for_open(fab) < 0) {
        fab->fab$l_sts = RMS$_SYN;
        return RMS$_SYN;
    }

    /*
     * NO PRE-CHECK HERE (vms-2b8, operator ruling 2026-07-31): see the
     * deleted rms_check_protection() above. unlink() below is the only
     * protection decision now; EACCES/EPERM maps to RMS$_PRV just below.
     */
    if (unlink(fab->_resolved_path) < 0) {
        fab->fab$l_stv = (uint32_t)errno;
        switch (errno) {
            case ENOENT:
                fab->fab$l_sts = RMS$_FNF;
                return RMS$_FNF;
            case EACCES:
            case EPERM:
                fab->fab$l_sts = RMS$_PRV;
                return RMS$_PRV;
            default:
                fab->fab$l_sts = RMS$_ACC;
                return RMS$_ACC;
        }
    }

    /* Remove the metadata sidecar (ignore errors) */
    char sidecar[1088];
    snprintf(sidecar, sizeof(sidecar), "%s%s",
             fab->_resolved_path, RMS_SIDECAR_SUFFIX);
    unlink(sidecar);

    /* Remove index sidecar (ignore errors) */
    char idxfile[1088];
    snprintf(idxfile, sizeof(idxfile), "%s%s",
             fab->_resolved_path, RMS_INDEX_SUFFIX);
    unlink(idxfile);

    fab->fab$l_sts = RMS$_NORMAL;
    fab->fab$l_stv = 0;
    return RMS$_NORMAL;
}

/*
 * sys$connect - Connect a RAB to its FAB, establishing a record stream.
 *
 * Initializes the internal stream state in the RAB. Validates the
 * RAB-FAB linkage. If RAB$M_EOF is set, positions to end of file.
 */
static uint32_t rms_impl_connect(void *rab_ptr)
{
    struct RAB *rab = (struct RAB *)rab_ptr;
    if (!rab || rab->rab$b_bid != RAB$C_BID) {
        return RMS$_RAB;
    }

    struct FAB *fab = rab->rab$l_fab;
    if (!fab || fab->fab$b_bid != FAB$C_BID) {
        rab->rab$l_sts = RMS$_FAB;
        return RMS$_FAB;
    }

    if (!fab->_rms_file) {
        rab->rab$l_sts = RMS$_ACC;
        return RMS$_ACC;
    }

    /* Initialize stream state */
    rab->_current_offset = 0;
    rab->_eof = 0;
    rab->_last_rec_offset = 0;
    rab->_last_rec_size = 0;

    /* Assign an internal stream identifier */
    static uint16_t next_isi = 1;
    pthread_mutex_lock(&rms_id_lock);
    rab->rab$w_isi = next_isi++;
    if (next_isi == 0) next_isi = 1;
    pthread_mutex_unlock(&rms_id_lock);

    /* If RAB$M_EOF is set, position to end of file */
    if (rab->rab$l_rop & RAB$M_EOF) {
        rab->_current_offset = rms_io_lseek(fab->_rms_file, 0, SEEK_END);
        if (rab->_current_offset < 0) rab->_current_offset = 0;
    }

    rab->rab$l_sts = RMS$_NORMAL;
    rab->rab$l_stv = 0;
    return RMS$_NORMAL;
}

/*
 * sys$disconnect - Disconnect a RAB from its FAB.
 *
 * Cleans up the stream state. The file remains open in the FAB.
 */
static uint32_t rms_impl_disconnect(void *rab_ptr)
{
    struct RAB *rab = (struct RAB *)rab_ptr;
    if (!rab || rab->rab$b_bid != RAB$C_BID) {
        return RMS$_RAB;
    }

    /* Clean up internal stream state */
    if (rab->_rms_stream) {
        free(rab->_rms_stream);
        rab->_rms_stream = NULL;
    }

    rab->_current_offset = 0;
    rab->_eof = 0;
    rab->_last_rec_offset = 0;
    rab->_last_rec_size = 0;
    rab->rab$w_isi = 0;

    rab->rab$l_sts = RMS$_NORMAL;
    rab->rab$l_stv = 0;
    return RMS$_NORMAL;
}

/*
 * sys$display - Display/retrieve file attributes.
 *
 * Loads metadata from the sidecar into the FAB and any XAB chain.
 * Also fills in XAB date/time and protection info from the filesystem.
 */
static uint32_t rms_impl_display(void *fab_ptr)
{
    struct FAB *fab = (struct FAB *)fab_ptr;
    if (!fab || fab->fab$b_bid != FAB$C_BID) {
        return RMS$_FAB;
    }

    /* Reload metadata from sidecar */
    load_metadata(fab);

    /* Fill in date/time XAB if present in the XAB chain */
    if (fab->_resolved_path[0]) {
        struct stat st;
        if (stat(fab->_resolved_path, &st) == 0) {
            /* Walk XAB chain looking for date and protection XABs */
            struct XABKEY *xab = fab->fab$l_xab;
            while (xab) {
                if (xab->xab$b_cod == XAB$C_DAT) {
                    struct XABDAT *dat = (struct XABDAT *)xab;
                    /*
                     * Store real VMS binary-time quadwords, not raw Unix
                     * time_t (vms-3dd). st_ctime/st_mtime are seconds since the
                     * Unix epoch; xab$q_cdt/xab$q_rdt are VMS's 100ns-since-
                     * 17-NOV-1858 format that DIRECTORY/DATE, $ASCTIM and
                     * XABDAT readers decode. Convert through the RTL converter.
                     */
                    dat->xab$q_cdt = unix_time_to_vms(st.st_ctime);
                    dat->xab$q_rdt = unix_time_to_vms(st.st_mtime);
                } else if (xab->xab$b_cod == XAB$C_PRO) {
                    struct XABPRO *pro = (struct XABPRO *)xab;
                    /* Convert Unix uid/gid to VMS UIC (group << 16 | member) */
                    pro->xab$l_uic = ((uint32_t)(st.st_gid & 0xFFFF) << 16) |
                                      (uint32_t)(st.st_uid & 0xFFFF);
                    /* Convert Unix mode to VMS protection mask */
                    pro->xab$w_pro = vmsfs_mode_to_protection(st.st_mode);
                }
                xab = (struct XABKEY *)xab->xab$l_nxt;
            }
        }
    }

    fab->fab$l_sts = RMS$_NORMAL;
    return RMS$_NORMAL;
}

/*
 * sys$rewind - Rewind a record stream to the beginning of the file.
 */
static uint32_t rms_impl_rewind(void *rab_ptr)
{
    struct RAB *rab = (struct RAB *)rab_ptr;
    if (!rab || rab->rab$b_bid != RAB$C_BID) {
        return RMS$_RAB;
    }

    struct FAB *fab = rab->rab$l_fab;
    if (!fab || !fab->_rms_file) {
        rab->rab$l_sts = RMS$_ACC;
        return RMS$_ACC;
    }

    rms_io_lseek(fab->_rms_file, 0, SEEK_SET);
    rab->_current_offset = 0;
    rab->_eof = 0;
    rab->_last_rec_offset = 0;
    rab->_last_rec_size = 0;

    rab->rab$l_sts = RMS$_NORMAL;
    return RMS$_NORMAL;
}

/*
 * sys$flush - Flush buffered data to disk.
 */
static uint32_t rms_impl_flush(void *rab_ptr)
{
    struct RAB *rab = (struct RAB *)rab_ptr;
    if (!rab || rab->rab$b_bid != RAB$C_BID) {
        return RMS$_RAB;
    }

    struct FAB *fab = rab->rab$l_fab;
    if (!fab || !fab->_rms_file) {
        rab->rab$l_sts = RMS$_ACC;
        return RMS$_ACC;
    }

    if (rms_io_fsync(fab->_rms_file) < 0) {
        rab->rab$l_sts = RMS$_WER;
        rab->rab$l_stv = (uint32_t)errno;
        return RMS$_WER;
    }

    rab->rab$l_sts = RMS$_NORMAL;
    return RMS$_NORMAL;
}

/*
 * sys$extend - Extend a file's allocation.
 *
 * VSI OpenVMS RMS Reference Manual, $EXTEND: increases a file's allocated
 * space by fab$l_alq blocks WITHOUT moving the file's end-of-file marker
 * (a pre-allocation, not a write). The caller sets fab$l_alq to the number
 * of blocks to add before the call.
 *
 * An OVMX file is backed by a plain Linux file whose blocks are allocated on
 * demand by the host filesystem, so the space $EXTEND promises is genuinely
 * available to the subsequent $PUTs without a physical pre-reserve (and up to
 * real disk capacity, exactly as $EXTEND itself is bounded). The observable
 * contract — "the records the caller is about to write will fit" — is therefore
 * met by the backing store. This is NOT a facade: no per-process state is faked
 * and no success is reported for a control block that is not a real, open file.
 * We deliberately do NOT grow the file physically, because a sequential file's
 * logical EOF is its byte length; padding it here would corrupt the record
 * stream a subsequent $GET reads back. (Register declaration for $EXTEND is in
 * the file header block above, with the other rms_core services.)
 */
static uint32_t rms_impl_extend(void *fab_ptr)
{
    struct FAB *fab = (struct FAB *)fab_ptr;
    if (!fab || fab->fab$b_bid != FAB$C_BID) {
        return RMS$_FAB;
    }
    if (!fab->_rms_file) {
        fab->fab$l_sts = RMS$_IFI;
        return RMS$_IFI;
    }

#if defined(__linux__)
    /* $EXTEND -> IO$_MODIFY (vms-bc7): allocate fab$l_alq more blocks to the
     * file WITHOUT moving EOF -- a real ODS-2 allocation (BITMAP.SYS +
     * retrieval-pointer append), by FID on the accessed channel. A zero request
     * is a no-op success. Fail-honest: a full volume is SS$_DEVICEFULL. */
    {
        rms_file_t *h = (rms_file_t *)fab->_rms_file;
        struct vms_acp_fileop_args fop;
        uint32_t st;

        if (fab->fab$l_alq == 0) {
            fab->fab$l_sts = RMS$_NORMAL;
            return RMS$_NORMAL;
        }
        memset(&fop, 0, sizeof(fop));
        fop.chan    = h->chan;
        fop.func    = VMS_ACP_FOP_MODIFY;
        fop.fidmode = 1;
        fop.fid_num = h->fid_num; fop.fid_seq = h->fid_seq;
        fop.fid_rvn = h->fid_rvn; fop.fid_nmx = h->fid_nmx;
        fop.exsz    = fab->fab$l_alq;
        st = vms_kif_acp_fileop(&fop);
        if (!$VMS_STATUS_SUCCESS(st)) {
            fab->fab$l_stv = st;
            fab->fab$l_sts = RMS$_CRE;   /* allocation/extend error */
            return RMS$_CRE;
        }
        h->hiblk = fop.new_hiblk;        /* EOF unchanged; only allocation grows */
        fab->fab$l_sts = RMS$_NORMAL;
        return RMS$_NORMAL;
    }
#else
    fab->fab$l_sts = RMS$_NORMAL;
    return RMS$_NORMAL;
#endif
}


/* ============================================================
 * Public RMS entry points: VMS three-argument form
 *   SYS$xxx cb ,[err] ,[suc]   (VSI OpenVMS RMS Reference, Part III)
 * Thin wrappers over the synchronous rms_impl_* bodies above that
 * dispatch the optional AST-level completion routine (rms_complete).
 * ============================================================ */
uint32_t sys$extend(void *fab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_extend(fab), fab, err, suc);
}

uint32_t sys$open(void *fab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_open(fab), fab, err, suc);
}

uint32_t sys$create(void *fab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_create(fab), fab, err, suc);
}

uint32_t sys$close(void *fab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_close(fab), fab, err, suc);
}

uint32_t sys$erase(void *fab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_erase(fab), fab, err, suc);
}

uint32_t sys$connect(void *rab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_connect(rab), rab, err, suc);
}

uint32_t sys$disconnect(void *rab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_disconnect(rab), rab, err, suc);
}

uint32_t sys$display(void *fab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_display(fab), fab, err, suc);
}

uint32_t sys$rewind(void *rab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_rewind(rab), rab, err, suc);
}

uint32_t sys$flush(void *rab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_flush(rab), rab, err, suc);
}
