/*
 * install.c - INSTALL utility: known (pre-registered) image database
 * management (bead vms-913.5).
 *
 *   INSTALL ADD    <image-filespec> [/OPEN] [/SHARED] [/HEADER_RESIDENT]
 *   INSTALL LIST   [/FULL]
 *   INSTALL REMOVE <image-filespec>
 *
 * Database: SYS$SYSTEM:VMS$KNOWN_IMAGES.DAT (VMS_SYSTEM_DIR/VMS$KNOWN_IMAGES.DAT).
 * On-disk format: struct kfe_file (src/imgact/known_images.h) — a fixed-size
 * binary struct, read and written whole (same "whole-file struct" idiom as
 * SYSGEN's sysgen_params.h / src/vmsdcl's SYSPARAMS.DAT).
 *
 * IPC to IMGACT.EXE's known-image search path: none needed beyond the
 * filesystem. INSTALL writes the whole file with plain buffered I/O;
 * src/imgact/known_images.c mmap(MAP_SHARED)s the same file for O(1) SONAME
 * lookup, so a fresh mapping picks up ADD/REMOVE changes immediately. See
 * known_images.h and docs/design-image-activation.md section 6.
 *
 * External utility only — deliberately NOT wired as a DCL builtin verb
 * (src/vmsdcl/dcl_builtin.c is owned by a concurrent bead). It runs as
 * SYS$SYSTEM:INSTALL.EXE like AUTHORIZE, SYSMAN, SYSGEN, etc.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "ovmx_layout.h"
#include "str_util.h"
#include "known_images.h"

#define KNOWN_IMAGES_DB  VMS_SYSTEM_DIR "/VMS$KNOWN_IMAGES.DAT"

/* ------------------------------------------------------------------ */
/* Database load/save — whole-struct fread/fwrite, mirrors the         */
/* sysgen_params.h / vms_sysman.c STARTUP_LIST idiom used elsewhere.   */
/* ------------------------------------------------------------------ */

/* Loads the known-image DB into *db. If the file is missing or not a
 * recognizable KFE database, *db is initialized to an empty, valid
 * database (INSTALL ADD will create the file on first write). Always
 * succeeds from the caller's point of view — there is nothing to roll
 * back to on a fresh system. */
static void db_load(struct kfe_file *db)
{
    memset(db, 0, sizeof(*db));
    db->magic = KFE_MAGIC;
    db->version = KFE_VERSION;
    db->count = 0;

    FILE *fp = fopen(KNOWN_IMAGES_DB, "rb");
    if (!fp)
        return;

    struct kfe_file tmp;
    if (fread(&tmp, sizeof(tmp), 1, fp) == 1 &&
        tmp.magic == KFE_MAGIC && tmp.version == KFE_VERSION &&
        tmp.count <= KFE_MAX_IMAGES) {
        *db = tmp;
    }
    fclose(fp);
}

/* mkdir -p VMS_SYSTEM_DIR (SYS$SYSTEM may not exist yet on a fresh build
 * tree / test fixture — mirrors vms_sysman.c's SYSMGR_DIR bootstrap). */
static void ensure_sysexe_dir(void)
{
    char tmp[512];
    strncpy(tmp, VMS_SYSTEM_DIR, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(tmp, 0755);
            *p = '/';
        }
    }
    (void)mkdir(tmp, 0755);
}

