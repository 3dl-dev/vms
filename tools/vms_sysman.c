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

/* ------------------------------------------------------------------ */
/*  Paths                                                              */
/* ------------------------------------------------------------------ */
#include "ovmx_layout.h"

#define SYSMGR_DIR     VMS_MANAGER_DIR
#define STARTUP_LIST   SYSMGR_DIR "/STARTUP_LIST.DAT"
#define SYSPARAMS_DAT  SYSMGR_DIR "/SYSPARAMS.DAT"

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
/*  SET ENVIRONMENT                                                    */
/* ------------------------------------------------------------------ */
static void cmd_set_environment(const char *rest)
{
    (void)rest;
    printf("Environment set to local node OVMX\n");
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
/*  PARAMETERS SHOW param                                              */
/* ------------------------------------------------------------------ */
static void cmd_parameters_show(const char *rest)
{
    char name[64] = "";
    const char *p = skip_ws(rest);
    int ni = 0;
    while (*p && !isspace((unsigned char)*p) && ni < (int)sizeof(name) - 1)
        name[ni++] = *p++;
    name[ni] = '\0';
    str_upper(name);

    if (name[0] == '\0') {
        printf("%%SYSMAN-E-NOPARAM, no parameter name specified\n");
        return;
    }

    FILE *fp = fopen(SYSPARAMS_DAT, "rb");
    if (!fp) {
        printf("%%SYSMAN-E-NOPARAMS, system parameter file not found\n");
        printf("  (Run SYSGEN USE DEFAULT / SYSGEN WRITE CURRENT to create it)\n");
        return;
    }

    /* Read and validate sysgen_file header */
    struct sysgen_file sf;
    if (fread(&sf, sizeof(sf), 1, fp) != 1) {
        printf("%%SYSMAN-E-READERR, error reading parameter file\n");
        fclose(fp);
        return;
    }
    fclose(fp);

    if (sf.magic != SYSGEN_MAGIC) {
        printf("%%SYSMAN-E-BADFILE, %s is not a valid SYSGEN parameter file\n",
               SYSPARAMS_DAT);
        return;
    }
    if (sf.version != SYSGEN_VERSION) {
        printf("%%SYSMAN-E-BADVER, unsupported parameter file version %u\n",
               sf.version);
        return;
    }
    if (sf.count > SYSGEN_MAX_PARAMS)
        sf.count = SYSGEN_MAX_PARAMS;

    int found = 0;
    for (uint32_t i = 0; i < sf.count; i++) {
        struct sysgen_param *param = &sf.params[i];
        char pname[33];
        memcpy(pname, param->name, 32);
        pname[32] = '\0';
        /* Trim trailing spaces */
        for (int j = 31; j >= 0 && (pname[j] == ' ' || pname[j] == '\0'); j--)
            pname[j] = '\0';
        str_upper(pname);
        if (strcmp(pname, name) == 0) {
            found = 1;
            printf("\nParameter Name    Current    Default    Minimum    Maximum\n");
            printf("--------------    -------    -------    -------    -------\n");
            printf("%-16s  %-9u  %-9u  %-9u  %u\n",
                   pname, param->current, param->default_val,
                   param->min_val, param->max_val);
            if (param->description[0])
                printf("  %s\n", param->description);
            printf("\n");
            break;
        }
    }

    if (!found)
        printf("%%SYSMAN-E-NOTFOUND, parameter %s not found\n", name);
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
    str_upper(name);

    p = skip_ws(p);
    int vi = 0;
    while (*p && !isspace((unsigned char)*p) && vi < (int)sizeof(valstr) - 1)
        valstr[vi++] = *p++;
    valstr[vi] = '\0';

    if (name[0] == '\0' || valstr[0] == '\0') {
        printf("%%SYSMAN-E-SYNTAX, usage: PARAMETERS SET param value\n");
        return;
    }

    uint32_t value = (uint32_t)strtoul(valstr, NULL, 0);

    FILE *fp = fopen(SYSPARAMS_DAT, "r+b");
    if (!fp) {
        printf("%%SYSMAN-E-NOPARAMS, system parameter file not found\n");
        return;
    }

    /* Read and validate sysgen_file header */
    struct sysgen_file sf;
    if (fread(&sf, sizeof(sf), 1, fp) != 1) {
        printf("%%SYSMAN-E-READERR, error reading parameter file\n");
        fclose(fp);
        return;
    }

    if (sf.magic != SYSGEN_MAGIC) {
        printf("%%SYSMAN-E-BADFILE, %s is not a valid SYSGEN parameter file\n",
               SYSPARAMS_DAT);
        fclose(fp);
        return;
    }
    if (sf.version != SYSGEN_VERSION) {
        printf("%%SYSMAN-E-BADVER, unsupported parameter file version %u\n",
               sf.version);
        fclose(fp);
        return;
    }
    if (sf.count > SYSGEN_MAX_PARAMS)
        sf.count = SYSGEN_MAX_PARAMS;

    int found = 0;
    for (uint32_t i = 0; i < sf.count; i++) {
        struct sysgen_param *param = &sf.params[i];
        char pname[33];
        memcpy(pname, param->name, 32);
        pname[32] = '\0';
        for (int j = 31; j >= 0 && (pname[j] == ' ' || pname[j] == '\0'); j--)
            pname[j] = '\0';
        str_upper(pname);
        if (strcmp(pname, name) == 0) {
            found = 1;
            if (value < param->min_val || value > param->max_val) {
                printf("%%SYSMAN-E-RANGE, value %u outside range [%u, %u]\n",
                       value, param->min_val, param->max_val);
                fclose(fp);
                return;
            }
            param->current = value;
            /* Write entire file back */
            fseek(fp, 0, SEEK_SET);
            fwrite(&sf, sizeof(sf), 1, fp);
            printf("%%SYSMAN-I-SETPARAM, %s set to %u\n", pname, value);
            break;
        }
    }
    fclose(fp);

    if (!found)
        printf("%%SYSMAN-E-NOTFOUND, parameter %s not found\n", name);
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
    } else {
        printf("%%SYSMAN-E-SYNTAX, unrecognized PARAMETERS subcommand\n");
        printf("  Valid: PARAMETERS SHOW param | PARAMETERS SET param value\n");
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

    printf("%%SYSMAN-I-OUTPUT, command execution on node OVMX\n");

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

    printf("%%SYSMAN-I-DONEALL, command execution completed\n");
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
    printf("  PARAMETERS SHOW param        Show system parameter value\n");
    printf("  PARAMETERS SET param value   Set system parameter value\n");
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
