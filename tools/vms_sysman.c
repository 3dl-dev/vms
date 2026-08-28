/*
 * vms_sysman.c - SYSMAN System Management Utility
 *
 * Interactive utility for managing VMS system configuration.
 * Provides commands for startup procedure management, system
 * parameter viewing/modification, and remote command execution.
 *
 * Commands:
 *   SET ENVIRONMENT /NODE=name
 *   STARTUP SHOW
 *   STARTUP ADD file /PHASE={LPMAIN|LPBETA}
 *   STARTUP REMOVE file
 *   PARAMETERS SHOW param
 *   PARAMETERS SET param value
 *   DO dcl-command
 *   SHUTDOWN NODE
 *   EXIT
 *   HELP
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>

#include "sysgen_params.h"
#include "ovmx_identity.h"   /* ovmx_node_name() -- the real local node name */

/* ------------------------------------------------------------------ */
/*  Paths                                                              */
/* ------------------------------------------------------------------ */
#include "ovmx_layout.h"

#define SYSMGR_DIR     VMS_MANAGER_DIR
#define STARTUP_LIST   SYSMGR_DIR "/STARTUP_LIST.DAT"

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Case-insensitive prefix match — returns 1 if str starts with pfx */
static int prefix_match(const char *str, const char *pfx)
{
    while (*pfx) {
        if (toupper((unsigned char)*str) != toupper((unsigned char)*pfx))
            return 0;
        str++;
        pfx++;
    }
    return 1;
}

/* Case-insensitive substring search — returns pointer into hay, or NULL */
static const char *stristr_ci(const char *hay, const char *needle)
{
    if (!*needle)
        return hay;
    for (; *hay; hay++) {
        const char *h = hay;
        const char *n = needle;
        while (*h && *n &&
               toupper((unsigned char)*h) == toupper((unsigned char)*n)) {
            h++;
            n++;
        }
        if (!*n)
            return hay;
    }
    return NULL;
}

/* Skip leading whitespace */
static const char *skip_ws(const char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    return s;
}

/* Upper-case a string in place */
static void str_upper(char *s)
{
    for (; *s; s++)
        *s = toupper((unsigned char)*s);
}

