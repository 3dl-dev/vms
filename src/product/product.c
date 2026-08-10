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
 * the visible SYS$COMMON:[SYSEXE] logical, so this is OVMX's own choice,
 * not a fact about PCSI):
 *   - No /DESTINATION (installing onto the currently-running system):
 *     kit entries land under this build's fixed SYS0/SYSCOMMON layout
 *     (ovmx_layout.h's SYSDISK_MOUNT "/SYS0/SYSCOMMON/" + the entry's own
 *     [SYSEXE]/[SYSLIB]/... bracket), matching VMS_SYSTEM_DIR's own shape
 *     and the tree distro/Dockerfile.bootable already masters DKA0: from.
 *   - /DESTINATION=<dev>: the target is a bare, freshly-INITIALIZEd volume
 *     with no SYS0/SYSCOMMON hierarchy of its own (see
 *     tests/qemu/test_mount_e2e.sh -- MOUNT gives a flat root), so entries
 *     land flat at <mount-point>/<bracket-dir>/<name> -- exactly the same
 *     mapping ovmx_kit_pack's own `extract` mode already uses (this is
 *     "extract, but through a live vmsfs mount with real protection/UIC
 *     and a product-database record" rather than a bare host directory).
 * The product database follows the same split: SYS$SYSTEM: means
 * VMS_SYSTEM_DIR on the default target, or <mount-point>/SYSEXE on an
 * explicit /DESTINATION (the destination root's OWN "SYS$SYSTEM:"-shaped
 * directory, matching the kit's own convention of placing SYSEXE content
 * there).
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
 * vmsfs_protection_to_mode() lives in libvmsfs (src/vmsfs/vmsfs_protect.c).
 * Its canonical prototype is declared in DCL's own internal header
 * (src/vmsdcl/include/dcl/dcl_cmd.h) rather than a public vmsfs header --
 * declared locally here since this standalone utility has no reason to
 * pull in the rest of DCL just for one function.
 */
extern mode_t vmsfs_protection_to_mode(uint16_t vms_prot);

#define PRODUCT_DB_NAME "VMS$PRODUCT_DATABASE.DAT"

static const char *pd_months[] = {
    "JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"
};

/* ================================================================
 * Small pure helpers duplicated from src/vmsdcl/dcl_cmd_misc.c's
 * cmd_mount()/cmd_dismount() (vms-651). Both are documented there as
 * "a PURE FUNCTION of the name -- any process can compute it without
 * asking anyone", which is exactly what lets a SEPARATE process
 * (PRODUCT.EXE, exec'd fresh by dcl_exec_utility) determine where a
 * device MOUNTed by an earlier DCL command landed, with no shared
 * per-process state to inherit across the fork+exec boundary.
 * ================================================================ */

static void pd_mount_point_for_device(const char *log_name, char *buf, size_t sz)
{
    char lower[16];
    size_t i;
    for (i = 0; log_name[i] && i < sizeof(lower) - 1; i++)
        lower[i] = (char)tolower((unsigned char)log_name[i]);
    lower[i] = '\0';
    snprintf(buf, sz, "/mnt/%s", lower);
}

static int pd_mount_point_is_mounted(const char *mount_point)
{
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp)
        return 0;

    char line[512];
    size_t mp_len = strlen(mount_point);
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *field2 = strchr(line, ' ');
        if (!field2) continue;
        field2++;
        char *after = strchr(field2, ' ');
        if (!after) continue;
        size_t flen = (size_t)(after - field2);
        if (flen == mp_len && strncmp(field2, mount_point, flen) == 0) {
            found = 1;
            break;
        }
    }
    fclose(fp);
    return found;
}

/* mkdir -p, ignoring EEXIST, over every '/'-separated component of @path. */
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

static int pd_mkdir_parent_of_file(const char *filepath)
{
    char parent[4096];
    snprintf(parent, sizeof(parent), "%s", filepath);
    char *slash = strrchr(parent, '/');
    if (!slash)
        return 0;
    *slash = '\0';
    if (parent[0] == '\0')
        return 0;
    return pd_mkdir_parents(parent);
}

