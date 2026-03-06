/*
 * vms_sysgen.c - SYSGEN System Generation Utility for OVMX
 *
 * Interactive utility for viewing and modifying VMS system parameters.
 * Parameters are stored in a binary database file compatible with
 * the sysgen_params.h format.
 *
 * Usage: SYSGEN
 *   Commands:
 *     USE CURRENT          - Load from /etc/ovmx/sysparams.dat
 *     USE DEFAULT          - Load factory defaults
 *     USE <filename>       - Load from specified file
 *     SHOW <parameter>     - Display one parameter
 *     SHOW /ALL            - Display all parameters
 *     SET <param> <value>  - Change a parameter value
 *     WRITE CURRENT        - Write to /etc/ovmx/sysparams.dat
 *     WRITE <filename>     - Write to specified file
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

/* ================================================================== */
/*                    Default Parameter Database                       */
/* ================================================================== */

static const struct sysgen_param default_params[] = {
    {"MAXPROCESSCNT",   64,      64,      4,       1024,     0,
     "Maximum number of concurrent processes"},
    {"CHANNELCNT",      16,      16,      4,       256,      SYSGEN_F_DYNAMIC,
     "Number of I/O channels per process"},
    {"DEFPRI",          4,       4,       0,       31,       SYSGEN_F_DYNAMIC,
     "Default process priority"},
    {"MAXPRI",          31,      31,      0,       31,       0,
     "Maximum process priority"},
    {"MAXBUF",          8192,    8192,    512,     65536,    SYSGEN_F_DYNAMIC,
     "Maximum buffered I/O byte count"},
    {"PQL_DWSDEFAULT",  256,     256,     64,      65536,    SYSGEN_F_DYNAMIC,
     "Default working set size"},
    {"PQL_DWSQUOTA",    512,     512,     64,      65536,    SYSGEN_F_DYNAMIC,
     "Working set quota"},
    {"PQL_DWSEXTENT",   2048,    2048,    64,      262144,   SYSGEN_F_DYNAMIC,
     "Working set extent"},
    {"PQL_DENQLM",      200,     200,     4,       32767,    SYSGEN_F_DYNAMIC,
     "Default enqueue limit"},
    {"PQL_DFILLM",      100,     100,     4,       8192,     SYSGEN_F_DYNAMIC,
     "Default open file limit"},
    {"PQL_DTQELM",      20,      20,      1,       1024,     SYSGEN_F_DYNAMIC,
     "Default timer queue entry limit"},
    {"PQL_DBIOLM",      40,      40,      4,       4096,     SYSGEN_F_DYNAMIC,
     "Default buffered I/O limit"},
    {"PQL_DDIOLM",      40,      40,      4,       4096,     SYSGEN_F_DYNAMIC,
     "Default direct I/O limit"},
    {"PQL_DBYTLM",      65536,   65536,   1024,    16777216, SYSGEN_F_DYNAMIC,
     "Default buffered I/O byte limit"},
    {"PQL_DPGFLQUOTA",  50000,   50000,   1024,    4194304,  SYSGEN_F_DYNAMIC,
     "Default page file quota"},
    {"VIRTUALPAGECNT",  1048576, 1048576, 1024,    67108864, 0,
     "Virtual page count"},
    {"GBLPAGES",        8192,    8192,    256,     4194304,  SYSGEN_F_DYNAMIC,
     "Global pages"},
    {"GBLSECTIONS",     256,     256,     16,      4096,     SYSGEN_F_DYNAMIC,
     "Global sections"},
    {"LNMPHASHTBL",     128,     128,     16,      8192,     SYSGEN_F_DYNAMIC,
     "Logical name hash table size"},
    {"ACP_MAPCACHE",    32,      32,      4,       256,      SYSGEN_F_DYNAMIC,
     "ACP map cache size"},
    {"BALSETCNT",       16,      16,      4,       256,      0,
     "Maximum number of processes in balance set"},
    {"IRPCOUNT",        256,     256,     32,      4096,     0,
     "Number of I/O request packets"},
    {"SRPCOUNT",        256,     256,     32,      4096,     0,
     "Number of small request packets"},
    {"LRPCOUNT",        32,      32,      4,       512,      0,
     "Number of large request packets"},
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
        if (load_from_file(SYSGEN_DEFAULT_PATH) == 0) {
            printf("%%SYSGEN-I-LOADED, %u parameters loaded from %s\n",
                   working_set.count, SYSGEN_DEFAULT_PATH);
        } else {
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

static void show_param(const struct sysgen_param *p)
{
    printf("  %-32s %10u %10u %10u %10u",
           p->name, p->current, p->default_val, p->min_val, p->max_val);
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

    /* Parse numeric value */
    char *endptr;
    unsigned long val = strtoul(value_str, &endptr, 0);
    if (*endptr != '\0') {
        fprintf(stderr,
                "%%SYSGEN-E-IVVAL, \"%s\" is not a valid numeric value\n",
                value_str);
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
        write_to_file(SYSGEN_DEFAULT_PATH);
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
    printf("  USE CURRENT          Load parameters from %s\n",
           SYSGEN_DEFAULT_PATH);
    printf("  USE DEFAULT          Load factory default parameters\n");
    printf("  USE <filename>       Load parameters from specified file\n");
    printf("\n");
    printf("  SHOW <parameter>     Display a single parameter\n");
    printf("  SHOW <prefix>*       Display parameters matching prefix\n");
    printf("  SHOW /ALL            Display all parameters\n");
    printf("\n");
    printf("  SET <param> <value>  Change a parameter value\n");
    printf("\n");
    printf("  WRITE CURRENT        Write parameters to %s\n",
           SYSGEN_DEFAULT_PATH);
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
