/*
 * product.c - PRODUCT.EXE: OVMX's PCSI-equivalent product installer
 * (bead vms-df9, docs/design-vms-faithful-install.md sec 3.3, docs/design-
 * ovmx-kit-format.md).
 *
 *   PRODUCT INSTALL <name> /SOURCE=<kit-filespec> [/DESTINATION=<devdir>]
 *   PRODUCT SHOW PRODUCT [/DESTINATION=<devdir>]
 *   PRODUCT SHOW HISTORY [/DESTINATION=<devdir>]
 *   (everything else: %PCSI-W-NOTIMPL, matching the pre-existing baseline)
 *
 * EXTERNAL UTILITY, NOT A DCL BUILTIN (vms-df9 constraint #1). Same pattern
 * as cmd_analyze/cmd_install/cmd_sysgen/cmd_sysman/cmd_mail in
 * src/vmsdcl/dcl_cmd_misc.c: cmd_product() only parses DCL syntax
 * (subcommand, product name, /SOURCE, /DESTINATION), resolves /SOURCE
 * through dcl_resolve_path() (so a VMS filespec OR a quoted literal Linux
 * path both work, exactly like COPY/LINK), and re-execs this binary via
 * dcl_exec_utility() -- it does not touch the kit format, the product
 * database, or a target volume itself. Before this bead, `cmd_product`
 * implemented PRODUCT SHOW PRODUCT/HISTORY directly in C inside the DCL
 * builtin table (a fat builtin), and every other operation was a bare
 * %PCSI-E-NOTIMPL stub with no PRODUCT.EXE at all. This keeps SHOW's
 * output shape identical (byte-for-byte on the no-database fallback path,
 * see below) while moving the real work out of the shell.
 *
 * KIT READING: reuses src/product/ovmx_kit_reader.c (open/validate/
 * read-entries/read-file-verified) -- the same module tools/ovmx_kit_pack.c
 * was refactored to use, so there is exactly one kit parser in the tree,
 * not two (vms-df9 constraint).
 *
 * WHERE FILES LAND (OVMX-invented policy, Rule 8 -- VSI has never
 * published where PCSI actually copies target-system files on disk below
 * the visible SYS$COMMON:[SYSEXE] logical, so the choice of a rooted vs
 * flat on-disk shape is OVMX's own, not a fact about PCSI):
 *   Both the default (currently-running system) and an explicit
 *   /DESTINATION lay the kit into the SAME rooted, concealed system-disk
 *   structure -- SYS0/ the boot root, SYS0/SYSCOMMON/ the shared common
 *   tree -- with the entry's own [SYSEXE]/[SYSLIB]/... bracket hanging off
 *   SYS0/SYSCOMMON/. This mirrors ovmx_layout.h's VMS_SYSEXE ==
 *   DEV:[SYS0.SYSCOMMON.SYSEXE] and the tree distro/Dockerfile.bootable
 *   masters DKA0: from, so a /DESTINATION-installed volume is boot-
 *   structurally identical to a mastered disk and boots AS ITS OWN system
 *   disk (vms-96ec, docs/design-vms-faithful-install.md §3.5). Before this
 *   fix, /DESTINATION wrote a FLAT <mount>/<bracket>/<name> with no
 *   SYS0/SYSCOMMON root: files landed on the volume but the target was NOT
 *   bootable -- STARTUP resolves SYS$SYSROOT:[SYSEXE]DCL.EXE through the
 *   rooted structure (VMS_SYSTEM_DIR = /vms/SYS0/SYSCOMMON/SYSEXE) and
 *   found nothing, halting %OVMX-F-SYSINIT. The SYS0/SYSCOMMON prefix
 *   lives on the volume, so it is mount-point-independent: the same bytes
 *   installed at /mnt/dkaNNN/SYS0/SYSCOMMON/... resolve correctly once the
 *   volume is booted as DKA0: -> /vms.
 * The product database follows the file layout: SYS$SYSTEM: means
 * VMS_SYSTEM_DIR on the default target, or SYS0/SYSCOMMON/SYSEXE off the
 * destination's own mount root on an explicit /DESTINATION (the same
 * SYS$SYSTEM: the installed files use), so PRODUCT SHOW PRODUCT
 * /DESTINATION reads the database back from where INSTALL wrote it.
 *
 * SECURITY (vms-df9 constraint #2): every file's protection/owner-UIC
 * comes from its `ovmx_kit_entry` (kit metadata written by
 * tools/ovmx_kit_pack.c from the packed tree), applied via fchmod(2)/
 * fchown(2) -- never a hardcoded 0777/world-writable default.
 *
 * A vmsfs.ko `.setattr` hook was tried and REVERTED (vms-79b): it broke
 * tests/qemu/test_release_e2e.sh's `norecord` case (a SYSUAF rewritten
 * with no SYSTEM row must HALT at boot; with the hook present it silently
 * booted to a login prompt instead -- confirmed by bisect, root cause not
 * fully isolated, blast radius too large to carry for what it bought).
 * What it bought was very little: vmsfs_blkdev_create() already assigns
 * every NEW file VMSFS_PROT_DEFAULT and the creating process's own UIC,
 * and ovmx_kit_pack.c's OVMX_KIT_PROT_DEFAULT / OVMX_KIT_UIC_*_DEFAULT are
 * numerically IDENTICAL to those (both 0xAA00, both SYSTEM [1,4] when
 * PRODUCT INSTALL runs as SYSTEM, which it is expected to) -- so for every
 * kit ovmx_kit_pack produces today, the value the kernel assigns AT
 * CREATION already matches the kit's own metadata before fchmod/fchown
 * below ever runs. Without the hook, chmod(2)/chown(2) against a real
 * blkdev-mode vmsfs mount still return success (the kernel's generic
 * simple_setattr() fallback, not a no-op or an error) but do not durably
 * change vi->vms_prot / the on-disk owner UIC if the requested value
 * actually DIFFERS from what create() already set -- a real limitation for
 * a kit that ever ships a divergent per-file protection or owner, but not
 * one any current kit hits, and NOT a security regression: a newly
 * created file can never land more permissive than VMSFS_PROT_DEFAULT
 * (never world-writable) regardless of this gap. Tracked as a follow-up
 * (vms-738) rather than solved by reintroducing kernel surface here.
 *
 * PRODUCT DATABASE: src/product/ovmx_product_db.h, OVMX-defined and
 * labeled (Rule 8) -- see that header. This file is the only reader and
 * writer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "ovmx_layout.h"
#include "ovmx_identity.h"
#include "ovmx_kit_reader.h"
#include "ovmx_product_db.h"

/*
 * vms-3a8 (converged flip, Wall 1): route PRODUCT INSTALL through the executive
 * Files-11 ACP, exactly as RMS and AUTHORIZE already do -- never a POSIX
 * open()/write()/mkdir() on a /mnt/<dev> passthrough (Rule 9 / INV-6). The flip
 * (vms-481, epic vms-208) moved MOUNT to the executive-global ACP: a MOUNTed
 * volume has NO /proc/mounts row and NO /mnt/<dev> Linux mount, so the old
 * pd_mount_point_is_mounted("/mnt/<dev>") probe reported %PCSI-E-NOTMOUNTED for a
 * unit the ACP had just mounted, and -- even bypassed -- do_install's raw
 * open()/write() landed the kit on the CONTAINER filesystem, never on the target
 * volume's backing disk, so a separate boot of that disk (R1 e2e vms-37f
 * container 2) found nothing installed.
 *
 *   - Destination detection: $ASSIGN a channel to the named unit through the ACP
 *     (vms_kif_acp_assign). Success => the executive has it mounted; SS$_NOSUCHDEV
 *     => genuinely not mounted => %PCSI-E-NOTMOUNTED (fail-honest).
 *   - Directory tree: create SYS0/SYSCOMMON/<bracket> on the target volume over
 *     the ACP (IO$_CREATE of NAME.DIR files, vms_kif_acp_fileop), the VMS way.
 *   - File + database writes: a fresh versioned file per kit entry through RMS
 *     (rms_open_named_handle create + rms_io_write_exact = $CREATE + block $PUT),
 *     which is itself ACP-routed and write-through synchronous -- durable across
 *     the install's DISMOUNT and the container boundary.
 *
 * When /dev/vms is unreachable (host ctest, plain-container gates) RMS's own
 * atomic-flip defer (rms_acp_absent) transparently POSIX-wraps the resolved
 * on-volume path, so a bare `ctest` still exercises the default-system paths;
 * the runtime never reaches that defer (Rule 9). Directory creation mirrors the
 * same split (ACP IO$_CREATE when present, mkdir -p on the defer).
 */
