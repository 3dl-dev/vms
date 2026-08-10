/*
 * vms_sysgen.c - SYSGEN System Generation Utility for OVMX
 *
 * Interactive utility for viewing and modifying VMS system parameters.
 * Parameters are stored in a binary database file compatible with
 * the sysgen_params.h format.
 *
 * vms-d34: USE/WRITE CURRENT resolve SYS$SYSTEM:OVMXVMSSYS.PAR (ovmx_layout.h's
 * VMS_PARAMS_PATH) through vmsfs, not a bare Linux path. WRITE CURRENT always
 * creates a NEW FILE VERSION (the highest existing version + 1 -- a real
 * vmsfs version, ";2" over ";1", matching the oracle's ALPHAVMSSYS.PAR
 * behavior, docs/design-boot-faithful.md §3.4); USE CURRENT reads the file
 * at its HIGHEST existing version. WRITE/USE <filename> (an explicit target
 * other than CURRENT) are unchanged: a literal path, no vmsfs translation,
 * no versioning -- this is what the unit tests use via OVMX_SYSGEN_PATH.
 *
 * Usage: SYSGEN
 *   Commands:
 *     USE CURRENT          - Load the highest version of SYS$SYSTEM:OVMXVMSSYS.PAR
 *     USE DEFAULT          - Load factory defaults
 *     USE <filename>       - Load from specified file (no versioning)
 *     SHOW <parameter>     - Display one parameter
 *     SHOW /ALL            - Display all parameters
 *     SET <param> <value>  - Change a parameter value
 *     WRITE CURRENT        - Write a NEW VERSION of SYS$SYSTEM:OVMXVMSSYS.PAR
 *     WRITE <filename>     - Write to specified file (no versioning)
 *     EXIT                 - Exit SYSGEN
 *     HELP                 - Show available commands
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>

#include "sysgen_params.h"
#include "ovmx_layout.h"
#include "ssdef.h"
#include "vmsfs/filespec.h"
#include "vmsfs/version.h"

/* ================================================================== */
/*                    Default Parameter Database                       */
/* ================================================================== */