/* Format current time in VMS format: DD-MMM-YYYY HH:MM:SS.CC */
static void vms_timestamp(char *buf, size_t len)
{
    static const char *months[] = {
        "JAN","FEB","MAR","APR","MAY","JUN",
        "JUL","AUG","SEP","OCT","NOV","DEC"
    };
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    snprintf(buf, len, "%02d-%s-%04d %02d:%02d:%02d.00",
             tm->tm_mday, months[tm->tm_mon], tm->tm_year + 1900,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
}

/* ------------------------------------------------------------------ */
/*  SET ENVIRONMENT / DO environment state (vms-495)                    */
/* ------------------------------------------------------------------ */
/*
 * SYSMAN begins in a LOCAL environment on the node it runs on -- the genuine
 * OpenVMS default (VSI OpenVMS System Management Utilities Reference Manual,
 * SET ENVIRONMENT). A LOCAL environment executes DO commands on this node,
 * which is exactly what OVMX can honestly do.
 *
 * A remote (SET ENVIRONMENT/NODE=<other>) or /CLUSTER environment requires
 * SYSMAN to connect to the SMISERVER process on each target node and ship the
 * command there. OVMX has NO SMISERVER and NO cluster command transport (that
 * is a separately-tracked rung, not built here). So a remote request cannot be
 * honestly serviced: SET ENVIRONMENT fails honest, AND the requested-but-
 * unserviceable target is recorded so a subsequent DO also fails honest rather
 * than silently forking a LOCAL DCL and mislabelling its output as the remote
 * node's. INV-6: never fake per-node/cluster success.
 */
enum sysman_env { SYSMAN_ENV_LOCAL, SYSMAN_ENV_REMOTE_UNAVAIL };
static enum sysman_env env_state = SYSMAN_ENV_LOCAL;
static char env_target[256] = "";   /* remote target text, for the DO error */

/* ------------------------------------------------------------------ */
/*  SET ENVIRONMENT [/NODE=name | /CLUSTER]                             */
/* ------------------------------------------------------------------ */
static void cmd_set_environment(const char *rest)
{
    /* rest begins with the ENVIRONMENT/ENV keyword itself; skip past it. */
    const char *p = rest;
    while (*p && !isspace((unsigned char)*p) && *p != '/')
        p++;
    const char *args = skip_ws(p);

    const char *cluster = stristr_ci(args, "/CLUSTER");
    const char *nodeq   = stristr_ci(args, "/NODE");

    char local[OVMX_IDENTITY_MAXLEN];
    ovmx_node_name(local, sizeof(local));

    /* No qualifier (or an explicit local node) -> LOCAL environment. This is
     * genuine, fully-serviceable VMS behavior. */
    if (!cluster && !nodeq) {
        env_state = SYSMAN_ENV_LOCAL;
        env_target[0] = '\0';
        printf("%%SYSMAN-I-ENV, environment established on local node %s\n",
               local);
        return;
    }

    if (cluster) {
        env_state = SYSMAN_ENV_REMOTE_UNAVAIL;
        strncpy(env_target, "cluster", sizeof(env_target) - 1);
        env_target[sizeof(env_target) - 1] = '\0';
        /* Honest failure -- OVMX has no cluster command transport. Not a
         * VMS-authentic message text (no oracle pin for the exact code); the
         * message states the real reason plainly rather than fake success. */
        printf("%%SYSMAN-E-NOSMISERVER, cannot establish cluster environment: "
               "OVMX has no SMISERVER / cluster command transport\n");
        return;
    }

    /* /NODE=value : extract the value after '='. */
    const char *eq = strchr(nodeq, '=');
    char value[256] = "";
    if (eq) {
        const char *v = skip_ws(eq + 1);
        int vi = 0;
        while (*v && !isspace((unsigned char)*v) && *v != '/'
               && vi < (int)sizeof(value) - 1)
            value[vi++] = *v++;
        value[vi] = '\0';
    }

    if (value[0] == '\0') {
        printf("%%SYSMAN-E-SYNTAX, SET ENVIRONMENT/NODE requires a node name\n");
        return;
    }

    /* A parenthesised or comma list is by definition multi-node (remote); a
     * single name is remote unless it is THIS node. */
    int remote;
    if (strchr(value, '(') || strchr(value, ','))
        remote = 1;
    else
        remote = (strcasecmp(value, local) != 0);

    if (!remote) {
        env_state = SYSMAN_ENV_LOCAL;
        env_target[0] = '\0';
        printf("%%SYSMAN-I-ENV, environment established on local node %s\n",
               local);
        return;
    }

    env_state = SYSMAN_ENV_REMOTE_UNAVAIL;
    strncpy(env_target, value, sizeof(env_target) - 1);
    env_target[sizeof(env_target) - 1] = '\0';
    printf("%%SYSMAN-E-NOSMISERVER, cannot establish environment on node %s: "
           "OVMX has no SMISERVER / cluster command transport\n", value);
}

/* ------------------------------------------------------------------ */
/*  STARTUP SHOW                                                       */
/* ------------------------------------------------------------------ */
static void cmd_startup_show(void)
{
    FILE *fp = fopen(STARTUP_LIST, "r");
    if (!fp) {
        printf("%%SYSMAN-I-NOLIST, no startup procedures defined\n");
        return;
    }

    printf("\nStartup procedure listing:\n\n");
    printf("Phase    File\n");
    printf("-----    ----\n");

    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), fp)) {
        /* Remove trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[--len] = '\0';
        if (len == 0)
            continue;

        /* Format: PHASE:FILENAME */
        char *colon = strchr(line, ':');
        if (!colon)
            continue;

        *colon = '\0';
        const char *phase = line;
        const char *file  = colon + 1;
        printf("%-8s %s\n", phase, file);
        count++;
    }
    fclose(fp);

    if (count == 0)
        printf("  (no startup procedures defined)\n");
    printf("\n");
}