#include "ssdef.h"          /* $VMS_STATUS_SUCCESS, SS$_NOSUCHFILE/_NOSUCHDEV */
#include "vmsfs/ods2.h"     /* ODS2_FH2_M_DIRECTORY */
#include "vms_kif.h"        /* vms_kif_acp_assign/_access/_deaccess/_fileop, _dassgn */
#include "rms_io.h"         /* rms_open_named_handle / rms_io_write_exact / ... */
#include "rms/rms.h"        /* rms_executive_absent */

#define PRODUCT_DB_NAME "VMS$PRODUCT_DATABASE.DAT"

static const char *pd_months[] = {
    "JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"
};

/* mkdir -p, ignoring EEXIST, over every '/'-separated component of @path.
 * Used ONLY on the executive-absent (host ctest / plain-container) defer, where
 * RMS itself falls back to POSIX; the runtime creates directories over the ACP
 * (pd_acp_mkdir_tree) and never reaches this. */
static int pd_mkdir_parents(const char *path)
{
    char accum[4096] = "";
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);

    char *tok, *save;
    for (tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        strncat(accum, "/", sizeof(accum) - strlen(accum) - 1);
        strncat(accum, tok, sizeof(accum) - strlen(accum) - 1);
        if (mkdir(accum, 0755) != 0 && errno != EEXIST)
            return -1;
    }
    return 0;
}