static const struct sysgen_param default_params[] = {
    {"MAXPROCESSCNT",   64,      64,      4,       1024,     0,
     "Maximum number of concurrent processes", SYSGEN_TYPE_NUMERIC, "", ""},
    {"CHANNELCNT",      16,      16,      4,       256,      SYSGEN_F_DYNAMIC,
     "Number of I/O channels per process", SYSGEN_TYPE_NUMERIC, "", ""},
    {"DEFPRI",          4,       4,       0,       31,       SYSGEN_F_DYNAMIC,
     "Default process priority", SYSGEN_TYPE_NUMERIC, "", ""},
    {"MAXPRI",          31,      31,      0,       31,       0,
     "Maximum process priority", SYSGEN_TYPE_NUMERIC, "", ""},
    {"MAXBUF",          8192,    8192,    512,     65536,    SYSGEN_F_DYNAMIC,
     "Maximum buffered I/O byte count", SYSGEN_TYPE_NUMERIC, "", ""},
    {"PQL_DWSDEFAULT",  256,     256,     64,      65536,    SYSGEN_F_DYNAMIC,
     "Default working set size", SYSGEN_TYPE_NUMERIC, "", ""},
    {"PQL_DWSQUOTA",    512,     512,     64,      65536,    SYSGEN_F_DYNAMIC,
     "Working set quota", SYSGEN_TYPE_NUMERIC, "", ""},
    {"PQL_DWSEXTENT",   2048,    2048,    64,      262144,   SYSGEN_F_DYNAMIC,
     "Working set extent", SYSGEN_TYPE_NUMERIC, "", ""},
    {"PQL_DENQLM",      200,     200,     4,       32767,    SYSGEN_F_DYNAMIC,
     "Default enqueue limit", SYSGEN_TYPE_NUMERIC, "", ""},
    {"PQL_DFILLM",      100,     100,     4,       8192,     SYSGEN_F_DYNAMIC,
     "Default open file limit", SYSGEN_TYPE_NUMERIC, "", ""},
    {"PQL_DTQELM",      20,      20,      1,       1024,     SYSGEN_F_DYNAMIC,
     "Default timer queue entry limit", SYSGEN_TYPE_NUMERIC, "", ""},
    {"PQL_DBIOLM",      40,      40,      4,       4096,     SYSGEN_F_DYNAMIC,
     "Default buffered I/O limit", SYSGEN_TYPE_NUMERIC, "", ""},
    {"PQL_DDIOLM",      40,      40,      4,       4096,     SYSGEN_F_DYNAMIC,
     "Default direct I/O limit", SYSGEN_TYPE_NUMERIC, "", ""},
    {"PQL_DBYTLM",      65536,   65536,   1024,    16777216, SYSGEN_F_DYNAMIC,
     "Default buffered I/O byte limit", SYSGEN_TYPE_NUMERIC, "", ""},
    {"PQL_DPGFLQUOTA",  50000,   50000,   1024,    4194304,  SYSGEN_F_DYNAMIC,
     "Default page file quota", SYSGEN_TYPE_NUMERIC, "", ""},
    {"VIRTUALPAGECNT",  1048576, 1048576, 1024,    67108864, 0,
     "Virtual page count", SYSGEN_TYPE_NUMERIC, "", ""},
    {"GBLPAGES",        8192,    8192,    256,     4194304,  SYSGEN_F_DYNAMIC,
     "Global pages", SYSGEN_TYPE_NUMERIC, "", ""},
    {"GBLSECTIONS",     256,     256,     16,      4096,     SYSGEN_F_DYNAMIC,
     "Global sections", SYSGEN_TYPE_NUMERIC, "", ""},
    {"LNMPHASHTBL",     128,     128,     16,      8192,     SYSGEN_F_DYNAMIC,
     "Logical name hash table size", SYSGEN_TYPE_NUMERIC, "", ""},
    {"ACP_MAPCACHE",    32,      32,      4,       256,      SYSGEN_F_DYNAMIC,
     "ACP map cache size", SYSGEN_TYPE_NUMERIC, "", ""},
    {"BALSETCNT",       16,      16,      4,       256,      0,
     "Maximum number of processes in balance set", SYSGEN_TYPE_NUMERIC, "", ""},
    {"IRPCOUNT",        256,     256,     32,      4096,     0,
     "Number of I/O request packets", SYSGEN_TYPE_NUMERIC, "", ""},
    {"SRPCOUNT",        256,     256,     32,      4096,     0,
     "Number of small request packets", SYSGEN_TYPE_NUMERIC, "", ""},
    {"LRPCOUNT",        32,      32,      4,       512,      0,
     "Number of large request packets", SYSGEN_TYPE_NUMERIC, "", ""},

    /* --- vms-ci.8: cluster node-identity parameters ---
     * OVMX-defined defaults (NOT VMS-authentic values) — see item vms-ci.8.
     * SCSSYSTEMID/ALLOCLASS/VOTES/EXPECTED_VOTES/VAXCLUSTER mirror the real
     * VMS SYSGEN parameter names; SCSNODE is the only string-typed param. */
    { .name = "SCSSYSTEMID", .current = 0, .default_val = 0,
      .min_val = 0, .max_val = 65535, .flags = SYSGEN_F_DYNAMIC,
      .description = "Cluster system ID (OVMX default 0)",
      .type = SYSGEN_TYPE_NUMERIC },
    { .name = "ALLOCLASS", .current = 0, .default_val = 0,
      .min_val = 0, .max_val = 255, .flags = SYSGEN_F_DYNAMIC,
      .description = "Allocation class for shared cluster devices",
      .type = SYSGEN_TYPE_NUMERIC },
    { .name = "VOTES", .current = 1, .default_val = 1,
      .min_val = 0, .max_val = 32767, .flags = SYSGEN_F_DYNAMIC,
      .description = "Cluster quorum votes contributed by this node",
      .type = SYSGEN_TYPE_NUMERIC },
    { .name = "EXPECTED_VOTES", .current = 1, .default_val = 1,
      .min_val = 1, .max_val = 32767, .flags = SYSGEN_F_DYNAMIC,
      .description = "Expected total cluster quorum votes",
      .type = SYSGEN_TYPE_NUMERIC },
    { .name = "VAXCLUSTER", .current = 0, .default_val = 0,
      .min_val = 0, .max_val = 2, .flags = SYSGEN_F_DYNAMIC,
      .description = "Cluster participation (0=disabled,1=enabled,2=auto)",
      .type = SYSGEN_TYPE_NUMERIC },
    { .name = "SCSNODE", .flags = SYSGEN_F_DYNAMIC,
      .description = "Cluster node name (SCS system name, max 6 chars)",
      .type = SYSGEN_TYPE_STRING,
      .str_current = "OVMX", .str_default = "OVMX" },
};

