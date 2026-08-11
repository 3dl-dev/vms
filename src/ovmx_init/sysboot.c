/*
 * sysboot.c - SYSBOOT> conversational-boot prompt (vms-b81)
 *
 * See sysboot.h for the interface contract and docs/design-boot-faithful.md
 * §2.2/§3.1/§4.2 for the design record and oracle captures this file is
 * pinned to. Operates on the SAME SYS$SYSTEM:OVMXVMSSYS.PAR file
 * tools/vms_sysgen.c and read_boot_parameters() (ovmx_init.c) already
 * read/write -- see sysboot.h's header comment for why it is handed a raw
 * Linux directory instead of going through sysgen_params.h's own
 * filespec-translating reader.
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "sysboot.h"
#include "ssdef.h"
#include "vmsfs/filespec.h"
#include "vmsfs/version.h"

/* ------------------------------------------------------------------ */
/* Boot-flag register (kernel cmdline)                                */
/* ------------------------------------------------------------------ */

int sysboot_conversational_requested(void)
{
    FILE *fp = fopen("/proc/cmdline", "r");
    if (!fp)
        return 0;

    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';

    const char *key = "ovmx.flags=";
    const char *p = strstr(buf, key);
    if (!p)
        return 0;
    p += strlen(key);

    char *end;
    (void)strtoul(p, &end, 0);   /* r5: reserved, not consulted here */
    if (*end != ',')
        return 0;
    unsigned long r6 = strtoul(end + 1, &end, 0);

    return (r6 & 1UL) != 0;
}

/* ------------------------------------------------------------------ */
/* Default parameter table                                            */
/* ------------------------------------------------------------------ */

/*
 * Same default value as tools/vms_sysgen.c's SCSNODE entry (both are
 * OVMX's own convention, not VMS-authentic -- vms-ci.8) so the two
 * utilities never visibly disagree about SCSNODE's factory default.
 */
static const struct sysgen_param sysboot_default_params[] = {
    { .name = "SCSNODE", .flags = SYSGEN_F_DYNAMIC,
      .description = "Cluster node name (SCS system name, max 6 chars)",
      .type = SYSGEN_TYPE_STRING,
      .str_current = "OVMX", .str_default = "OVMX" },
};

#define SYSBOOT_DEFAULT_PARAM_COUNT \
    ((uint32_t)(sizeof(sysboot_default_params) / sizeof(sysboot_default_params[0])))

static void sysboot_load_defaults(struct sysgen_file *ws)
{
    memset(ws, 0, sizeof(*ws));
    ws->magic   = SYSGEN_MAGIC;
    ws->version = SYSGEN_VERSION;
    ws->count   = SYSBOOT_DEFAULT_PARAM_COUNT;
    for (uint32_t i = 0; i < SYSBOOT_DEFAULT_PARAM_COUNT; i++)
        ws->params[i] = sysboot_default_params[i];
}

static int load_from_file(struct sysgen_file *ws, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return -1;

    struct sysgen_file tmp;
    int ok = (fread(&tmp, sizeof(tmp), 1, fp) == 1) &&
             tmp.magic == SYSGEN_MAGIC && tmp.version == SYSGEN_VERSION;
    fclose(fp);
    if (!ok)
        return -1;

    if (tmp.count > SYSGEN_MAX_PARAMS)
        tmp.count = SYSGEN_MAX_PARAMS;
    *ws = tmp;
    return 0;
}

/* Highest existing version of <dir>/<name>.<ext>, or -1 if none. */
static int highest_version_path(const char *dir, const char *name,
                                 const char *ext, char *path, size_t pathlen)
{
    int highest = vmsfs_get_highest_version(dir, name, ext);
    if (highest < 1)
        return -1;
    int n = snprintf(path, pathlen, "%s/%s.%s;%d", dir, name, ext, highest);
    return (n > 0 && (size_t)n < pathlen) ? highest : -1;
}

void sysboot_load_working_set(struct sysgen_file *ws, const char *dir,
                               const char *name, const char *ext)
{
    char path[VMSFS_MAX_PATH];
    if (highest_version_path(dir, name, ext, path, sizeof(path)) >= 1 &&
        load_from_file(ws, path) == 0)
        return;
    sysboot_load_defaults(ws);
}