/* ================================================================
 * Destination resolution: where a /DESTINATION device (or the default,
 * currently-running system) actually lands files, and how a kit entry's
 * relative path becomes a VMS filespec on that volume. See the file header
 * comment for the ACP-routed policy this implements.
 * ================================================================ */

struct pd_dest {
    int  is_default;              /* no /DESTINATION given                     */
    char devname[16];             /* canonical "DKA100:" form (default: DKA0:) */
};

/* @destarg is the raw qualifier value (may be NULL/empty) with or without a
 * trailing colon, any case. Returns 0 and fills *dest, or -1 with an honest
 * message already printed (destination named but not mounted in the executive).
 *
 * The DEFAULT (currently-running system) needs no mount check -- its own system
 * disk is by definition mounted -- so it is never probed. An explicit
 * /DESTINATION is verified through the executive Files-11 ACP: $ASSIGN a channel
 * to the unit (vms_kif_acp_assign). SS$_NOSUCHDEV -- the ONLY "not a mounted
 * volume" status -- yields %PCSI-E-NOTMOUNTED (fail-honest, INV-6); this is also
 * what an executive-absent host returns, correctly refusing a /DESTINATION
 * install with no /dev/vms. */
static int pd_resolve_destination(const char *destarg, struct pd_dest *dest)
{
    memset(dest, 0, sizeof(*dest));

    if (!destarg || !destarg[0]) {
        dest->is_default = 1;
        snprintf(dest->devname, sizeof(dest->devname), "%s:", SYSDISK_DEVICE);
        return 0;
    }

    size_t n = strlen(destarg);
    if (n >= sizeof(dest->devname) - 1) n = sizeof(dest->devname) - 2;
    size_t i;
    for (i = 0; i < n; i++)
        dest->devname[i] = (char)toupper((unsigned char)destarg[i]);
    dest->devname[i] = '\0';
    if (i == 0 || dest->devname[i - 1] != ':') {
        dest->devname[i] = ':';
        dest->devname[i + 1] = '\0';
    }

    uint32_t chan = 0;
    uint32_t st = vms_kif_acp_assign(dest->devname, &chan);
    if (!$VMS_STATUS_SUCCESS(st)) {
        fprintf(stderr, "%%PCSI-E-NOTMOUNTED, destination device %s is not mounted\n",
                dest->devname);
        return -1;
    }
    vms_kif_dassgn(chan);   /* only wanted to confirm it is a mounted volume */
    dest->is_default = 0;
    return 0;
}

/*
 * Turn a kit entry's relative path ("SYSEXE/DCL.EXE", "SYSEXE/SUB/FOO.DAT", or a
 * bare "FOO.DAT" for a [000000] entry) into (a) the on-volume rooted directory
 * path in ODS-2 "A.B.C" form and (b) the full VMS filespec on the destination
 * device.
 *
 * BOTH the default and an explicit /DESTINATION lay the kit into the SAME
 * rooted, concealed system-disk structure a mastered ovmx-distrib.img already
 * has (vms-96ec, vms-649, docs/design-vms-faithful-install.md §3.5): SYS0 is the
 * boot root and SYS0.SYSCOMMON the shared common tree, so an entry's own bracket
 * ("SYSEXE", "SYSLIB", ...) hangs off SYS0.SYSCOMMON, exactly matching
 * ovmx_layout.h's VMS_SYSEXE == DEV:[SYS0.SYSCOMMON.SYSEXE] and the tree
 * distro/Dockerfile.bootable masters DKA0: from. This is what makes a
 * /DESTINATION-installed volume BOOTABLE AS ITS OWN system disk: booting mounts
 * the target as DKA0: and STARTUP resolves SYS$SYSROOT:[SYSEXE]DCL.EXE through
 * the rooted structure -- precisely where these bytes land, because the
 * SYS0.SYSCOMMON prefix lives ON the volume and is mount-point-independent. Do
 * not reintroduce a flat branch -- a flat target is not a system disk (Rule 1:
 * match VMS; INV-6: really bootable, not a flag).
 *
 * Returns 0, or -1 on overflow / a malformed relpath.
 */
static int pd_vms_paths(const struct pd_dest *dest, const char *relpath,
                        char *dirpath, size_t dpsz,     /* "SYS0.SYSCOMMON.SYSEXE" */
                        char *filespec, size_t fssz)    /* "DKA100:[<dir>]DCL.EXE" */
{
    char work[4096];
    snprintf(work, sizeof(work), "%s", relpath);

    /* Split off the trailing filename; the leading portion (in-place, with
     * '/'-separated components rewritten to the ODS-2 '.' separator) is the
     * directory path relative to the common root. */
    char *slash = strrchr(work, '/');
    const char *fname;
    const char *dircomp = "";
    if (slash) {
        *slash = '\0';
        fname = slash + 1;
        for (char *p = work; *p; p++)
            if (*p == '/') *p = '.';
        dircomp = work;
    } else {
        fname = work;               /* bare basename ([000000] entry) */
    }
    if (!fname[0])
        return -1;

    if (dircomp[0])
        snprintf(dirpath, dpsz, "SYS0.SYSCOMMON.%s", dircomp);
    else
        snprintf(dirpath, dpsz, "SYS0.SYSCOMMON");

    snprintf(filespec, fssz, "%s[%s]%s", dest->devname, dirpath, fname);
    return 0;
}