#define DEFAULT_PARAM_COUNT \
    ((uint32_t)(sizeof(default_params) / sizeof(default_params[0])))

/* ================================================================== */
/*                        Working Set State                           */
/* ================================================================== */

static struct sysgen_file working_set;
static int working_set_loaded;
static int working_set_modified;

/* ================================================================== */
/*                       Utility Functions                            */
/* ================================================================== */

static void str_upper(char *s)
{
    for (; *s; s++)
        *s = (char)toupper((unsigned char)*s);
}

static char *str_trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s) {
        char *end = s + strlen(s) - 1;
        while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    }
    return s;
}

/* ================================================================== */
/*                    Database I/O Functions                           */
/* ================================================================== */

/*
 * resolve_params_location - Translate VMS_PARAMS_PATH (SYS$SYSTEM:
 * OVMXVMSSYS.PAR) through vmsfs and split the result into the directory
 * plus name/ext vmsfs_get_highest_version()/vmsfs_create_new_version()
 * want. Mirrors the split already done at the RMS/DCL layer (rms_core.c's
 * rms_next_version, dcl_filespec.c's resolve_version_suffix).
 *
 * Returns 0 on success, -1 if the filespec cannot be translated.
 */
static int resolve_params_location(char *dir, size_t dirlen, char *name,
                                    size_t namelen, char *ext, size_t extlen)
{
    char linux_path[VMSFS_MAX_PATH];
    if (!$VMS_STATUS_SUCCESS(vmsfs_to_linux_path(VMS_PARAMS_PATH, linux_path,
                                                  sizeof(linux_path)))) {
        return -1;
    }

    const char *slash = strrchr(linux_path, '/');
    const char *base = slash ? slash + 1 : linux_path;

    if (slash) {
        size_t dlen = (size_t)(slash - linux_path);
        if (dlen >= dirlen) dlen = dirlen - 1;
        memcpy(dir, linux_path, dlen);
        dir[dlen] = '\0';
    } else {
        dir[0] = '\0';
    }

    char basebuf[VMSFS_MAX_NAME + VMSFS_MAX_TYPE + 2];
    strncpy(basebuf, base, sizeof(basebuf) - 1);
    basebuf[sizeof(basebuf) - 1] = '\0';

    char *dot = strrchr(basebuf, '.');
    if (dot) {
        *dot = '\0';
        strncpy(ext, dot + 1, extlen - 1);
        ext[extlen - 1] = '\0';
    } else {
        ext[0] = '\0';
    }
    strncpy(name, basebuf, namelen - 1);
    name[namelen - 1] = '\0';
    return 0;
}

static void load_defaults(void)
{
    memset(&working_set, 0, sizeof(working_set));
    working_set.magic   = SYSGEN_MAGIC;
    working_set.version = SYSGEN_VERSION;
    working_set.count   = DEFAULT_PARAM_COUNT;

    for (uint32_t i = 0; i < DEFAULT_PARAM_COUNT && i < SYSGEN_MAX_PARAMS; i++) {
        working_set.params[i] = default_params[i];
    }

    working_set_loaded   = 1;
    working_set_modified = 0;
}

static int load_from_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "%%SYSGEN-E-OPENIN, error opening %s as input\n", path);
        fprintf(stderr, "-RMS-E-FNF, file not found\n");
        return -1;
    }

    struct sysgen_file tmp;
    if (fread(&tmp, sizeof(tmp), 1, fp) != 1) {
        fclose(fp);
        fprintf(stderr, "%%SYSGEN-E-READERR, error reading %s\n", path);
        return -1;
    }
    fclose(fp);

    if (tmp.magic != SYSGEN_MAGIC) {
        fprintf(stderr, "%%SYSGEN-E-BADFILE, %s is not a valid SYSGEN parameter file\n",
                path);
        return -1;
    }
    if (tmp.version != SYSGEN_VERSION) {
        fprintf(stderr, "%%SYSGEN-E-BADVER, unsupported parameter file version %u\n",
                tmp.version);
        return -1;
    }
    if (tmp.count > SYSGEN_MAX_PARAMS) {
        fprintf(stderr,
                "%%SYSGEN-W-TRUNC, parameter count %u exceeds maximum, truncating\n",
                tmp.count);
        tmp.count = SYSGEN_MAX_PARAMS;
    }

    working_set = tmp;
    working_set_loaded   = 1;
    working_set_modified = 0;
    return 0;
}