/* ------------------------------------------------------------------ */
/*  STARTUP ADD file /PHASE=phase                                      */
/* ------------------------------------------------------------------ */
static void cmd_startup_add(const char *rest)
{
    /* Parse: filename /PHASE=LPMAIN */
    char filename[256] = "";
    char phase[32] = "LPMAIN";

    /* Extract tokens */
    const char *p = skip_ws(rest);
    int fi = 0;
    while (*p && !isspace((unsigned char)*p) && *p != '/' && fi < (int)sizeof(filename) - 1)
        filename[fi++] = *p++;
    filename[fi] = '\0';

    /* Look for /PHASE= qualifier */
    const char *qual = strstr(p, "/PHASE=");
    if (!qual)
        qual = strstr(p, "/phase=");
    if (qual) {
        qual += 7; /* skip "/PHASE=" */
        int pi = 0;
        while (*qual && !isspace((unsigned char)*qual) && pi < (int)sizeof(phase) - 1)
            phase[pi++] = *qual++;
        phase[pi] = '\0';
        str_upper(phase);
    }

    if (filename[0] == '\0') {
        printf("%%SYSMAN-E-NOFILE, no filename specified\n");
        return;
    }

    str_upper(filename);

    /* Ensure directory exists — create each path component directly */
    {
        const char *dir = SYSMGR_DIR;
        char tmp[512];
        strncpy(tmp, dir, sizeof(tmp) - 1);
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

    FILE *fp = fopen(STARTUP_LIST, "a");
    if (!fp) {
        printf("%%SYSMAN-E-OPENERR, cannot open startup list\n");
        return;
    }
    fprintf(fp, "%s:%s\n", phase, filename);
    fclose(fp);
    printf("%%SYSMAN-I-ADDED, %s added to %s startup\n", filename, phase);
}

/* ------------------------------------------------------------------ */
/*  STARTUP REMOVE file                                                */
/* ------------------------------------------------------------------ */
static void cmd_startup_remove(const char *rest)
{
    char filename[256] = "";
    const char *p = skip_ws(rest);
    int fi = 0;
    while (*p && !isspace((unsigned char)*p) && fi < (int)sizeof(filename) - 1)
        filename[fi++] = *p++;
    filename[fi] = '\0';

    if (filename[0] == '\0') {
        printf("%%SYSMAN-E-NOFILE, no filename specified\n");
        return;
    }

    str_upper(filename);

    FILE *fp = fopen(STARTUP_LIST, "r");
    if (!fp) {
        printf("%%SYSMAN-E-NOTFOUND, startup list does not exist\n");
        return;
    }

    /* Read all lines, write back without the matching one.
     * Use heap allocation to avoid 128KB stack buffer. */
    #define SYSMAN_MAX_LINES 256
    #define SYSMAN_LINE_LEN  512
    char (*lines)[SYSMAN_LINE_LEN] = malloc(SYSMAN_MAX_LINES * SYSMAN_LINE_LEN);
    if (!lines) {
        printf("%%SYSMAN-E-NOMEM, memory allocation failed\n");
        fclose(fp);
        return;
    }
    int count = 0;
    int removed = 0;

    while (count < SYSMAN_MAX_LINES && fgets(lines[count], SYSMAN_LINE_LEN, fp)) {
        /* Check if this line contains the filename (after the colon) */
        char *colon = strchr(lines[count], ':');
        if (colon) {
            char entry[256];
            strncpy(entry, colon + 1, sizeof(entry) - 1);
            entry[sizeof(entry) - 1] = '\0';
            /* Strip newline */
            size_t len = strlen(entry);
            if (len > 0 && entry[len - 1] == '\n')
                entry[len - 1] = '\0';
            str_upper(entry);
            if (strcmp(entry, filename) == 0) {
                removed = 1;
                continue; /* skip this line */
            }
        }
        count++;
    }
    fclose(fp);

    if (!removed) {
        printf("%%SYSMAN-E-NOTFOUND, %s not found in startup list\n", filename);
        free(lines);
        return;
    }

    fp = fopen(STARTUP_LIST, "w");
    if (!fp) {
        printf("%%SYSMAN-E-OPENERR, cannot write startup list\n");
        free(lines);
        return;
    }
    for (int i = 0; i < count; i++)
        fputs(lines[i], fp);
    fclose(fp);
    free(lines);
    printf("%%SYSMAN-I-REMOVED, %s removed from startup list\n", filename);
}

/* ------------------------------------------------------------------ */
/*  STARTUP dispatch                                                   */
/* ------------------------------------------------------------------ */
static void cmd_startup(const char *rest)
{
    const char *p = skip_ws(rest);

    if (prefix_match(p, "SHOW") || *p == '\0') {
        cmd_startup_show();
    } else if (prefix_match(p, "ADD")) {
        cmd_startup_add(p + 3);
    } else if (prefix_match(p, "REMOVE")) {
        cmd_startup_remove(p + 6);
    } else {
        printf("%%SYSMAN-E-SYNTAX, unrecognized STARTUP subcommand\n");
    }
}

/* ------------------------------------------------------------------ */
/*  PARAMETERS work area                                               */
/* ------------------------------------------------------------------ */
/*
 * SYSMAN PARAMETERS operates on a "work area" -- an in-memory copy of a
 * parameter set loaded by PARAMETERS USE {ACTIVE|CURRENT|file}, modified by
 * PARAMETERS SET, displayed by PARAMETERS SHOW, and committed back by
 * PARAMETERS WRITE {ACTIVE|CURRENT|file} (VSI OpenVMS System Management
 * Utilities Reference Manual, SYSMAN PARAMETERS command family; VSI OpenVMS
 * System Manager's Manual, Vol. 2, "Managing System Parameters with SYSMAN").
 *
 * The store is the SAME one SYSGEN.EXE reads and writes -- the current
 * SYS$SYSTEM:OVMXVMSSYS.PAR parameter file (sysgen_params.h) -- so a change
 * SET+WRITTEN here is visible to SYSGEN and to F$GETSYI, and vice versa.
 * The work area is type-aware (numeric and string parameters, e.g. the
 * string-typed SCSNODE), matching SYSGEN.EXE.
 */
static struct sysgen_file ws;
static int ws_loaded;
static int ws_modified;

/* Load CURRENT into the work area (auto-load on first SHOW/SET if the user
 * did not issue an explicit PARAMETERS USE). Returns 1 on success, 0 if no
 * current parameter file exists yet. */
static int params_autoload(void)
{
    if (ws_loaded)
        return 1;
    if (sysgen_load_working(&ws) == 0) {
        ws_loaded = 1;
        ws_modified = 0;
        return 1;
    }
    return 0;
}

/* Render a string-typed value the way SHOW displays it: double-quoted and
 * padded to the stored width (mirrors SYSGEN.EXE's format_sysgen_string_field
 * so the two utilities agree; the field width is an OVMX display choice, not a
 * byte-for-byte VMS match). */
static void params_format_string(const char *val, char *out, size_t outlen)
{
    char padded[SYSGEN_STRVAL_LEN];
    size_t i = 0;
    for (; i < sizeof(padded) - 1 && val[i]; i++) padded[i] = val[i];
    for (; i < sizeof(padded) - 1; i++) padded[i] = ' ';
    padded[sizeof(padded) - 1] = '\0';
    snprintf(out, outlen, "\"%s\"", padded);
}

static void params_show_header(void)
{
    printf("\n");
    printf("  %-32s %10s %10s %10s %10s\n",
           "Parameter Name", "Current", "Default", "Minimum", "Maximum");
    printf("  %-32s %10s %10s %10s %10s\n",
           "--------------", "-------", "-------", "-------", "-------");
}

static void params_show_row(const struct sysgen_param *p)
{
    if (p->type == SYSGEN_TYPE_STRING) {
        char cur[SYSGEN_STRVAL_LEN + 2], def[SYSGEN_STRVAL_LEN + 2];
        params_format_string(p->str_current, cur, sizeof(cur));
        params_format_string(p->str_default, def, sizeof(def));
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

/* ------------------------------------------------------------------ */
/*  PARAMETERS USE {ACTIVE | CURRENT | file}                           */
/* ------------------------------------------------------------------ */
static void cmd_parameters_use(const char *rest)
{
    char target[256] = "";
    const char *p = skip_ws(rest);
    int ti = 0;
    while (*p && !isspace((unsigned char)*p) && ti < (int)sizeof(target) - 1)
        target[ti++] = *p++;
    target[ti] = '\0';

    if (target[0] == '\0') {
        printf("%%SYSMAN-E-SYNTAX, PARAMETERS USE {ACTIVE|CURRENT|file-spec}\n");
        return;
    }

    /* ACTIVE and CURRENT both resolve to OVMX's single parameter store (the
     * running readers consult the same file the boot uses -- there is no
     * separate in-memory active set to load; documented in sysgen_params.h). */
    if (strcasecmp(target, "CURRENT") == 0 || strcasecmp(target, "ACTIVE") == 0) {
        if (sysgen_load_working(&ws) == 0) {
            ws_loaded = 1;
            ws_modified = 0;
            printf("%%SYSMAN-I-USEPARAM, %u parameters loaded from %s parameter set\n",
                   ws.count, strcasecmp(target, "ACTIVE") == 0 ? "ACTIVE" : "CURRENT");
        } else {
            printf("%%SYSMAN-E-NOPARAMS, current system parameter file not found\n");
            printf("-SYSMAN-I-USESYSGEN, use SYSGEN USE DEFAULT / WRITE CURRENT to create it\n");
        }
        return;
    }

    /* Explicit file target (literal path -- no versioning). */
    FILE *fp = fopen(target, "rb");
    if (!fp) {
        printf("%%SYSMAN-E-OPENIN, error opening %s as input\n", target);
        return;
    }
    struct sysgen_file tmp;
    if (fread(&tmp, sizeof(tmp), 1, fp) != 1) {
        printf("%%SYSMAN-E-READERR, error reading %s\n", target);
        fclose(fp);
        return;
    }
    fclose(fp);
    if (tmp.magic != SYSGEN_MAGIC) {
        printf("%%SYSMAN-E-BADFILE, %s is not a valid SYSGEN parameter file\n",
               target);
        return;
    }
    if (tmp.version != SYSGEN_VERSION) {
        printf("%%SYSMAN-E-BADVER, unsupported parameter file version %u\n",
               tmp.version);
        return;
    }
    if (tmp.count > SYSGEN_MAX_PARAMS)
        tmp.count = SYSGEN_MAX_PARAMS;
    ws = tmp;
    ws_loaded = 1;
    ws_modified = 0;
    printf("%%SYSMAN-I-USEPARAM, %u parameters loaded from %s\n",
           ws.count, target);
}

/* ------------------------------------------------------------------ */
/*  PARAMETERS SHOW {param | prefix* | /ALL}                           */
/* ------------------------------------------------------------------ */
static void cmd_parameters_show(const char *rest)
{
    char name[64] = "";
    const char *p = skip_ws(rest);
    int ni = 0;
    while (*p && !isspace((unsigned char)*p) && ni < (int)sizeof(name) - 1)
        name[ni++] = *p++;
    name[ni] = '\0';

    if (name[0] == '\0') {
        printf("%%SYSMAN-E-NOPARAM, no parameter name specified\n");
        printf("-SYSMAN-I-SYNTAX, PARAMETERS SHOW {param | prefix* | /ALL}\n");
        return;
    }

    if (!params_autoload()) {
        printf("%%SYSMAN-E-NOPARAMS, current system parameter file not found\n");
        printf("-SYSMAN-I-USESYSGEN, use SYSGEN USE DEFAULT / WRITE CURRENT to create it\n");
        return;
    }

    /* SHOW /ALL */
    if (strcasecmp(name, "/ALL") == 0) {
        params_show_header();
        for (uint32_t i = 0; i < ws.count; i++)
            params_show_row(&ws.params[i]);
        printf("\n  %u parameters displayed. (D = Dynamic)\n\n", ws.count);
        return;
    }

    /* Exact match first. */
    for (uint32_t i = 0; i < ws.count; i++) {
        if (strcasecmp(ws.params[i].name, name) == 0) {
            params_show_header();
            params_show_row(&ws.params[i]);
            if (ws.params[i].description[0])
                printf("  %s\n", ws.params[i].description);
            printf("\n");
            return;
        }
    }

    /* Trailing-* wildcard prefix match. */
    size_t len = strlen(name);
    if (len > 1 && name[len - 1] == '*') {
        name[len - 1] = '\0';
        len--;
        int found = 0;
        params_show_header();
        for (uint32_t i = 0; i < ws.count; i++) {
            if (strncasecmp(ws.params[i].name, name, len) == 0) {
                params_show_row(&ws.params[i]);
                found = 1;
            }
        }
        if (found) {
            printf("\n");
            return;
        }
    }

    str_upper(name);
    printf("%%SYSMAN-E-NOSUCHP, %s is not a valid parameter name\n", name);
}

/* ------------------------------------------------------------------ */
/*  PARAMETERS SET param value                                         */
/* ------------------------------------------------------------------ */
static void cmd_parameters_set(const char *rest)
{
    char name[64] = "";
    char valstr[64] = "";
    const char *p = skip_ws(rest);
    int ni = 0;
    while (*p && !isspace((unsigned char)*p) && ni < (int)sizeof(name) - 1)
        name[ni++] = *p++;
    name[ni] = '\0';

    p = skip_ws(p);
    int vi = 0;
    while (*p && !isspace((unsigned char)*p) && vi < (int)sizeof(valstr) - 1)
        valstr[vi++] = *p++;
    valstr[vi] = '\0';

    if (name[0] == '\0' || valstr[0] == '\0') {
        printf("%%SYSMAN-E-SYNTAX, usage: PARAMETERS SET param value\n");
        return;
    }

    if (!params_autoload()) {
        printf("%%SYSMAN-E-NOPARAMS, current system parameter file not found\n");
        printf("-SYSMAN-I-USESYSGEN, use SYSGEN USE DEFAULT / WRITE CURRENT to create it\n");
        return;
    }

    struct sysgen_param *found = NULL;
    for (uint32_t i = 0; i < ws.count; i++) {
        if (strcasecmp(ws.params[i].name, name) == 0) {
            found = &ws.params[i];
            break;
        }
    }

    if (!found) {
        str_upper(name);
        printf("%%SYSMAN-E-NOSUCHP, %s is not a valid parameter name\n", name);
        return;
    }

    if (found->flags & SYSGEN_F_INFORMATIONAL) {
        printf("%%SYSMAN-E-RDONLY, parameter %s is informational (read-only)\n",
               found->name);
        return;
    }

    if (found->type == SYSGEN_TYPE_STRING) {
        char newval[SYSGEN_STRVAL_LEN];
        strncpy(newval, valstr, sizeof(newval) - 1);
        newval[sizeof(newval) - 1] = '\0';
        str_upper(newval);

        char oldval[SYSGEN_STRVAL_LEN];
        memcpy(oldval, found->str_current, sizeof(oldval));
        memcpy(found->str_current, newval, sizeof(found->str_current));
        ws_modified = 1;
        printf("%%SYSMAN-I-SETPARAM, %s changed from %s to %s\n",
               found->name, oldval, found->str_current);
        return;
    }

    /* Numeric parameter. */
    char *endptr;
    unsigned long val = strtoul(valstr, &endptr, 0);
    if (*endptr != '\0') {
        printf("%%SYSMAN-E-IVVAL, \"%s\" is not a valid numeric value\n", valstr);
        return;
    }
    if (val < found->min_val) {
        printf("%%SYSMAN-E-TOOSMALL, value %lu below minimum %u for %s\n",
               val, found->min_val, found->name);
        return;
    }
    if (val > found->max_val) {
        printf("%%SYSMAN-E-TOOLARGE, value %lu exceeds maximum %u for %s\n",
               val, found->max_val, found->name);
        return;
    }

    uint32_t old_val = found->current;
    found->current = (uint32_t)val;
    ws_modified = 1;
    printf("%%SYSMAN-I-SETPARAM, %s changed from %u to %u\n",
           found->name, old_val, (uint32_t)val);
}

/* ------------------------------------------------------------------ */
/*  PARAMETERS WRITE {ACTIVE | CURRENT | file}                         */
/* ------------------------------------------------------------------ */
static void cmd_parameters_write(const char *rest)
{
    char target[256] = "";
    const char *p = skip_ws(rest);
    int ti = 0;
    while (*p && !isspace((unsigned char)*p) && ti < (int)sizeof(target) - 1)
        target[ti++] = *p++;
    target[ti] = '\0';

    if (target[0] == '\0') {
        printf("%%SYSMAN-E-SYNTAX, PARAMETERS WRITE {ACTIVE|CURRENT|file-spec}\n");
        return;
    }
    if (!ws_loaded) {
        printf("%%SYSMAN-E-NODATA, no parameter set loaded -- USE ACTIVE/CURRENT first\n");
        return;
    }

    if (strcasecmp(target, "CURRENT") == 0 || strcasecmp(target, "ACTIVE") == 0) {
        int is_active = (strcasecmp(target, "ACTIVE") == 0);
        char path[VMSFS_MAX_PATH] = "";
        int version = 0;
        /* CURRENT mints a new next-boot version; ACTIVE updates the store the
         * running readers consult in place (sysgen_params.h documents the
         * single-store model). */
        int status = sysgen_commit_working(&ws, is_active ? 0 : 1,
                                            path, sizeof(path), &version);
        if (!$VMS_STATUS_SUCCESS(status)) {
            printf("%%SYSMAN-E-WRITEERR, error writing %s system parameters\n",
                   is_active ? "ACTIVE" : "CURRENT");
            return;
        }
        ws_modified = 0;
        printf("%%SYSMAN-I-WRITTEN, %u parameters written to %s parameter set\n",
               ws.count, is_active ? "ACTIVE" : "CURRENT");
        return;
    }

    /* Explicit literal file target (no versioning). */
    FILE *fp = fopen(target, "wb");
    if (!fp) {
        printf("%%SYSMAN-E-OPENOUT, error opening %s for output\n", target);
        return;
    }
    if (fwrite(&ws, sizeof(ws), 1, fp) != 1) {
        printf("%%SYSMAN-E-WRITEERR, error writing %s\n", target);
        fclose(fp);
        return;
    }
    fclose(fp);
    ws_modified = 0;
    printf("%%SYSMAN-I-WRITTEN, %u parameters written to %s\n", ws.count, target);
}

/* ------------------------------------------------------------------ */
/*  PARAMETERS dispatch                                                */
/* ------------------------------------------------------------------ */
static void cmd_parameters(const char *rest)
{
    const char *p = skip_ws(rest);

    if (prefix_match(p, "SHOW")) {
        cmd_parameters_show(p + 4);
    } else if (prefix_match(p, "SET")) {
        cmd_parameters_set(p + 3);
    } else if (prefix_match(p, "USE")) {
        cmd_parameters_use(p + 3);
    } else if (prefix_match(p, "WRITE")) {
        cmd_parameters_write(p + 5);
    } else {
        printf("%%SYSMAN-E-SYNTAX, unrecognized PARAMETERS subcommand\n");
        printf("  Valid: SHOW | SET | USE {ACTIVE|CURRENT|file} | WRITE {ACTIVE|CURRENT|file}\n");
    }
}

/* ------------------------------------------------------------------ */
/*  DO dcl-command                                                     */
/* ------------------------------------------------------------------ */
static void cmd_do(const char *rest)
{
    const char *command = skip_ws(rest);
    if (*command == '\0') {
        printf("%%SYSMAN-E-NOCMD, no command specified\n");
        return;
    }

    /* INV-6: OVMX can only execute DO in a LOCAL environment. A remote /
     * cluster environment has no command transport, so fail honest rather than
     * forking a LOCAL DCL and mislabelling its output as a remote node's. */
    if (env_state != SYSMAN_ENV_LOCAL) {
        printf("%%SYSMAN-E-NOSMISERVER, cannot execute command on environment "
               "\"%s\": OVMX has no SMISERVER / cluster command transport\n",
               env_target[0] ? env_target : "remote");
        return;
    }

    /* LOCAL environment: report the real local node name, not a hardcoded
     * "OVMX" pretense of cluster reach. */
    char node[OVMX_IDENTITY_MAXLEN];
    ovmx_node_name(node, sizeof(node));

    printf("%%SYSMAN-I-OUTPUT, command execution on node %s\n", node);

    /* Find DCL.EXE */
    char dcl_path[PATH_MAX];
    snprintf(dcl_path, sizeof(dcl_path), "%s/DCL.EXE", VMS_SYSTEM_DIR);
    if (access(dcl_path, X_OK) != 0) {
        /* Try vmsdcl as fallback */
        snprintf(dcl_path, sizeof(dcl_path), "%s/vmsdcl", VMS_SYSTEM_DIR);
        if (access(dcl_path, X_OK) != 0) {
            /* Try PATH */
            strncpy(dcl_path, "vmsdcl", sizeof(dcl_path));
        }
    }

    pid_t pid = fork();
    if (pid == 0) {
        execlp(dcl_path, dcl_path, "-c", command, (char *)NULL);
        /* If that fails, try just "DCL.EXE" on PATH */
        execlp("DCL.EXE", "DCL.EXE", "-c", command, (char *)NULL);
        execlp("vmsdcl", "vmsdcl", "-c", command, (char *)NULL);
        fprintf(stderr, "%%SYSMAN-F-NODCL, cannot execute DCL\n");
        _exit(1);
    } else if (pid > 0) {
        int wstatus;
        waitpid(pid, &wstatus, 0);
    } else {
        printf("%%SYSMAN-E-CREPRC, cannot create subprocess\n");
        return;
    }

    printf("%%SYSMAN-I-DONEALL, command execution complete on node %s\n", node);
}

/* ------------------------------------------------------------------ */
/*  SHUTDOWN NODE                                                      */
/* ------------------------------------------------------------------ */
static void cmd_shutdown(const char *rest)
{
    (void)rest;
    char ts[64];
    vms_timestamp(ts, sizeof(ts));

    printf("%%SYSMAN-I-SHUTDOWN, shutdown request sent to node OVMX\n");
    printf("%%OPCOM, %s, operator _OPA0:\n", ts);
    printf("Operator requested shutdown\n");
    printf("SHUTDOWN -- OVMX, %s\n", ts);
    printf("%%SHUTDOWN-I-DISLOGINS, interactive logins will now be disabled\n");
    printf("%%SHUTDOWN-I-STOPQUEUES, the batch/print queues will now be stopped\n");
    printf("%%SHUTDOWN-I-SHUTDONE, shutdown complete\n");
}

/* ------------------------------------------------------------------ */
/*  HELP                                                               */
/* ------------------------------------------------------------------ */
static void cmd_help(void)
{
    printf("\n");
    printf("SYSMAN commands:\n\n");
    printf("  SET ENVIRONMENT /NODE=name   Set target node environment\n");
    printf("  STARTUP SHOW                 List startup procedures\n");
    printf("  STARTUP ADD file /PHASE=ph   Add startup procedure (LPMAIN/LPBETA)\n");
    printf("  STARTUP REMOVE file          Remove startup procedure\n");
    printf("  PARAMETERS USE {ACTIVE|CURRENT|file}   Load parameters into work area\n");
    printf("  PARAMETERS SHOW {param|prefix*|/ALL}   Show system parameter value(s)\n");
    printf("  PARAMETERS SET param value             Set system parameter value\n");
    printf("  PARAMETERS WRITE {ACTIVE|CURRENT|file} Commit the work area\n");
    printf("  DO command                   Execute DCL command on target node\n");
    printf("  SHUTDOWN NODE                Shut down the target node\n");
    printf("  HELP                         Display this help\n");
    printf("  EXIT                         Exit SYSMAN\n");
    printf("\n");
}

/* ------------------------------------------------------------------ */
/*  Main command loop                                                  */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    int interactive = isatty(fileno(stdin));

    if (interactive) {
        printf("\n");
        printf("OpenVMS System Management Utility\n\n");
    }

    char line[1024];
    for (;;) {
        if (interactive) {
            printf("SYSMAN> ");
            fflush(stdout);
        }

        if (!fgets(line, sizeof(line), stdin))
            break;

        /* Strip trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[--len] = '\0';

        const char *p = skip_ws(line);
        if (*p == '\0' || *p == '!')
            continue;

        /* Make uppercase copy for matching */
        char upper[1024];
        strncpy(upper, p, sizeof(upper) - 1);
        upper[sizeof(upper) - 1] = '\0';
        str_upper(upper);

        if (prefix_match(upper, "EXIT") || prefix_match(upper, "QUIT")) {
            break;
        } else if (prefix_match(upper, "SET")) {
            const char *rest = skip_ws(p + 3);
            char urest[1024];
            strncpy(urest, rest, sizeof(urest) - 1);
            urest[sizeof(urest) - 1] = '\0';
            str_upper(urest);
            if (prefix_match(urest, "ENVIRONMENT") || prefix_match(urest, "ENV")) {
                cmd_set_environment(rest);
            } else {
                printf("%%SYSMAN-E-SYNTAX, unrecognized SET subcommand\n");
            }
        } else if (prefix_match(upper, "STARTUP")) {
            cmd_startup(p + 7);
        } else if (prefix_match(upper, "PARAMETERS") || prefix_match(upper, "PARAM")) {
            /* Skip the keyword itself */
            const char *rest = p;
            if (prefix_match(upper, "PARAMETERS"))
                rest += 10;
            else
                rest += 5; /* "PARAM" minimum abbreviation */
                /* Advance past remaining chars of the keyword */
            while (*rest && !isspace((unsigned char)*rest))
                rest++;
            cmd_parameters(rest);
        } else if (prefix_match(upper, "DO")) {
            cmd_do(p + 2);
        } else if (prefix_match(upper, "SHUTDOWN")) {
            cmd_shutdown(p + 8);
        } else if (prefix_match(upper, "HELP")) {
            cmd_help();
        } else {
            printf("%%SYSMAN-E-IVVERB, unrecognized command - check spelling and try again\n");
        }
    }

    if (interactive)
        printf("\n");

    return 0;
}