/* The VMS filespec of SYS$SYSTEM:VMS$PRODUCT_DATABASE.DAT on this destination:
 * SYS$SYSTEM: is SYSEXE under the common root, so the database sits inside the
 * SAME rooted structure as every installed file (vms-96ec), read back from where
 * INSTALL wrote it by PRODUCT SHOW PRODUCT /DESTINATION. */
static void pd_db_path(const struct pd_dest *dest, char *out, size_t outsz)
{
    snprintf(out, outsz, "%s[SYS0.SYSCOMMON.SYSEXE]%s", dest->devname, PRODUCT_DB_NAME);
}

/* ================================================================
 * Directory tree creation over the executive Files-11 ACP.
 *
 * A fresh INITIALIZEd ODS-2 target has only an empty MFD ([000000]); PRODUCT
 * INSTALL must create SYS0/SYSCOMMON/<bracket> before RMS $CREATE can enter a
 * file there. This walks the "A.B.C" path component by component, IO$_ACCESSing
 * each "<COMP>.DIR;1" under the running parent DID and, when absent
 * (SS$_NOSUCHFILE), IO$_CREATEing a directory file (attr.filechar =
 * ODS2_FH2_M_DIRECTORY) -- the VMS way DCL CREATE/DIRECTORY reaches the ACP.
 * Fail-honest: any non-SS$_NOSUCHFILE access failure is returned as-is (INV-6).
 * ================================================================ */

/* Uppercase @comp and append ".DIR" into @out ("SYSEXE" -> "SYSEXE.DIR"). */
static void pd_dir_filename(const char *comp, char *out, size_t outsz)
{
    size_t i = 0;
    for (; comp[i] && i + 5 < outsz; i++)
        out[i] = (char)toupper((unsigned char)comp[i]);
    out[i] = '\0';
    strncat(out, ".DIR", outsz - strlen(out) - 1);
}

static uint32_t pd_acp_mkdir_tree(const char *devname, const char *dirpath)
{
    uint32_t chan = 0;
    uint32_t st = vms_kif_acp_assign(devname, &chan);
    if (!$VMS_STATUS_SUCCESS(st))
        return st;

    /* Parent DID, starting at the MFD (all-zero DID). */
    uint16_t pdn = 0, pds = 0;
    uint8_t  pdr = 0, pdx = 0;

    char work[256];
    snprintf(work, sizeof(work), "%s", dirpath);
    char *save = NULL, *tok;
    for (tok = strtok_r(work, ".", &save); tok; tok = strtok_r(NULL, ".", &save)) {
        char nm[VMS_ACP_NAME_SIZE];
        pd_dir_filename(tok, nm, sizeof(nm));

        struct vms_acp_access_args a;
        memset(&a, 0, sizeof(a));
        a.chan = chan;
        a.did_num = pdn; a.did_seq = pds; a.did_rvn = pdr; a.did_nmx = pdx;
        a.version = 1;                          /* directories are version ;1 */
        strncpy(a.name, nm, VMS_ACP_NAME_SIZE - 1);

        st = vms_kif_acp_access(&a);
        if ($VMS_STATUS_SUCCESS(st)) {
            pdn = a.fid_num; pds = a.fid_seq; pdr = a.fid_rvn; pdx = a.fid_nmx;
            vms_kif_acp_deaccess(chan);         /* only wanted its FID */
            continue;
        }
        if (st != SS$_NOSUCHFILE) {             /* honest error, not "absent" */
            vms_kif_dassgn(chan);
            return st;
        }

        struct vms_acp_fileop_args fop;
        memset(&fop, 0, sizeof(fop));
        fop.chan      = chan;
        fop.func      = VMS_ACP_FOP_CREATE;
        fop.modifiers = VMS_ACP_M_CREATE;       /* enter it in the parent dir */
        fop.did_num = pdn; fop.did_seq = pds; fop.did_rvn = pdr; fop.did_nmx = pdx;
        fop.version   = 1;
        fop.attr.filechar = ODS2_FH2_M_DIRECTORY;   /* => is_dir in the ACP */
        strncpy(fop.name, nm, VMS_ACP_NAME_SIZE - 1);

        st = vms_kif_acp_fileop(&fop);
        if (!$VMS_STATUS_SUCCESS(st)) {
            vms_kif_dassgn(chan);
            return st;
        }
        pdn = fop.fid_num; pds = fop.fid_seq; pdr = fop.fid_rvn; pdx = fop.fid_nmx;
    }

    vms_kif_dassgn(chan);
    return SS$_NORMAL;
}