static int write_to_file(const char *path)
{
    if (!working_set_loaded) {
        fprintf(stderr, "%%SYSGEN-E-NODATA, no parameter set loaded\n");
        fprintf(stderr, "-SYSGEN-I-USECMD, use USE CURRENT or USE DEFAULT first\n");
        return -1;
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "%%SYSGEN-E-OPENOUT, error opening %s for output - %s\n",
                path, strerror(errno));
        return -1;
    }

    if (fwrite(&working_set, sizeof(working_set), 1, fp) != 1) {
        fclose(fp);
        fprintf(stderr, "%%SYSGEN-E-WRITEERR, error writing %s\n", path);
        return -1;
    }
    fclose(fp);

    working_set_modified = 0;
    printf("%%SYSGEN-I-WRITTEN, %u parameters written to %s\n",
           working_set.count, path);
    return 0;
}

/*
 * write_new_version - WRITE CURRENT's real body: create a NEW FILE VERSION
 * of SYS$SYSTEM:OVMXVMSSYS.PAR (the highest existing version + 1, via
 * vmsfs_create_new_version -- a real vmsfs version, not an in-memory
 * counter, matching the oracle's ALPHAVMSSYS.PAR ";2" over ";1" behavior,
 * docs/design-boot-faithful.md §3.4) and write the working set into it.
 *
 * Returns 0 on success, -1 on error (messages already printed).
 */
static int write_new_version(void)
{
    if (!working_set_loaded) {
        fprintf(stderr, "%%SYSGEN-E-NODATA, no parameter set loaded\n");
        fprintf(stderr, "-SYSGEN-I-USECMD, use USE CURRENT or USE DEFAULT first\n");
        return -1;
    }

    char dir[VMSFS_MAX_PATH], name[VMSFS_MAX_NAME + 1], ext[VMSFS_MAX_TYPE + 1];
    if (resolve_params_location(dir, sizeof(dir), name, sizeof(name),
                                 ext, sizeof(ext)) != 0) {
        fprintf(stderr,
                "%%SYSGEN-E-OPENOUT, error resolving " "SYS$SYSTEM:OVMXVMSSYS.PAR\n");
        return -1;
    }
    /* vmsfs_create_new_version() needs a real directory to scan/create in;
     * an empty dir (relative filespec with no device) means "here". */
    if (!dir[0]) strncpy(dir, ".", sizeof(dir) - 1);

    char path[VMSFS_MAX_PATH];
    int new_version = 0;
    int status = vmsfs_create_new_version(dir, name, ext, path, sizeof(path),
                                           &new_version);
    if (!$VMS_STATUS_SUCCESS(status)) {
        fprintf(stderr,
                "%%SYSGEN-E-OPENOUT, error creating a new version of "
                "SYS$SYSTEM:OVMXVMSSYS.PAR\n");
        return -1;
    }

    /* vmsfs_create_new_version() already created the file (O_CREAT|O_EXCL,
     * empty) to claim the version number race-free; reopen it to write the
     * actual bytes. */
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "%%SYSGEN-E-OPENOUT, error opening %s for output - %s\n",
                path, strerror(errno));
        return -1;
    }
    if (fwrite(&working_set, sizeof(working_set), 1, fp) != 1) {
        fclose(fp);
        fprintf(stderr, "%%SYSGEN-E-WRITEERR, error writing %s\n", path);
        return -1;
    }
    fclose(fp);

    working_set_modified = 0;
    printf("%%SYSGEN-I-WRITTEN, %u parameters written to "
           "SYS$SYSTEM:OVMXVMSSYS.PAR;%d\n", working_set.count, new_version);
    return 0;
}

/* ================================================================== */
/*                      Command Handlers                              */
/* ================================================================== */