static int db_save(const struct kfe_file *db)
{
    ensure_sysexe_dir();
    FILE *fp = fopen(KNOWN_IMAGES_DB, "wb");
    if (!fp)
        return -1;
    size_t n = fwrite(db, sizeof(*db), 1, fp);
    fclose(fp);
    return (n == 1) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Filespec resolution                                                 */
/* ------------------------------------------------------------------ */

/*
 * resolve_filespec - turn an image filespec into {SONAME, Linux path}.
 *
 * Accepts "SYS$SHARE:name", "SYS$SYSTEM:name", or a bare "name" (defaults
 * to SYS$SHARE, where installed shareable images live). Any other device/
 * logical prefix is rejected — real INSTALL only manages images in the
 * known SYS$SHARE/SYS$SYSTEM locations.
 *
 * Returns 0 on success, -1 if the filespec cannot be resolved.
 */
static int resolve_filespec(const char *filespec, char *soname, size_t soname_len,
                             char *linux_path, size_t path_len)
{
    const char *colon = strrchr(filespec, ':');
    const char *dir;
    const char *name;

    if (colon) {
        char prefix[32] = {0};
        size_t plen = (size_t)(colon - filespec);
        if (plen == 0 || plen >= sizeof(prefix))
            return -1;
        memcpy(prefix, filespec, plen);
        name = colon + 1;

        if (strcasecmp(prefix, "SYS$SHARE") == 0)
            dir = VMS_LIBRARY_DIR;
        else if (strcasecmp(prefix, "SYS$SYSTEM") == 0)
            dir = VMS_SYSTEM_DIR;
        else
            return -1;
    } else {
        dir = VMS_LIBRARY_DIR;
        name = filespec;
    }

    if (name[0] == '\0')
        return -1;

    str_upcase_copy(soname, name, soname_len);

    int n = snprintf(linux_path, path_len, "%s/%s", dir, soname);
    if (n < 0 || (size_t)n >= path_len)
        return -1;
    return 0;
}

/* Find an entry by SONAME (zero-padded, full-width compare — matches the
 * on-disk convention used by known_images.c). Returns index or -1. */
static int find_entry(const struct kfe_file *db, const char *soname)
{
    for (uint32_t i = 0; i < db->count; i++) {
        if (strncmp(db->entries[i].soname, soname, sizeof(db->entries[i].soname)) == 0)
            return (int)i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Subcommands                                                         */
/* ------------------------------------------------------------------ */

static int cmd_add(int argc, char *argv[])
{
    if (argc < 1) {
        fprintf(stderr, "%%INSTALL-E-NOFILE, no image filespec specified\n");
        return 1;
    }

    uint32_t flags = 0;
    for (int i = 1; i < argc; i++) {
        if (strcasecmp(argv[i], "/OPEN") == 0)
            flags |= KFE_F_OPEN;
        else if (strcasecmp(argv[i], "/SHARED") == 0)
            flags |= KFE_F_SHARED;
        else if (strcasecmp(argv[i], "/HEADER_RESIDENT") == 0)
            flags |= KFE_F_HEADER_RESIDENT;
        else {
            fprintf(stderr, "%%INSTALL-E-IVQUAL, unrecognized qualifier \"%s\"\n", argv[i]);
            return 1;
        }
    }

    char soname[64] = {0};
    char path[256] = {0};
    if (resolve_filespec(argv[0], soname, sizeof(soname), path, sizeof(path)) != 0) {
        fprintf(stderr, "%%INSTALL-E-BADFILE, cannot resolve \"%s\"\n", argv[0]);
        return 1;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "%%INSTALL-E-FILNOTFND, file %s not found\n", path);
        return 1;
    }

    struct kfe_file db;
    db_load(&db);

    int idx = find_entry(&db, soname);
    if (idx < 0) {
        if (db.count >= KFE_MAX_IMAGES) {
            fprintf(stderr, "%%INSTALL-E-TOOMANY, known image database is full (%d max)\n",
                    KFE_MAX_IMAGES);
            return 1;
        }
        idx = (int)db.count++;
        memset(&db.entries[idx], 0, sizeof(db.entries[idx]));
    }

    strncpy(db.entries[idx].soname, soname, sizeof(db.entries[idx].soname) - 1);
    strncpy(db.entries[idx].path, path, sizeof(db.entries[idx].path) - 1);
    db.entries[idx].flags = flags;

    /* GSMATCH caching: no linker in this tree emits .vms.ident on shareable
     * images yet (docs/design-image-activation.md sections 3 and 6), so
     * there is nothing to read here. Default to ALWAYS/0.0 — the on-disk
     * format already carries these fields so no format change is needed
     * once a producer exists. */
    db.entries[idx].gsmatch_op = KFE_GSMATCH_ALWAYS;
    db.entries[idx].major = 0;
    db.entries[idx].minor = 0;

    if (db_save(&db) != 0) {
        fprintf(stderr, "%%INSTALL-E-WRITEERR, cannot write known image database\n");
        return 1;
    }

    printf("%%INSTALL-I-ADDED, %s installed\n", soname);
    return 0;
}

static int cmd_remove(int argc, char *argv[])
{
    if (argc < 1) {
        fprintf(stderr, "%%INSTALL-E-NOFILE, no image filespec specified\n");
        return 1;
    }

    char soname[64] = {0};
    char path[256] = {0};
    if (resolve_filespec(argv[0], soname, sizeof(soname), path, sizeof(path)) != 0) {
        fprintf(stderr, "%%INSTALL-E-BADFILE, cannot resolve \"%s\"\n", argv[0]);
        return 1;
    }

    struct kfe_file db;
    db_load(&db);

    int idx = find_entry(&db, soname);
    if (idx < 0) {
        fprintf(stderr, "%%INSTALL-E-NOTINSTALLED, %s is not installed\n", soname);
        return 1;
    }

    for (uint32_t i = (uint32_t)idx; i + 1 < db.count; i++)
        db.entries[i] = db.entries[i + 1];
    db.count--;
    memset(&db.entries[db.count], 0, sizeof(db.entries[db.count]));

    if (db_save(&db) != 0) {
        fprintf(stderr, "%%INSTALL-E-WRITEERR, cannot write known image database\n");
        return 1;
    }

    printf("%%INSTALL-I-REMOVED, %s removed\n", soname);
    return 0;
}

static void format_flags(uint32_t flags, char *buf, size_t len)
{
    buf[0] = '\0';
    if (flags & KFE_F_OPEN)
        strncat(buf, "OPEN ", len - strlen(buf) - 1);
    if (flags & KFE_F_SHARED)
        strncat(buf, "SHARED ", len - strlen(buf) - 1);
    if (flags & KFE_F_HEADER_RESIDENT)
        strncat(buf, "HEADER_RESIDENT ", len - strlen(buf) - 1);
    size_t n = strlen(buf);
    if (n > 0 && buf[n - 1] == ' ')
        buf[n - 1] = '\0';
}

static int cmd_list(int argc, char *argv[])
{
    int full = 0;
    for (int i = 0; i < argc; i++) {
        if (strcasecmp(argv[i], "/FULL") == 0)
            full = 1;
        else {
            fprintf(stderr, "%%INSTALL-E-IVQUAL, unrecognized qualifier \"%s\"\n", argv[i]);
            return 1;
        }
    }

    struct kfe_file db;
    db_load(&db);

    if (db.count == 0) {
        printf("%%INSTALL-I-NOIMAGES, no images installed\n");
        return 0;
    }

    printf("\nKnown Image List\n\n");
    for (uint32_t i = 0; i < db.count; i++) {
        const struct kfe_entry *e = &db.entries[i];
        char flagbuf[64];
        format_flags(e->flags, flagbuf, sizeof(flagbuf));

        if (full) {
            printf("%s\n", e->soname);
            printf("    File:       %s\n", e->path);
            printf("    Attributes: %s\n", flagbuf[0] ? flagbuf : "(none)");
            printf("    GSMATCH:    op=%u, %u.%u\n\n", e->gsmatch_op, e->major, e->minor);
        } else {
            printf("%-32s %s\n", e->soname, flagbuf);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
        "Usage:\n"
        "  INSTALL ADD    <image-filespec> [/OPEN] [/SHARED] [/HEADER_RESIDENT]\n"
        "  INSTALL LIST   [/FULL]\n"
        "  INSTALL REMOVE <image-filespec>\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *verb = argv[1];
    if (strcasecmp(verb, "ADD") == 0)
        return cmd_add(argc - 2, argv + 2);
    if (strcasecmp(verb, "LIST") == 0)
        return cmd_list(argc - 2, argv + 2);
    if (strcasecmp(verb, "REMOVE") == 0)
        return cmd_remove(argc - 2, argv + 2);

    fprintf(stderr, "%%INSTALL-E-SYNTAX, unrecognized command \"%s\"\n", verb);
    usage();
    return 1;
}