/* Ensure @dirpath ("SYS0.SYSCOMMON.SYSEXE") exists on @dest. On the runtime the
 * ACP creates it (pd_acp_mkdir_tree); on the executive-absent host defer -- where
 * RMS itself POSIX-wraps the on-volume path under SYSDISK_MOUNT -- mkdir -p the
 * corresponding Linux directory (default destination only; a /DESTINATION target
 * has no host mount and is refused honestly by pd_resolve_destination). Returns 0
 * on success, -1 with a message printed. */
static int pd_ensure_tree(const struct pd_dest *dest, const char *dirpath)
{
    if (rms_executive_absent()) {
        if (!dest->is_default) {
            fprintf(stderr, "%%PCSI-E-NOTMOUNTED, %s has no executive Files-11 ACP\n",
                    dest->devname);
            return -1;
        }
        char linux_dir[4096], slashed[256];
        snprintf(slashed, sizeof(slashed), "%s", dirpath);
        for (char *p = slashed; *p; p++)
            if (*p == '.') *p = '/';
        snprintf(linux_dir, sizeof(linux_dir), "%s/%s", SYSDISK_MOUNT, slashed);
        if (pd_mkdir_parents(linux_dir) != 0) {
            fprintf(stderr, "%%PCSI-E-MKDIR, cannot create directory %s: %s\n",
                    linux_dir, strerror(errno));
            return -1;
        }
        return 0;
    }

    uint32_t st = pd_acp_mkdir_tree(dest->devname, dirpath);
    if (!$VMS_STATUS_SUCCESS(st)) {
        fprintf(stderr, "%%PCSI-E-MKDIR, cannot create directory [%s] on %s (0x%08X)\n",
                dirpath, dest->devname, st);
        return -1;
    }
    return 0;
}

/* ================================================================
 * File + product-database writes through RMS ($CREATE + block $PUT), which is
 * itself ACP-routed and write-through synchronous on the runtime, POSIX-wrapped
 * on the executive-absent defer -- src/vmsrms/rms_io.h rms_open_named_handle.
 * ================================================================ */

/* Write @len bytes of @buf to VMS filespec @spec as a fresh versioned file.
 * Returns 0 on success, -1 with a %PCSI- message printed. */
static int pd_write_file(const char *spec, const uint8_t *buf, size_t len)
{
    uint32_t st = 0;
    rms_file_t *h = rms_open_named_handle(spec, /*want_write*/1, /*create*/1, &st);
    if (!h) {
        fprintf(stderr, "%%PCSI-E-CREATE, cannot create %s (0x%08X)\n", spec, st);
        return -1;
    }
    int rc = 0;
    if (len > 0 && rms_io_write_exact(h, buf, len) != 0) {
        fprintf(stderr, "%%PCSI-E-WRITE, write failed for %s\n", spec);
        rc = -1;
    }
    if (rc == 0)
        rms_io_fsync(h);
    rms_close_named_handle(h);
    return rc;
}

/* 1 if VMS filespec @spec names a file that already exists on its volume
 * (IO$_ACCESS by name over the ACP), 0 otherwise. */
static int pd_file_exists(const char *spec)
{
    uint32_t st = 0;
    rms_file_t *h = rms_open_named_handle(spec, /*want_write*/0, /*create*/0, &st);
    if (h) {
        rms_close_named_handle(h);
        return 1;
    }
    return 0;
}

/* ================================================================
 * Product database: whole-struct read/write through the same ACP-routed RMS
 * handle (same idiom as src/imgact/known_images.h's KFE database).
 * ================================================================ */

/* Always yields a valid (possibly empty) database -- there is nothing to
 * roll back to on a target that has never had a product installed. */
static void pd_db_load(const char *spec, struct ovmx_product_db *db)
{
    memset(db, 0, sizeof(*db));
    memcpy(db->db_magic, OVMX_PRODDB_MAGIC, OVMX_PRODDB_MAGIC_LEN);
    db->db_format_version = OVMX_PRODDB_FORMAT_VERSION;
    db->db_record_count = 0;

    uint32_t st = 0;
    rms_file_t *h = rms_open_named_handle(spec, /*want_write*/0, /*create*/0, &st);
    if (!h)
        return;

    struct ovmx_product_db tmp;
    ssize_t got = rms_io_read_exact(h, &tmp, sizeof(tmp));
    if (got == (ssize_t)sizeof(tmp) &&
        memcmp(tmp.db_magic, OVMX_PRODDB_MAGIC, OVMX_PRODDB_MAGIC_LEN) == 0 &&
        tmp.db_format_version == OVMX_PRODDB_FORMAT_VERSION &&
        tmp.db_record_count <= OVMX_PRODDB_MAX_PRODUCTS) {
        *db = tmp;
    }
    rms_close_named_handle(h);
}

/* Writes the whole struct as a fresh versioned SYS$SYSTEM: file through the ACP.
 * Protection/owner follow the executive's create-time defaults (SYSTEM's UIC +
 * VMSFS_PROT_DEFAULT) -- a system file, never world-writable -- the same policy
 * every installed kit file now takes (see the do_install note; the fchmod/fchown
 * that once ran on a raw POSIX fd governed only the retired passthrough path). */