static void cmd_use(const char *arg)
{
    char buf[256];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *target = str_trim(buf);

    if (strlen(target) == 0) {
        fprintf(stderr, "%%SYSGEN-E-IVVERB, no source specified\n");
        fprintf(stderr,
                "-SYSGEN-I-SYNTAX, USE CURRENT | USE DEFAULT | USE <filename>\n");
        return;
    }

    if (strcasecmp(target, "CURRENT") == 0) {
        char dir[VMSFS_MAX_PATH], name[VMSFS_MAX_NAME + 1], ext[VMSFS_MAX_TYPE + 1];
        char path[VMSFS_MAX_PATH];
        int loaded = 0;

        if (resolve_params_location(dir, sizeof(dir), name, sizeof(name),
                                     ext, sizeof(ext)) == 0) {
            int highest = vmsfs_get_highest_version(dir[0] ? dir : ".", name, ext);
            if (highest >= 1) {
                int n = snprintf(path, sizeof(path), "%s/%s.%s;%d",
                                  dir, name, ext, highest);
                if (n > 0 && (size_t)n < sizeof(path) &&
                    load_from_file(path) == 0) {
                    printf("%%SYSGEN-I-LOADED, %u parameters loaded from %s\n",
                           working_set.count, path);
                    loaded = 1;
                }
            }
        }

        if (!loaded) {
            printf("%%SYSGEN-I-NOCURRENT, no current parameter file found\n");
            printf("-SYSGEN-I-USEDEF, loading factory defaults\n");
            load_defaults();
        }
    } else if (strcasecmp(target, "DEFAULT") == 0) {
        load_defaults();
        printf("%%SYSGEN-I-DEFLOADED, %u factory default parameters loaded\n",
               working_set.count);
    } else {
        /* Treat as filename */
        if (load_from_file(target) == 0) {
            printf("%%SYSGEN-I-LOADED, %u parameters loaded from %s\n",
                   working_set.count, target);
        }
    }
}

static void show_header(void)
{
    printf("\n");
    printf("  %-32s %10s %10s %10s %10s\n",
           "Parameter Name", "Current", "Default", "Minimum", "Maximum");
    printf("  %-32s %10s %10s %10s %10s\n",
           "--------------", "-------", "-------", "-------", "-------");
}

/*
 * format_sysgen_string_field - Render a string-typed parameter value the
 * way SHOW displays it: double-quoted and space-padded to the field's full
 * stored width (SYSGEN_STRVAL_LEN-1 content bytes), e.g. "OVMX   " for a
 * 4-char value. docs/design-boot-faithful.md §3.1 (the Alpha oracle's
 * SYSBOOT> SHOW SCSNODE) shows the same quoted-and-padded shape for a
 * string parameter; this SHOW is a different program (SYSGEN.EXE, not
 * SYSBOOT>) so the exact field width is an OVMX display choice, not a
 * byte-for-byte match -- matching storage width is what's observed here.
 */
static void format_sysgen_string_field(const char *val, char *out, size_t outlen)
{
    char padded[SYSGEN_STRVAL_LEN];
    size_t i = 0;
    for (; i < sizeof(padded) - 1 && val[i]; i++) padded[i] = val[i];
    for (; i < sizeof(padded) - 1; i++) padded[i] = ' ';
    padded[sizeof(padded) - 1] = '\0';
    snprintf(out, outlen, "\"%s\"", padded);
}

static void show_param(const struct sysgen_param *p)
{
    if (p->type == SYSGEN_TYPE_STRING) {
        char cur[SYSGEN_STRVAL_LEN + 2], def[SYSGEN_STRVAL_LEN + 2];
        format_sysgen_string_field(p->str_current, cur, sizeof(cur));
        format_sysgen_string_field(p->str_default, def, sizeof(def));
        printf("  %-32s %10s %10s %10s %10s",
               p->name, cur, def, "-", "-");
    } else {
        printf("  %-32s %10u %10u %10u %10u",
               p->name, p->current, p->default_val, p->min_val, p->max_val);
    }
    if (p->flags & SYSGEN_F_DYNAMIC)
        printf("  D");
    printf("\n");
}