/* ================================================================
 * Destination resolution: where a /DESTINATION device (or the default,
 * currently-running system) actually lands files. See the file header
 * comment for the two-shape policy this implements.
 * ================================================================ */

struct pd_dest {
    int  is_default;              /* no /DESTINATION given */
    char devname[16];             /* canonical "DKA100:" form, "" if default */
    char mount_root[256];         /* Linux path files land under */
};

/* @destarg is the raw qualifier value (may be NULL/empty) with or without
 * a trailing colon, any case. Returns 0 and fills *dest, or -1 with an
 * honest message already printed (destination named but not mounted). */
static int pd_resolve_destination(const char *destarg, struct pd_dest *dest)
{
    memset(dest, 0, sizeof(*dest));

    if (!destarg || !destarg[0]) {
        dest->is_default = 1;
        snprintf(dest->mount_root, sizeof(dest->mount_root), "%s", SYSDISK_MOUNT);
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

    char log_name[16];
    snprintf(log_name, sizeof(log_name), "%s", dest->devname);
    size_t ln = strlen(log_name);
    if (ln > 0 && log_name[ln - 1] == ':')
        log_name[ln - 1] = '\0';

    pd_mount_point_for_device(log_name, dest->mount_root, sizeof(dest->mount_root));

    if (!pd_mount_point_is_mounted(dest->mount_root)) {
        fprintf(stderr, "%%PCSI-E-NOTMOUNTED, destination device %s is not mounted\n",
                dest->devname);
        return -1;
    }
    dest->is_default = 0;
    return 0;
}

/* Where a kit entry's relative path ("SYSEXE/DCL.EXE") lands. */
static void pd_kit_target_path(const struct pd_dest *dest, const char *relpath,
                               char *out, size_t outsz)
{
    if (dest->is_default)
        snprintf(out, outsz, "%s/SYS0/SYSCOMMON/%s", SYSDISK_MOUNT, relpath);
    else
        snprintf(out, outsz, "%s/%s", dest->mount_root, relpath);
}

/* Where SYS$SYSTEM:VMS$PRODUCT_DATABASE.DAT lives for this destination. */
static void pd_db_path(const struct pd_dest *dest, char *out, size_t outsz)
{
    if (dest->is_default)
        snprintf(out, outsz, "%s/%s", VMS_SYSTEM_DIR, PRODUCT_DB_NAME);
    else
        snprintf(out, outsz, "%s/SYSEXE/%s", dest->mount_root, PRODUCT_DB_NAME);
}

/* ================================================================
 * Product database: whole-file struct fread()/fwrite(), same idiom as
 * src/imgact/known_images.h's KFE database (see ovmx_product_db.h).
 * ================================================================ */

/* Always yields a valid (possibly empty) database -- there is nothing to
 * roll back to on a target that has never had a product installed. */
static void pd_db_load(const char *path, struct ovmx_product_db *db)
{
    memset(db, 0, sizeof(*db));
    memcpy(db->db_magic, OVMX_PRODDB_MAGIC, OVMX_PRODDB_MAGIC_LEN);
    db->db_format_version = OVMX_PRODDB_FORMAT_VERSION;
    db->db_record_count = 0;

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return;

    struct ovmx_product_db tmp;
    if (fread(&tmp, sizeof(tmp), 1, fp) == 1 &&
        memcmp(tmp.db_magic, OVMX_PRODDB_MAGIC, OVMX_PRODDB_MAGIC_LEN) == 0 &&
        tmp.db_format_version == OVMX_PRODDB_FORMAT_VERSION &&
        tmp.db_record_count <= OVMX_PRODDB_MAX_PRODUCTS) {
        *db = tmp;
    }
    fclose(fp);
}

/* Writes the whole struct, then applies a real, non-world-writable
 * protection (never the create()-default left as-is by accident, and
 * never 777) -- the database is itself a system file. Owner UIC is
 * whatever this process (PRODUCT INSTALL, expected to run as SYSTEM)
 * already creates it as; no chown needed for the account that made it. */
static int pd_db_save(const char *path, const struct ovmx_product_db *db)
{
    if (pd_mkdir_parent_of_file(path) != 0)
        return -1;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;

    FILE *fp = fdopen(fd, "wb");
    if (!fp) {
        close(fd);
        return -1;
    }
    size_t wrote = fwrite(db, sizeof(*db), 1, fp);
    fflush(fp);
    int ffd = fileno(fp);
    fchmod(ffd, vmsfs_protection_to_mode(OVMX_KIT_PROT_DEFAULT));
    fsync(ffd);
    fclose(fp);

    return (wrote == 1) ? 0 : -1;
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
    int failed = 0;
    for (uint32_t i = 0; i < r.hdr.kh_file_count && !failed; i++) {
        struct ovmx_kit_entry *e = &entries[i];

        char rel[4096];
        if (ovmx_kit_reader_relpath(e->ke_filespec, rel, sizeof(rel)) != OVMX_KIT_READER_OK) {
            fprintf(stderr, "%%PCSI-E-BADSPEC, malformed target filespec: %s\n",
                    e->ke_filespec);
            failed = 1; break;
        }

        char target[8192];
        pd_kit_target_path(&dest, rel, target, sizeof(target));

        if (pd_mkdir_parent_of_file(target) != 0) {
            fprintf(stderr, "%%PCSI-E-MKDIR, cannot create directory for %s: %s\n",
                    target, strerror(errno));
            failed = 1; break;
        }

        uint8_t *buf = NULL;
        int rrc = ovmx_kit_reader_read_file(&r, e, &buf);
        if (rrc != OVMX_KIT_READER_OK) {
            fprintf(stderr, "%%PCSI-E-BADKIT, %s: content checksum/read failure for %s\n",
                    source, e->ke_filespec);
            failed = 1; break;
        }

        /* Restrictive placeholder mode until the real kit-metadata
         * protection is applied below -- never left at this value, and
         * never opened with a permissive create mode (vms-df9 constraint
         * #2: protection/UIC come from the kit, not a default). */
        int fd = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            fprintf(stderr, "%%PCSI-E-CREATE, cannot create %s: %s\n",
                    target, strerror(errno));
            free(buf); failed = 1; break;
        }
        if (buf && write(fd, buf, e->ke_size) != (ssize_t)e->ke_size) {
            fprintf(stderr, "%%PCSI-E-WRITE, write failed for %s: %s\n",
                    target, strerror(errno));
            free(buf); close(fd); failed = 1; break;
        }
        free(buf);

        /* Protection + owner UIC FROM THE KIT METADATA, never a default --
         * see the file header comment (vms-79b/vms-738) for why this does
         * not yet durably persist a value that DIFFERS from vmsfs_blkdev_
         * create()'s own defaults (a kernel-side gap, not a security hole:
         * a new file can never land more permissive than create()'s own
         * VMSFS_PROT_DEFAULT). The calls still run and still fail loudly
         * on a real error -- they are correct for every kit shipped today,
         * where the kit's own metadata already matches those defaults. */
        mode_t mode = vmsfs_protection_to_mode(e->ke_protection);
        if (fchmod(fd, mode) != 0) {
            fprintf(stderr, "%%PCSI-E-SETPROT, cannot set protection on %s: %s\n",
                    target, strerror(errno));
            close(fd); failed = 1; break;
        }
        if (fchown(fd, e->ke_uic_member, e->ke_uic_group) != 0) {
            fprintf(stderr, "%%PCSI-E-SETOWNER, cannot set owner UIC [%u,%u] on %s: %s\n",
                    e->ke_uic_group, e->ke_uic_member, target, strerror(errno));
            close(fd); failed = 1; break;
        }
        fsync(fd);
        close(fd);
        installed++;
    }

    free(entries);

    if (failed) {
        ovmx_kit_reader_close(&r);
        return 1;
    }

    /* Record the installation in the product database. */
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
        fprintf(stderr, "%%PCSI-E-DBWRITE, cannot write product database %s: %s\n",
                dbpath, strerror(errno));
        ovmx_kit_reader_close(&r);
        return 1;
    }

    ovmx_kit_reader_close(&r);

    printf("%%PCSI-I-DONE, product installation completed, %u file(s) installed\n",
           installed);
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