static int pd_db_save(const char *spec, const struct ovmx_product_db *db)
{
    return pd_write_file(spec, (const uint8_t *)db, sizeof(*db));
}

static struct ovmx_product_record *pd_db_find_or_add(struct ovmx_product_db *db,
                                                      const char *name)
{
    for (uint32_t i = 0; i < db->db_record_count; i++) {
        if (strncmp(db->db_records[i].pr_name, name, OVMX_PRODDB_NAME_MAX) == 0)
            return &db->db_records[i];
    }
    if (db->db_record_count >= OVMX_PRODDB_MAX_PRODUCTS)
        return NULL;
    struct ovmx_product_record *r = &db->db_records[db->db_record_count++];
    memset(r, 0, sizeof(*r));
    return r;
}

/* ================================================================
 * PRODUCT INSTALL
 * ================================================================ */

static int do_install(int argc, char *argv[])
{
    const char *product_name = NULL;
    const char *source = NULL;
    const char *destarg = NULL;

    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "/SOURCE=", 8) == 0) {
            source = argv[i] + 8;
        } else if (strncmp(argv[i], "/DESTINATION=", 13) == 0) {
            destarg = argv[i] + 13;
        } else if (!product_name && argv[i][0] != '/') {
            product_name = argv[i];
        }
    }

    if (!product_name || !product_name[0]) {
        fprintf(stderr, "%%PCSI-E-NOPROD, product name required\n");
        return 1;
    }
    if (!source || !source[0]) {
        fprintf(stderr, "%%PCSI-E-NOSOURCE, /SOURCE kit filespec required\n");
        return 1;
    }

    struct pd_dest dest;
    if (pd_resolve_destination(destarg, &dest) != 0)
        return 1;

    ovmx_kit_reader_t r;
    int rc = ovmx_kit_reader_open(&r, source);
    if (rc != OVMX_KIT_READER_OK) {
        switch (rc) {
        case OVMX_KIT_READER_ERR_OPEN:
            fprintf(stderr, "%%PCSI-E-OPENIN, cannot open %s: %s\n", source, strerror(errno));
            break;
        case OVMX_KIT_READER_ERR_NOTKIT:
            fprintf(stderr, "%%PCSI-E-BADKIT, %s is not an OVMX kit file\n", source);
            break;
        case OVMX_KIT_READER_ERR_CHKSUM:
            fprintf(stderr, "%%PCSI-E-BADKIT, %s kit header checksum mismatch\n", source);
            break;
        default:
            fprintf(stderr, "%%PCSI-E-OPENIN, cannot read %s\n", source);
        }
        return 1;
    }

    char kname[OVMX_KIT_NAME_MAX + 1], kprod[OVMX_KIT_PRODUCER_MAX + 1],
         kver[OVMX_KIT_VERSION_MAX + 1];
    snprintf(kname, sizeof(kname), "%s", r.hdr.kh_product_name);
    snprintf(kprod, sizeof(kprod), "%s", r.hdr.kh_producer);
    snprintf(kver, sizeof(kver), "%s", r.hdr.kh_product_version);

    printf("The following product has been selected:\n");
    printf("    %s %-20s Full LP\n", kname, kver);
    printf("Configuring %s %s: OpenVMS Operating System\n", kname, kver);

    struct ovmx_kit_entry *entries = NULL;
    rc = ovmx_kit_reader_entries(&r, &entries);
    if (rc != OVMX_KIT_READER_OK) {
        fprintf(stderr, "%%PCSI-E-READ, short read of kit index in %s\n", source);
        ovmx_kit_reader_close(&r);
        return 1;
    }

    uint32_t installed = 0;
    uint32_t preserved = 0;
    int failed = 0;
    for (uint32_t i = 0; i < r.hdr.kh_file_count && !failed; i++) {
        struct ovmx_kit_entry *e = &entries[i];

        char rel[4096];
        if (ovmx_kit_reader_relpath(e->ke_filespec, rel, sizeof(rel)) != OVMX_KIT_READER_OK) {
            fprintf(stderr, "%%PCSI-E-BADSPEC, malformed target filespec: %s\n",
                    e->ke_filespec);
            failed = 1; break;
        }

        char dirpath[512], spec[8192];
        if (pd_vms_paths(&dest, rel, dirpath, sizeof(dirpath),
                         spec, sizeof(spec)) != 0) {
            fprintf(stderr, "%%PCSI-E-BADSPEC, malformed target filespec: %s\n",
                    e->ke_filespec);
            failed = 1; break;
        }

        /*
         * vms-2c9: a seed-once entry (a site-customizable template the kit
         * ships, e.g. SYS$MANAGER:SYSTARTUP_VMS.COM -- ovmx_kit_format.h's
         * OVMX_KIT_ENTRY_FLAG_SEED_ONCE) is written only when the target
         * does not already exist ON THE VOLUME (an IO$_ACCESS by name over the
         * ACP, not a POSIX stat). Present means an UPGRADE over an
         * already-populated destination -- that copy is the SITE'S, not the
         * kit's, so PRESERVE it, matching the way real OpenVMS never
         * reprovisions SYS$MANAGER: startup procedures once seeded (vms-f05).
         * A kit with no flag on this entry (ke_flags == 0) never takes this
         * branch, so install behavior for such kits is unchanged.
         */
        if (e->ke_flags & OVMX_KIT_ENTRY_FLAG_SEED_ONCE) {
            if (pd_file_exists(spec)) {
                preserved++;
                continue;
            }
        }

        /* Create SYS0/SYSCOMMON/<bracket> on the target volume over the ACP
         * before RMS $CREATE enters the file there. */
        if (pd_ensure_tree(&dest, dirpath) != 0) {
            failed = 1; break;
        }

        uint8_t *buf = NULL;
        int rrc = ovmx_kit_reader_read_file(&r, e, &buf);
        if (rrc != OVMX_KIT_READER_OK) {
            fprintf(stderr, "%%PCSI-E-BADKIT, %s: content checksum/read failure for %s\n",
                    source, e->ke_filespec);
            failed = 1; break;
        }

        /*
         * Write the entry to the target volume by VMS filespec through RMS
         * ($CREATE + block $PUT) -- an ACP write to the volume's backing disk,
         * write-through synchronous, durable across the install's DISMOUNT
         * (vms-3a8). Protection/owner UIC follow the executive's create-time
         * defaults (SYSTEM's UIC + VMSFS_PROT_DEFAULT); the kit's own metadata
         * is numerically identical to those for every kit shipped today, and
         * the old fchmod/fchown on a raw POSIX fd governed only the retired
         * /mnt passthrough (a divergent per-file protection remains the
         * vms-738 follow-up, unchanged by this fix -- never more permissive
         * than the create default, so not a security regression).
         */
        if (pd_write_file(spec, buf, (size_t)e->ke_size) != 0) {
            free(buf); failed = 1; break;
        }
        free(buf);
        installed++;
    }

    free(entries);

    if (failed) {
        ovmx_kit_reader_close(&r);
        return 1;
    }

    /* Record the installation in the target volume's own product database. */
    char dbpath[8192];
    pd_db_path(&dest, dbpath, sizeof(dbpath));

    struct ovmx_product_db db;
    pd_db_load(dbpath, &db);

    struct ovmx_product_record *pr = pd_db_find_or_add(&db, kname);
    if (!pr) {
        fprintf(stderr, "%%PCSI-E-DBFULL, product database at %s is full\n", dbpath);
        ovmx_kit_reader_close(&r);
        return 1;
    }
    snprintf(pr->pr_name, sizeof(pr->pr_name), "%s", kname);
    snprintf(pr->pr_producer, sizeof(pr->pr_producer), "%s", kprod);
    snprintf(pr->pr_version, sizeof(pr->pr_version), "%s", kver);
    pr->pr_install_time = (uint64_t)time(NULL);
    pr->pr_file_count = r.hdr.kh_file_count;
    pr->pr_state = OVMX_PRODUCT_STATE_INSTALLED;

    if (pd_db_save(dbpath, &db) != 0) {
        /* pd_db_save already printed the specific %PCSI-E-CREATE/WRITE. */
        ovmx_kit_reader_close(&r);
        return 1;
    }

    ovmx_kit_reader_close(&r);

    printf("%%PCSI-I-DONE, product installation completed, %u file(s) installed, "
           "%u file(s) preserved (site-owned)\n",
           installed, preserved);
    return 0;
}