static void cmd_show(const char *arg)
{
    if (!working_set_loaded) {
        fprintf(stderr, "%%SYSGEN-E-NODATA, no parameter set loaded\n");
        fprintf(stderr, "-SYSGEN-I-USECMD, use USE CURRENT or USE DEFAULT first\n");
        return;
    }

    char buf[256];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *name = str_trim(buf);

    if (strlen(name) == 0) {
        fprintf(stderr, "%%SYSGEN-E-IVVERB, no parameter specified\n");
        fprintf(stderr, "-SYSGEN-I-SYNTAX, SHOW <parameter> | SHOW /ALL\n");
        return;
    }

    /* SHOW /ALL */
    if (strcasecmp(name, "/ALL") == 0) {
        show_header();
        for (uint32_t i = 0; i < working_set.count; i++) {
            show_param(&working_set.params[i]);
        }
        printf("\n  %u parameters displayed. (D = Dynamic)\n\n",
               working_set.count);
        return;
    }

    /* Show single parameter — exact match first */
    int found = 0;
    for (uint32_t i = 0; i < working_set.count; i++) {
        if (strcasecmp(working_set.params[i].name, name) == 0) {
            show_header();
            show_param(&working_set.params[i]);
            printf("  %s\n\n", working_set.params[i].description);
            found = 1;
            break;
        }
    }

    if (found) return;

    /* Try wildcard match (trailing *) */
    size_t len = strlen(name);
    int wildcard = (len > 1 && name[len - 1] == '*');
    if (wildcard) {
        name[len - 1] = '\0';
        len--;
        show_header();
        for (uint32_t i = 0; i < working_set.count; i++) {
            if (strncasecmp(working_set.params[i].name, name, len) == 0) {
                show_param(&working_set.params[i]);
                found = 1;
            }
        }
        if (found) {
            printf("\n");
            return;
        }
    }

    fprintf(stderr, "%%SYSGEN-E-NOSUCHP, no such parameter \"%s\"\n", name);
}

static void cmd_set(const char *arg)
{
    if (!working_set_loaded) {
        fprintf(stderr, "%%SYSGEN-E-NODATA, no parameter set loaded\n");
        fprintf(stderr, "-SYSGEN-I-USECMD, use USE CURRENT or USE DEFAULT first\n");
        return;
    }

    char buf[256];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *s = str_trim(buf);

    /* Parse: <parameter> <value> */
    char *space = s;
    while (*space && !isspace((unsigned char)*space)) space++;
    if (!*space) {
        fprintf(stderr, "%%SYSGEN-E-IVVERB, missing value\n");
        fprintf(stderr, "-SYSGEN-I-SYNTAX, SET <parameter> <value>\n");
        return;
    }

    *space = '\0';
    char *param_name = s;
    char *value_str  = str_trim(space + 1);

    if (strlen(value_str) == 0) {
        fprintf(stderr, "%%SYSGEN-E-IVVERB, missing value\n");
        fprintf(stderr, "-SYSGEN-I-SYNTAX, SET %s <value>\n", param_name);
        return;
    }

    /* Find parameter (case-insensitive) */
    struct sysgen_param *found = NULL;
    for (uint32_t i = 0; i < working_set.count; i++) {
        if (strcasecmp(working_set.params[i].name, param_name) == 0) {
            found = &working_set.params[i];
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "%%SYSGEN-E-NOSUCHP, no such parameter \"%s\"\n",
                param_name);
        return;
    }

    if (found->flags & SYSGEN_F_INFORMATIONAL) {
        fprintf(stderr,
                "%%SYSGEN-E-RDONLY, parameter %s is informational (read-only)\n",
                found->name);
        return;
    }

    if (found->type == SYSGEN_TYPE_STRING) {
        char newval[SYSGEN_STRVAL_LEN];
        strncpy(newval, value_str, sizeof(newval) - 1);
        newval[sizeof(newval) - 1] = '\0';
        str_upper(newval);

        char oldval[SYSGEN_STRVAL_LEN];
        memcpy(oldval, found->str_current, sizeof(oldval));
        memcpy(found->str_current, newval, sizeof(found->str_current));
        working_set_modified = 1;

        printf("%%SYSGEN-I-SETPARAM, %s changed from %s to %s\n",
               found->name, oldval, found->str_current);
        return;
    }

    /* Parse numeric value */
    char *endptr;
    unsigned long val = strtoul(value_str, &endptr, 0);
    if (*endptr != '\0') {
        fprintf(stderr,
                "%%SYSGEN-E-IVVAL, \"%s\" is not a valid numeric value\n",
                value_str);
        return;
    }

    /* Validate range */
    if (val < found->min_val) {
        fprintf(stderr,
                "%%SYSGEN-E-TOOSMALL, value %lu below minimum %u for %s\n",
                val, found->min_val, found->name);
        return;
    }
    if (val > found->max_val) {
        fprintf(stderr,
                "%%SYSGEN-E-TOOLARGE, value %lu exceeds maximum %u for %s\n",
                val, found->max_val, found->name);
        return;
    }

    uint32_t old_val = found->current;
    found->current = (uint32_t)val;
    working_set_modified = 1;

    printf("%%SYSGEN-I-SETPARAM, %s changed from %u to %u\n",
           found->name, old_val, (uint32_t)val);
}