const char *sysboot_get_string(const struct sysgen_file *ws, const char *name)
{
    static char out[SYSGEN_STRVAL_LEN + 1];

    for (uint32_t i = 0; i < ws->count; i++) {
        if (ws->params[i].type == SYSGEN_TYPE_STRING &&
            strcasecmp(ws->params[i].name, name) == 0) {
            size_t n = strnlen(ws->params[i].str_current, SYSGEN_STRVAL_LEN);
            memcpy(out, ws->params[i].str_current, n);
            out[n] = '\0';
            while (n > 0 && out[n - 1] == ' ')
                out[--n] = '\0';
            return out;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Utility                                                             */
/* ------------------------------------------------------------------ */

static char *str_trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s) {
        char *end = s + strlen(s) - 1;
        while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    }
    return s;
}

static struct sysgen_param *find_param(struct sysgen_file *ws, const char *name)
{
    for (uint32_t i = 0; i < ws->count; i++)
        if (strcasecmp(ws->params[i].name, name) == 0)
            return &ws->params[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* SHOW - table format BYTE-SHAPED to docs/design-boot-faithful.md §3.1 */
/* ------------------------------------------------------------------ */

/*
 * These two lines are pinned VERBATIM to the oracle capture (§3.1) --
 * do not "clean up" the spacing, it is measured, not chosen.
 */
static void show_header(void)
{
    printf("Parameter Name            Current    Default     Min.       Max.   Unit  Dynamic\n");
    printf("--------------            -------    -------   -------    -------  ----  -------\n");
}

/*
 * A STRING-typed row, byte-shaped to the two oracle rows in §3.1 (SCSNODE
 * with a value set, STARTUP_P1 unset). Column layout (0-indexed):
 *   Name:     cols  0-23  (width 24, left-justified)
 *   Current:  cols 24-33  (width 10, right-justified quoted field)
 *   Default:  cols 34-43  (width 10, right-justified quoted field, always
 *                          the blank "    " widget -- the oracle shows no
 *                          meaningful default/min for a string parameter)
 *   Min.:     cols 44-53  (width 10, same blank widget)
 *   Max.:     cols 54-64  (width 11, right-justified quoted field)
 *   Unit:     " Ascii"
 * Two behaviors measured directly from the two oracle rows and reproduced
 * here rather than explained (Rule 8 -- replicated for byte-fidelity, the
 * mechanism is not otherwise documented):
 *   - when the parameter has NO value set, Current renders as the same
 *     4-space blank widget as Default/Min (not padded to the full 8-byte
 *     storage width);
 *   - the Max wildcard renders "ZZZZ" (upper) when the parameter has a
 *     value set, "zzzz" (lower) when it does not.
 */
static void show_string_row(const struct sysgen_param *p)
{
    char cur_field[16];
    size_t n = strnlen(p->str_current, SYSGEN_STRVAL_LEN);
    while (n > 0 && p->str_current[n - 1] == ' ')
        n--;
    int has_value = (n > 0);

    if (has_value) {
        char cur[SYSGEN_STRVAL_LEN + 1];
        memset(cur, ' ', SYSGEN_STRVAL_LEN);
        cur[SYSGEN_STRVAL_LEN] = '\0';
        size_t full = strnlen(p->str_current, SYSGEN_STRVAL_LEN);
        memcpy(cur, p->str_current, full);
        snprintf(cur_field, sizeof(cur_field), "\"%s\"", cur);
    } else {
        snprintf(cur_field, sizeof(cur_field), "\"    \"");
    }

    printf("%-24s%10s%10s%10s%11s Ascii\n",
           p->name, cur_field, "\"    \"", "\"    \"",
           has_value ? "\"ZZZZ\"" : "\"zzzz\"");
}

/*
 * A NUMERIC-typed row. NOT oracle-verified byte-for-byte -- §3.1 captured
 * only string-typed parameters (SCSNODE, STARTUP_P1). Kept structurally
 * consistent with the string row (same column starts) rather than
 * inventing a VMS-looking format with no observation behind it.
 */
static void show_numeric_row(const struct sysgen_param *p)
{
    printf("%-24s%10u%10u%10u%10u  Dec%s\n",
           p->name, p->current, p->default_val, p->min_val, p->max_val,
           (p->flags & SYSGEN_F_DYNAMIC) ? "  D" : "");
}

static void cmd_show(struct sysgen_file *ws, const char *arg,
                      const char *startup_file)
{
    char buf[256];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *name = str_trim(buf);

    if (strlen(name) == 0) {
        fprintf(stderr, "%%SYSGEN-E-IVVERB, no parameter specified\n");
        fprintf(stderr, "-SYSGEN-I-SYNTAX, SHOW <parameter> | SHOW /STARTUP\n");
        return;
    }

    if (strcasecmp(name, "/STARTUP") == 0) {
        printf("  Startup command file = %s\n", startup_file);
        return;
    }

    struct sysgen_param *p = find_param(ws, name);
    if (!p) {
        fprintf(stderr, "%%SYSGEN-E-NOSUCHP, no such parameter \"%s\"\n", name);
        return;
    }

    show_header();
    if (p->type == SYSGEN_TYPE_STRING)
        show_string_row(p);
    else
        show_numeric_row(p);
}

/* ------------------------------------------------------------------ */
/* SET - in-memory only (VMS persistence semantics: WRITE is separate)  */
/* ------------------------------------------------------------------ */

static void cmd_set(struct sysgen_file *ws, const char *arg)
{
    char buf[256];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *s = str_trim(buf);

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

    struct sysgen_param *p = find_param(ws, param_name);
    if (!p) {
        fprintf(stderr, "%%SYSGEN-E-NOSUCHP, no such parameter \"%s\"\n",
                param_name);
        return;
    }
    if (p->flags & SYSGEN_F_INFORMATIONAL) {
        fprintf(stderr,
                "%%SYSGEN-E-RDONLY, parameter %s is informational (read-only)\n",
                p->name);
        return;
    }

    if (p->type == SYSGEN_TYPE_STRING) {
        char newval[SYSGEN_STRVAL_LEN];
        memset(newval, 0, sizeof(newval));
        strncpy(newval, value_str, sizeof(newval) - 1);
        for (char *c = newval; *c; c++) *c = (char)toupper((unsigned char)*c);

        char oldval[SYSGEN_STRVAL_LEN];
        memcpy(oldval, p->str_current, sizeof(oldval));
        memset(p->str_current, 0, sizeof(p->str_current));
        memcpy(p->str_current, newval, sizeof(p->str_current));

        printf("%%SYSGEN-I-SETPARAM, %s changed from %s to %s\n",
               p->name, oldval, p->str_current);
        return;
    }

    char *endptr;
    unsigned long val = strtoul(value_str, &endptr, 0);
    if (*endptr != '\0') {
        fprintf(stderr, "%%SYSGEN-E-IVVAL, \"%s\" is not a valid numeric value\n",
                value_str);
        return;
    }
    if (val < p->min_val) {
        fprintf(stderr, "%%SYSGEN-E-TOOSMALL, value %lu below minimum %u for %s\n",
                val, p->min_val, p->name);
        return;
    }
    if (val > p->max_val) {
        fprintf(stderr, "%%SYSGEN-E-TOOLARGE, value %lu exceeds maximum %u for %s\n",
                val, p->max_val, p->name);
        return;
    }

    uint32_t old_val = p->current;
    p->current = (uint32_t)val;
    printf("%%SYSGEN-I-SETPARAM, %s changed from %u to %u\n",
           p->name, old_val, p->current);
}

/* ------------------------------------------------------------------ */
/* USE / WRITE                                                         */
/* ------------------------------------------------------------------ */

static void cmd_use(struct sysgen_file *ws, const char *arg, const char *dir,
                     const char *name, const char *ext)
{
    char buf[256];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *target = str_trim(buf);

    if (strcasecmp(target, "CURRENT") == 0) {
        char path[VMSFS_MAX_PATH];
        if (highest_version_path(dir, name, ext, path, sizeof(path)) >= 1 &&
            load_from_file(ws, path) == 0) {
            printf("%%SYSGEN-I-LOADED, %u parameters loaded from %s\n",
                   ws->count, path);
            return;
        }
        printf("%%SYSGEN-I-NOCURRENT, no current parameter file found\n");
        printf("-SYSGEN-I-USEDEF, loading factory defaults\n");
        sysboot_load_defaults(ws);
    } else if (strcasecmp(target, "DEFAULT") == 0) {
        sysboot_load_defaults(ws);
        printf("%%SYSGEN-I-DEFLOADED, %u factory default parameters loaded\n",
               ws->count);
    } else {
        fprintf(stderr, "%%SYSGEN-E-IVVERB, invalid USE target \"%s\"\n", target);
        fprintf(stderr, "-SYSGEN-I-SYNTAX, USE CURRENT | USE DEFAULT\n");
    }
}

/*
 * WRITE - creates a REAL NEW VMSFS VERSION of SYS$SYSTEM:OVMXVMSSYS.PAR
 * (the highest existing version + 1), the same primitive tools/vms_sysgen.c's
 * WRITE CURRENT uses (vms-d34) -- so a version SYSBOOT writes is exactly as
 * durable, and exactly as visible to every other consumer, as one SYSGEN
 * writes. This is the ONLY command that touches the file; SET (above) never
 * does (VMS persistence semantics).
 */
static void cmd_write(struct sysgen_file *ws, const char *dir,
                       const char *name, const char *ext)
{
    char path[VMSFS_MAX_PATH];
    int new_version = 0;
    int status = vmsfs_create_new_version(dir, name, ext, path, sizeof(path),
                                           &new_version);
    if (!$VMS_STATUS_SUCCESS(status)) {
        fprintf(stderr,
                "%%SYSGEN-E-OPENOUT, error creating a new version of "
                "SYS$SYSTEM:%s.%s\n", name, ext);
        return;
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "%%SYSGEN-E-OPENOUT, error opening %s for output - %s\n",
                path, strerror(errno));
        return;
    }
    if (fwrite(ws, sizeof(*ws), 1, fp) != 1) {
        fclose(fp);
        fprintf(stderr, "%%SYSGEN-E-WRITEERR, error writing %s\n", path);
        return;
    }
    fclose(fp);
    /* Force the write out now -- the same reasoning as install_system()'s
     * sync() call elsewhere in this file's history: an operator who WRITEs
     * then powers off before the kernel's periodic writeback fires must not
     * lose it. */
    sync();
    printf("%%SYSGEN-I-WRITTEN, %u parameters written to SYS$SYSTEM:%s.%s;%d\n",
           ws->count, name, ext, new_version);
}

/* ------------------------------------------------------------------ */
/* The prompt loop                                                     */
/* ------------------------------------------------------------------ */

void sysboot_run_prompt(struct sysgen_file *ws, const char *dir,
                         const char *name, const char *ext,
                         const char *startup_file)
{
    char line[512];

    for (;;) {
        printf("SYSBOOT> ");
        fflush(stdout);

        if (!fgets(line, (int)sizeof(line), stdin)) {
            /* EOF on the console: nothing more can be typed. Proceed as
             * if CONTINUE had been entered rather than loop forever. */
            printf("\n");
            return;
        }

        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *cmd = str_trim(line);
        if (*cmd == '\0')
            continue;

        char verb[32];
        char *sp = cmd;
        while (*sp && !isspace((unsigned char)*sp)) sp++;
        size_t vlen = (size_t)(sp - cmd);
        if (vlen >= sizeof(verb)) vlen = sizeof(verb) - 1;
        memcpy(verb, cmd, vlen);
        verb[vlen] = '\0';
        for (char *c = verb; *c; c++) *c = (char)toupper((unsigned char)*c);

        while (*sp && isspace((unsigned char)*sp)) sp++;
        const char *arg_start = sp;

        if (strcmp(verb, "CONTINUE") == 0) {
            return;
        } else if (strcmp(verb, "SHOW") == 0) {
            cmd_show(ws, arg_start, startup_file);
        } else if (strcmp(verb, "SET") == 0) {
            cmd_set(ws, arg_start);
        } else if (strcmp(verb, "USE") == 0) {
            cmd_use(ws, arg_start, dir, name, ext);
        } else if (strcmp(verb, "WRITE") == 0) {
            cmd_write(ws, dir, name, ext);
        } else {
            /* Unknown commands error the way SYSGEN already errors --
             * SYSBOOT has no oracle-captured error vocabulary of its own
             * (see sysboot.h). */
            fprintf(stderr, "%%SYSGEN-E-IVVERB, unrecognized command \"%s\"\n",
                    verb);
            fprintf(stderr, "-SYSGEN-I-HELP, type HELP for available commands\n");
        }
    }
}