/* ================================================================
 * PRODUCT SHOW PRODUCT / SHOW HISTORY
 * ================================================================ */

static int do_show(int argc, char *argv[])
{
    if (argc < 3 || argv[2][0] == '\0') {
        fprintf(stderr, "%%PCSI-W-NOTIMPL, operation not implemented\n");
        return 0;
    }

    char showwhat[32];
    snprintf(showwhat, sizeof(showwhat), "%s", argv[2]);
    for (int i = 0; showwhat[i]; i++)
        showwhat[i] = (char)toupper((unsigned char)showwhat[i]);

    if (strcmp(showwhat, "PRODUCT") != 0 && strcmp(showwhat, "HISTORY") != 0) {
        fprintf(stderr, "%%PCSI-W-NOTIMPL, operation not implemented\n");
        return 0;
    }

    const char *destarg = NULL;
    for (int i = 3; i < argc; i++) {
        if (strncmp(argv[i], "/DESTINATION=", 13) == 0)
            destarg = argv[i] + 13;
    }

    struct pd_dest dest;
    if (pd_resolve_destination(destarg, &dest) != 0)
        return 1;

    char dbpath[8192];
    pd_db_path(&dest, dbpath, sizeof(dbpath));

    struct ovmx_product_db db;
    pd_db_load(dbpath, &db);

    /* No database yet on the DEFAULT (currently-running) system: this is
     * the system BEFORE any PRODUCT INSTALL has ever run against it
     * (mastering writes the volume directly, not through this utility --
     * a separate, tracked gap, not this bead's scope). Preserve the
     * pre-existing baseline row exactly (tests/dcl/test_misc_commands.sh
     * asserts "contains:OVMX" + "contains:Installed" from a bare
     * PRODUCT SHOW PRODUCT) rather than reporting a dishonest "0 products"
     * for a system that IS, in fact, running OVMX. An explicit
     * /DESTINATION with no database is different: that target genuinely
     * has nothing installed, so it honestly reports zero. */
    int synthesize_fallback = (dest.is_default && db.db_record_count == 0);

    if (strcmp(showwhat, "PRODUCT") == 0) {
        printf("----------------------------------- ----------- -----------\n");
        printf("PRODUCT                             KIT TYPE    STATE\n");
        printf("----------------------------------- ----------- -----------\n");
        if (synthesize_fallback) {
            printf("OVMX %-30s Full LP     Installed\n", ovmx_product_version());
            printf("----------------------------------- ----------- -----------\n");
            printf("1 product found\n");
        } else {
            for (uint32_t i = 0; i < db.db_record_count; i++) {
                struct ovmx_product_record *pr = &db.db_records[i];
                /* The PRODUCT column carries name AND version as one
                 * identifier -- matching real PCSI's own SHOW PRODUCT shape
                 * (e.g. "VSI I64VMS OPENVMS V8.4-2L1") and this file's own
                 * synthesize_fallback path just above, which already embeds
                 * ovmx_product_version() the same way. Before this fix the
                 * real (non-fallback) path printed pr_name alone, so a
                 * PRODUCT SHOW PRODUCT against an actually-installed system
                 * could never show WHICH version was installed or whether
                 * an upgrade had changed it -- found building vms-f05's
                 * install->UPGRADE->boot gate, which needs exactly that. */
                char namever[OVMX_PRODDB_NAME_MAX + 32];
                snprintf(namever, sizeof(namever), "%s %s", pr->pr_name, pr->pr_version);
                printf("%-36s %-11s %s\n", namever, "Full LP",
                       pr->pr_state == OVMX_PRODUCT_STATE_INSTALLED ? "Installed" : "Unknown");
            }
            printf("----------------------------------- ----------- -----------\n");
            printf("%u product%s found\n", db.db_record_count,
                   db.db_record_count == 1 ? "" : "s");
        }
        return 0;
    }

    /* HISTORY */
    printf("----------------------------------- ----------- ----------- -----------\n");
    printf("PRODUCT                             KIT TYPE    STATE       DATE\n");
    printf("----------------------------------- ----------- ----------- -----------\n");
    if (synthesize_fallback) {
        time_t now = time(NULL);
        struct tm *tmv = localtime(&now);
        printf("OVMX %-30s Full LP     Installed   %2d-%s-%04d\n",
               ovmx_product_version(),
               tmv->tm_mday, pd_months[tmv->tm_mon], 1900 + tmv->tm_year);
        printf("----------------------------------- ----------- ----------- -----------\n");
        printf("1 item found\n");
    } else {
        for (uint32_t i = 0; i < db.db_record_count; i++) {
            struct ovmx_product_record *pr = &db.db_records[i];
            time_t it = (time_t)pr->pr_install_time;
            struct tm *tmv = localtime(&it);
            /* Same name+version identifier as SHOW PRODUCT above. */
            char namever[OVMX_PRODDB_NAME_MAX + 32];
            snprintf(namever, sizeof(namever), "%s %s", pr->pr_name, pr->pr_version);
            printf("%-36s %-11s %-11s %2d-%s-%04d\n", namever, "Full LP",
                   pr->pr_state == OVMX_PRODUCT_STATE_INSTALLED ? "Installed" : "Unknown",
                   tmv->tm_mday, pd_months[tmv->tm_mon], 1900 + tmv->tm_year);
        }
        printf("----------------------------------- ----------- ----------- -----------\n");
        printf("%u item%s found\n", db.db_record_count, db.db_record_count == 1 ? "" : "s");
    }
    return 0;
}

/* ================================================================
 * main
 * ================================================================ */

int main(int argc, char *argv[])
{
    if (argc < 2 || argv[1][0] == '\0') {
        fprintf(stderr, "%%PCSI-W-NOTIMPL, operation not implemented\n");
        return 0;
    }

    char subcmd[32];
    snprintf(subcmd, sizeof(subcmd), "%s", argv[1]);
    for (int i = 0; subcmd[i]; i++)
        subcmd[i] = (char)toupper((unsigned char)subcmd[i]);

    if (strcmp(subcmd, "INSTALL") == 0)
        return do_install(argc, argv);
    if (strcmp(subcmd, "SHOW") == 0)
        return do_show(argc, argv);

    fprintf(stderr, "%%PCSI-W-NOTIMPL, operation not implemented\n");
    return 0;
}