static void cmd_write(const char *arg)
{
    char buf[256];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *target = str_trim(buf);

    if (strlen(target) == 0 || strcasecmp(target, "CURRENT") == 0) {
        write_new_version();
    } else {
        write_to_file(target);
    }
}

static void cmd_help(void)
{
    printf("\n");
    printf("  SYSGEN Commands:\n");
    printf("  ================\n");
    printf("\n");
    printf("  USE CURRENT          Load the highest version of SYS$SYSTEM:OVMXVMSSYS.PAR\n");
    printf("  USE DEFAULT          Load factory default parameters\n");
    printf("  USE <filename>       Load parameters from specified file\n");
    printf("\n");
    printf("  SHOW <parameter>     Display a single parameter\n");
    printf("  SHOW <prefix>*       Display parameters matching prefix\n");
    printf("  SHOW /ALL            Display all parameters\n");
    printf("\n");
    printf("  SET <param> <value>  Change a parameter value\n");
    printf("\n");
    printf("  WRITE CURRENT        Write a NEW VERSION of SYS$SYSTEM:OVMXVMSSYS.PAR\n");
    printf("  WRITE <filename>     Write parameters to specified file\n");
    printf("\n");
    printf("  HELP                 Display this help\n");
    printf("  EXIT                 Exit SYSGEN\n");
    printf("\n");
    printf("  Parameter Flags:  D = Dynamic (can be changed without reboot)\n");
    printf("\n");
}

/* ================================================================== */
/*                          Main Loop                                 */
/* ================================================================== */

static int process_command(const char *line)
{
    char buf[512];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *cmd = str_trim(buf);

    if (strlen(cmd) == 0) return 0;

    /* Extract verb (first word) and argument (rest of line) */
    char verb[32];
    const char *arg_start;
    char *sp = cmd;
    while (*sp && !isspace((unsigned char)*sp)) sp++;

    size_t vlen = (size_t)(sp - cmd);
    if (vlen >= sizeof(verb)) vlen = sizeof(verb) - 1;
    memcpy(verb, cmd, vlen);
    verb[vlen] = '\0';
    str_upper(verb);

    /* Skip whitespace to find argument */
    while (*sp && isspace((unsigned char)*sp)) sp++;
    arg_start = sp;

    if (strcmp(verb, "EXIT") == 0 || strcmp(verb, "QUIT") == 0) {
        if (working_set_modified) {
            printf("%%SYSGEN-I-MODIFIED, "
                   "parameter set has been modified but not written\n");
        }
        return 1;
    }

    if (strcmp(verb, "USE") == 0) {
        cmd_use(arg_start);
    } else if (strcmp(verb, "SHOW") == 0) {
        cmd_show(arg_start);
    } else if (strcmp(verb, "SET") == 0) {
        cmd_set(arg_start);
    } else if (strcmp(verb, "WRITE") == 0) {
        cmd_write(arg_start);
    } else if (strcmp(verb, "HELP") == 0 || strcmp(verb, "?") == 0) {
        cmd_help();
    } else {
        fprintf(stderr, "%%SYSGEN-E-IVVERB, unrecognized command \"%s\"\n",
                verb);
        fprintf(stderr, "-SYSGEN-I-HELP, type HELP for available commands\n");
    }

    return 0;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    int interactive = isatty(fileno(stdin));

    if (interactive) {
        printf("\n");
        printf("      OpenVMS System Generation Utility\n");
        printf("\n");
    }

    char line[512];

    for (;;) {
        if (interactive)
            printf("SYSGEN> ");

        if (!fgets(line, (int)sizeof(line), stdin))
            break;

        /* Strip newline */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        if (process_command(line))
            break;
    }

    if (interactive)
        printf("\n");

    return 0;
}
