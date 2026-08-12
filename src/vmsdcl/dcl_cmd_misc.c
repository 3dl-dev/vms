/*
 * dcl_cmd_misc.c - DCL miscellaneous command implementations
 *
 * DIFFERENCES, SORT, DUMP, ANALYZE, MAIL, INQUIRE, MONITOR, SYSGEN,
 * SYSMAN, REPLY, REQUEST, ACCOUNTING, HELP, RECALL, TCPIP,
 * MOUNT, DISMOUNT, EDIT, ATTACH, CONVERT, INSTALL, LINK, PHONE, PRODUCT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <pwd.h>
#include <grp.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <limits.h>
#include <mntent.h>
#include <sys/statvfs.h>

#include "dcl/context.h"
#include "dcl/terminal.h"
#include "dcl/parser.h"
#include "dcl/symbol.h"
#include "dcl/cdu.h"
#include "dcl/dcl_cmd.h"
#include "dcl/disk_logical.h"
#include "ssdef.h"
#include "vms/logical.h"
#include "vms/privs.h"
#include "opcdef.h"
#include "ovmx_accounting.h"
#include "starlet.h"
#include "ovmx_identity.h"
#include "vmsfs/filespec.h"
#include "vmsfs/device.h"
#include "ovmx_layout.h"
#include "vms_kif.h"

#ifdef HAVE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

/*
 * DIFFERENCES - Compare two files and show differences.
 * Format: DIFFERENCES file1 file2
 * Implements a simple line-by-line diff with VMS-style output.
 */
int cmd_differences(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOFILE",
                  "missing file specification(s)");
        return SS$_BADPARAM;
    }

    char path1[1024], path2[1024];
    dcl_resolve_path(ctx, cmd->params[0], path1, sizeof(path1));
    dcl_resolve_path(ctx, cmd->params[1], path2, sizeof(path2));

    FILE *f1 = fopen(path1, "r");
    if (!f1) {
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }
    FILE *f2 = fopen(path2, "r");
    if (!f2) {
        fclose(f1);
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[1]);
        return SS$_NOSUCHFILE;
    }

    /* VMS DIFFERENCES header */
    char vms1[256], vms2[256];
    const char *b1 = strrchr(path1, '/'); if (b1) b1++; else b1 = path1;
    const char *b2 = strrchr(path2, '/'); if (b2) b2++; else b2 = path2;
    size_t ni;
    for (ni = 0; b1[ni] && ni < sizeof(vms1)-1; ni++)
        vms1[ni] = (char)toupper((unsigned char)b1[ni]);
    vms1[ni] = '\0';
    for (ni = 0; b2[ni] && ni < sizeof(vms2)-1; ni++)
        vms2[ni] = (char)toupper((unsigned char)b2[ni]);
    vms2[ni] = '\0';

    printf("\n");
    printf("*************************\n");
    printf("File SYS$DISK:[]%s;1\n", vms1);
    printf("File SYS$DISK:[]%s;1\n", vms2);
    printf("*************************\n\n");

    char line1[4096], line2[4096];
    int lineno = 0;
    int diffs = 0;

    while (1) {
        char *r1 = fgets(line1, sizeof(line1), f1);
        char *r2 = fgets(line2, sizeof(line2), f2);
        lineno++;

        if (!r1 && !r2) break;

        /* Remove trailing newlines for comparison */
        if (r1) {
            size_t l = strlen(line1);
            if (l > 0 && line1[l-1] == '\n') line1[l-1] = '\0';
        }
        if (r2) {
            size_t l = strlen(line2);
            if (l > 0 && line2[l-1] == '\n') line2[l-1] = '\0';
        }

        if (!r1 || !r2 || strcmp(line1, line2) != 0) {
            diffs++;
            printf("***\n");
            if (r1) printf("  (%d) %s\n", lineno, line1);
            else    printf("  (%d) <end of file>\n", lineno);
            printf("***\n");
            if (r2) printf("  (%d) %s\n", lineno, line2);
            else    printf("  (%d) <end of file>\n", lineno);
            printf("\n");

            if (!r1 || !r2) break;
        }
    }

    fclose(f1);
    fclose(f2);

    if (diffs == 0) {
        printf("Number of difference sections found: 0\n\n");
        printf("%%DIFF-I-IDENT, files are identical\n");
    } else {
        printf("Number of difference sections found: %d\n", diffs);
    }

    return SS$_NORMAL;
}

/* qsort comparators for SORT command */
static int sort_cmp_asc(const void *a, const void *b)
{
    return strcasecmp(*(const char **)a, *(const char **)b);
}

static int sort_cmp_desc(const void *a, const void *b)
{
    return strcasecmp(*(const char **)b, *(const char **)a);
}

/*
 * SORT - Sort a file.
 * Format: SORT input-file output-file
 * Reads the input file line by line, sorts, writes to output.
 */
int cmd_sort(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOFILE",
                  "missing input and/or output file specification");
        return SS$_BADPARAM;
    }

    char src_path[1024], dst_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], src_path, sizeof(src_path));
    dcl_resolve_path(ctx, cmd->params[1], dst_path, sizeof(dst_path));

    /* Read all lines */
    FILE *fp = fopen(src_path, "r");
    if (!fp) {
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    /* Collect lines into dynamic array */
    char **lines = NULL;
    size_t line_count = 0;
    size_t line_cap = 0;
    char buf[4096];

    while (fgets(buf, sizeof(buf), fp)) {
        if (line_count >= line_cap) {
            size_t new_cap = (line_cap == 0) ? 64 : line_cap * 2;
            char **new_lines = realloc(lines, new_cap * sizeof(char *));
            if (!new_lines) {
                dcl_error("DCL", 4, "INSFMEM", "insufficient memory for sort");
                fclose(fp);
                for (size_t i = 0; i < line_count; i++) free(lines[i]);
                free(lines);
                return SS$_INSFMEM;
            }
            lines = new_lines;
            line_cap = new_cap;
        }
        lines[line_count] = strdup(buf);
        if (!lines[line_count]) {
            dcl_error("DCL", 4, "INSFMEM", "insufficient memory for sort");
            fclose(fp);
            for (size_t i = 0; i < line_count; i++) free(lines[i]);
            free(lines);
            return SS$_INSFMEM;
        }
        line_count++;
    }
    fclose(fp);

    /* Sort: /REVERSE reverses, default ascending case-insensitive */
    int reverse = dcl_has_qualifier(cmd, "REVERSE");

    /* Sort using qsort with case-insensitive comparison */
    if (!reverse) {
        qsort(lines, line_count, sizeof(char *), sort_cmp_asc);
    } else {
        qsort(lines, line_count, sizeof(char *), sort_cmp_desc);
    }

    /* Write sorted output */
    FILE *out = fopen(dst_path, "w");
    if (!out) {
        dcl_error("RMS", 2, "CRE", "cannot create - %s", cmd->params[1]);
        for (size_t i = 0; i < line_count; i++) free(lines[i]);
        free(lines);
        return SS$_FILACCERR;
    }

    for (size_t i = 0; i < line_count; i++) {
        fputs(lines[i], out);
        free(lines[i]);
    }
    free(lines);
    fclose(out);

    return SS$_NORMAL;
}

int cmd_dump(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOFILE", "missing file specification");
        return SS$_BADPARAM;
    }

    char linux_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], linux_path, sizeof(linux_path));

    FILE *fp = fopen(linux_path, "rb");
    if (!fp) {
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    /* /BLOCKS=n — limit to n 512-byte blocks (0 = all) */
    long max_blocks = 0;
    const char *blk_str = dcl_qualifier_value(cmd, "BLOCKS");
    if (blk_str && blk_str[0]) max_blocks = strtol(blk_str, NULL, 10);

    /* VMS DUMP header */
    char vms_name[256];
    const char *bn = strrchr(linux_path, '/');
    if (bn) bn++; else bn = linux_path;
    size_t i;
    for (i = 0; bn[i] && i < sizeof(vms_name)-1; i++)
        vms_name[i] = (char)toupper((unsigned char)bn[i]);
    vms_name[i] = '\0';

    printf("\nDump of file SYS$DISK:[]%s;1\n\n", vms_name);
    printf("File ID (0,0,0)  End of file block 0  Offset 0\n\n");

    unsigned char buf[16];
    long offset = 0;
    size_t n;
    long block = 0;
    int in_block_start = 1;

    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        /* Print block header at start of each 512-byte block */
        if (in_block_start || (offset % 512 == 0)) {
            if (offset > 0 && offset % 512 == 0) {
                block++;
                if (max_blocks > 0 && block >= max_blocks) break;
                printf("\nVirtual block number %ld (00000%02lX), 512 (0200) bytes\n\n",
                       block + 1, block * 512);
            } else if (in_block_start) {
                printf("Virtual block number 1 (00000000), 512 (0200) bytes\n\n");
                in_block_start = 0;
            }
        }

        /* Print offset (VMS style: relative to start of current block) */
        long block_offset = offset % 512;
        printf("%08lX ", block_offset);

        /* Hex bytes (4 groups of 4, space between) */
        for (size_t j = 0; j < 16; j++) {
            if (j > 0 && j % 4 == 0) printf(" ");
            if (j < n)
                printf("%02X", buf[j]);
            else
                printf("  ");
        }

        printf("  ");

        /* ASCII */
        for (size_t j = 0; j < n; j++) {
            printf("%c", isprint(buf[j]) ? buf[j] : '.');
        }
        printf("\n");

        offset += (long)n;
    }

    fclose(fp);
    printf("\n");

    return SS$_NORMAL;
}

/* ================================================================== */
/*           External Utility Executor                                 */
/* ================================================================== */

/*
 * dcl_exec_utility - Fork/exec a VMS utility and wait for completion.
 *
 * Activates the utility image out of SYS$SYSTEM by TRANSLATING the SYS$SYSTEM
 * logical -- the same path RUN and foreign-command activation take (see
 * dcl_exec_foreign_command()/dcl_activate_image() -> dcl_resolve_path() ->
 * vmsfs_to_linux_path() -> lnm_translate()). On OpenVMS every SYS$SYSTEM: image
 * is located this way: `$ RUN SYS$SYSTEM:image` and DCL's own dispatch of
 * SYS$SYSTEM:*.EXE utilities both resolve the SYS$SYSTEM logical, which is why a
 * `DEFINE/SYSTEM SYS$SYSTEM ...` (or a rooted/relocated system disk) relocates
 * where utilities activate from (VSI OpenVMS DCL Dictionary, RUN entry, "the
 * command interpreter uses SYS$SYSTEM as the default device and directory";
 * VSI OpenVMS System Manager's Manual, the SYS$SYSTEM system logical name).
 *
 * This USED TO build VMS_SYSTEM_DIR/exe directly -- a compile-time Linux path
 * (SYSDISK_MOUNT/SYS0/SYSCOMMON/SYSEXE, ovmx_layout.h) that BYPASSED the
 * logical. That made DEFINE/SYSTEM SYS$SYSTEM relocate RUN but NOT utility
 * activation (SYSGEN/MAIL/AUTHORIZE/...) -- a split-brain and exactly the first
 * thing a VMS admin tests. Now both paths honour the one logical (vms-7d8,
 * governing principle vms-8ad: operation VMS-exact, identity OVMX). The
 * translation machinery is vms-240's unified translator, reused, not a second
 * copy.
 *
 * @exe_name:  Binary name (e.g. "SYSGEN.EXE", "MAIL.EXE")
 * @facility:  Error message facility (e.g. "SYSGEN", "MAIL")
 * @argv:      NULL-terminated argument vector (argv[0] is placeholder)
 * @argc:      Number of arguments (not counting NULL terminator)
 *
 * Returns VMS status code.
 */
int dcl_exec_utility(const char *exe_name, const char *facility,
                            char *argv[], int argc)
{
    (void)argc;

    /*
     * Resolve SYS$SYSTEM:<exe_name> through the SAME translator RUN uses. The
     * ':' makes dcl_resolve_path() hand the whole spec to vmsfs, which
     * translates the SYS$SYSTEM logical (relocated by DEFINE/SYSTEM or a rooted
     * disk) before producing the Linux path.
     */
    char sys_path[PATH_MAX];
    char vms_spec[PATH_MAX];
    snprintf(vms_spec, sizeof(vms_spec), "SYS$SYSTEM:%s", exe_name);

    struct dcl_context *ctx = dcl_get_context();

    const char *bin = NULL;
    if (ctx &&
        dcl_resolve_path(ctx, vms_spec, sys_path, sizeof(sys_path)) == 0 &&
        access(sys_path, X_OK) == 0)
        bin = sys_path;

    /*
     * Ultimate fallback default (INV-6 honest, never a silent fake): only if
     * SYS$SYSTEM was untranslatable in this process (no LNM manager / no ctx),
     * fall back to the compile-time SYS$SYSTEM location so a minimal host-tooling
     * invocation still works -- mirroring how RUN degrades. This is the DEFAULT
     * equivalence of SYS$SYSTEM, not a bypass of a redefinition: when SYS$SYSTEM
     * IS defined, the translation above wins and this is never consulted.
     */
    char def_path[PATH_MAX];
    if (!bin) {
        snprintf(def_path, sizeof(def_path), "%s/%s", VMS_SYSTEM_DIR, exe_name);
        if (access(def_path, X_OK) == 0)
            bin = def_path;
    }

    /* Set argv[0] to resolved path or exe_name for PATH search */
    argv[0] = (char *)(bin ? bin : exe_name);

    pid_t pid = fork();
    if (pid == 0) {
        if (bin)
            execv(bin, argv);
        execvp(exe_name, argv);
        fprintf(stderr, "%%%s-F-NOIMG, cannot execute %s\n", facility, exe_name);
        _exit(1);
    } else if (pid > 0) {
        extern volatile sig_atomic_t dcl_running_child;
        dcl_running_child = (sig_atomic_t)pid;
        int wstatus;
        waitpid(pid, &wstatus, WUNTRACED);
        dcl_running_child = 0;
        if (WIFSTOPPED(wstatus)) {
            printf("\nInterrupt\n");
            ctx->interrupted_pid = pid;
            return SS$_ABORT;
        }
        if (WIFEXITED(wstatus))
            return (WEXITSTATUS(wstatus) == 0) ? SS$_NORMAL : SS$_ABORT;
    } else {
        dcl_error("DCL", 4, "CREPRC", "cannot create process for %s", facility);
        return SS$_ABORT;
    }
    return SS$_NORMAL;
}

/* ================================================================== */
/*                        ANALYZE Command                              */
/* ================================================================== */

int cmd_analyze(struct dcl_command *cmd)
{
    const char *qualifier = NULL;
    const char *param = NULL;

    if (dcl_has_qualifier(cmd, "DISK_STRUCTURE"))
        qualifier = "/DISK_STRUCTURE";
    else if (dcl_has_qualifier(cmd, "SYSTEM"))
        qualifier = "/SYSTEM";
    else if (dcl_has_qualifier(cmd, "IMAGE"))
        qualifier = "/IMAGE";
    else if (dcl_has_qualifier(cmd, "OBJECT"))
        qualifier = "/OBJECT";

    if (!qualifier) {
        if (cmd->param_count >= 1 && cmd->params[0][0] == '/') {
            qualifier = cmd->params[0];
            param = (cmd->param_count >= 2) ? cmd->params[1] : NULL;
        } else {
            dcl_error("ANALYZE", 2, "NOKEYW",
                      "qualifier required (/DISK_STRUCTURE, /SYSTEM, /IMAGE, /OBJECT)");
            return SS$_BADPARAM;
        }
    } else {
        param = (cmd->param_count >= 1 && cmd->params[0][0] != '\0')
                ? cmd->params[0] : NULL;
    }

    char *argv[8] = {NULL};
    int argc = 0;
    argv[argc++] = NULL; /* placeholder for binary path */
    argv[argc++] = (char *)qualifier;
    if (param)
        argv[argc++] = (char *)param;
    argv[argc] = NULL;
    return dcl_exec_utility("ANALYZE.EXE", "ANALYZE", argv, argc);
}

/* MAIL — pass qualifiers and params through */
int cmd_mail(struct dcl_command *cmd)
{
    char *argv[64] = {NULL};
    int argc = 0;
    argv[argc++] = NULL; /* placeholder */
    for (int i = 0; i < cmd->qualifier_count && argc < 62; i++)
        argv[argc++] = cmd->qualifiers[i].name;
    for (int i = 0; i < cmd->param_count && argc < 62; i++) {
        if (cmd->params[i][0] != '\0')
            argv[argc++] = cmd->params[i];
    }
    argv[argc] = NULL;
    return dcl_exec_utility("MAIL.EXE", "MAIL", argv, argc);
}

/*
 * INQUIRE - Prompt user for input, store in symbol.
 */
int cmd_inquire(struct dcl_command *cmd)
{
    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOKEYW", "missing symbol name");
        return SS$_BADPARAM;
    }

    const char *symbol_name = cmd->params[0];
    const char *prompt_text = (cmd->param_count >= 2) ? cmd->params[1] : "";

    /* Display prompt */
    if (prompt_text[0]) {
        printf("%s: ", prompt_text);
    } else {
        /* Default prompt is symbol name */
        char upper_name[256];
        size_t i;
        for (i = 0; i < sizeof(upper_name) - 1 && symbol_name[i]; i++)
            upper_name[i] = (char)toupper((unsigned char)symbol_name[i]);
        upper_name[i] = '\0';
        printf("%s: ", upper_name);
    }
    fflush(stdout);

    char buf[1024];
    if (!fgets(buf, sizeof(buf), stdin)) {
        dcl_sym_set(symbol_name, "", DCL_SYM_LOCAL);
        return SS$_NORMAL;
    }

    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';

    /* Unless /NOPUNCTUATION, upcase the input */
    if (!dcl_has_qualifier(cmd, "NOPUNCTUATION")) {
        for (size_t i = 0; buf[i]; i++) {
            buf[i] = (char)toupper((unsigned char)buf[i]);
        }
    }

    dcl_sym_set(symbol_name, buf, DCL_SYM_LOCAL);
    return SS$_NORMAL;
}

/* MONITOR — pass subcommand (default SYSTEM) */
int cmd_monitor(struct dcl_command *cmd)
{
    const char *subcmd = (cmd->param_count >= 1 && cmd->params[0][0] != '\0')
                         ? cmd->params[0] : "SYSTEM";
    char *argv[4] = {NULL, (char *)subcmd, NULL};
    return dcl_exec_utility("MONITOR.EXE", "MONITOR", argv, 2);
}

/* SYSGEN — interactive, no args */
int cmd_sysgen(struct dcl_command *cmd)
{
    (void)cmd;
    char *argv[2] = {NULL, NULL};
    return dcl_exec_utility("SYSGEN.EXE", "SYSGEN", argv, 1);
}

/* SYSMAN — interactive, no args */
int cmd_sysman(struct dcl_command *cmd)
{
    (void)cmd;
    char *argv[2] = {NULL, NULL};
    return dcl_exec_utility("SYSMAN.EXE", "SYSMAN", argv, 1);
}

/* ================================================================== */
/*           OPCOM Commands: REPLY and REQUEST                         */
/* ================================================================== */

/*
 * REPLY /ENABLE - Enable the current terminal as an operator terminal.
 * REPLY /DISABLE - Disable operator terminal.
 *
 * On real VMS, REPLY /ENABLE marks the terminal as an operator console.
 * Messages sent via sys$sndopr are then written to enabled terminals.
 * In OVMX, we log the enable/disable event to OPERATOR.LOG and
 * print a confirmation message — operator messages go to the log.
 *
 * Usage:
 *   REPLY /ENABLE[=class]   - enable operator messages
 *   REPLY /DISABLE          - disable operator terminal
 *   REPLY /TO=rqid "text"   - reply to a pending request
 */
int cmd_reply(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();
    /*
     * NO FABRICATED OPERATOR NAME (vms-f42d, CLAUDE.md Rule 10). This
     * read ": \"SYSTEM\"", so an unnamed process enabling itself as an
     * operator terminal wrote SYSTEM into OPERATOR.LOG. The unnamed row
     * is reachable without privilege (any SPAWNed subprocess -- see
     * vms_proc_register() in src/kernel/vms_module.c). Deleted, not
     * replaced; long form at lex_user() in src/vmsdcl/dcl_lexical.c.
     */
    const char *username = ctx->username;

    /* Build a minimal OPC message for the log */
    struct {
        struct opcdef hdr;
        char text[128];
    } msgbuf;
    memset(&msgbuf, 0, sizeof(msgbuf));

    struct dsc$descriptor_s desc;
    desc.dsc$a_pointer = (char *)&msgbuf.hdr;
    desc.dsc$w_length  = 0;  /* filled below */

    if (dcl_has_qualifier(cmd, "ENABLE")) {
        const char *cls = dcl_qualifier_value(cmd, "ENABLE");
        char detail[64] = "CENTRAL";
        if (cls && cls[0]) {
            strncpy(detail, cls, sizeof(detail) - 1);
        }
        printf("%%OPCOM-I-OPRENA, operator %s enabled for %s class messages\n",
               username, detail);

        msgbuf.hdr.opc$b_ms_type   = OPC$_RQ_ENABLE;
        msgbuf.hdr.opc$b_ms_target = OPC$M_NM_CENTRL;
        int n = snprintf(msgbuf.hdr.opc$l_ms_text,
                         sizeof(msgbuf.text),
                         "operator %s enabled (%s)", username, detail);
        desc.dsc$w_length = (uint16_t)(OPC$K_MS_HDRLEN + n);
        sys$sndopr(&desc, 0);

    } else if (dcl_has_qualifier(cmd, "DISABLE")) {
        printf("%%OPCOM-I-OPRDIS, operator %s disabled\n", username);

        msgbuf.hdr.opc$b_ms_type   = OPC$_RQ_DISABLE;
        msgbuf.hdr.opc$b_ms_target = OPC$M_NM_CENTRL;
        int n = snprintf(msgbuf.hdr.opc$l_ms_text,
                         sizeof(msgbuf.text),
                         "operator %s disabled", username);
        desc.dsc$w_length = (uint16_t)(OPC$K_MS_HDRLEN + n);
        sys$sndopr(&desc, 0);

    } else if (dcl_has_qualifier(cmd, "TO")) {
        /* REPLY /TO=rqid "reply text" */
        const char *to_val = dcl_qualifier_value(cmd, "TO");
        const char *reply_text = (cmd->param_count >= 1) ? cmd->params[0] : "";

        printf("%%OPCOM-I-REPLY, reply sent to request %s: %s\n",
               to_val ? to_val : "?", reply_text);

        msgbuf.hdr.opc$b_ms_type   = OPC$_RQ_REPLY;
        msgbuf.hdr.opc$b_ms_target = OPC$M_NM_CENTRL;
        int n = snprintf(msgbuf.hdr.opc$l_ms_text,
                         sizeof(msgbuf.text),
                         "reply to rqid %s: %s",
                         to_val ? to_val : "0", reply_text);
        desc.dsc$w_length = (uint16_t)(OPC$K_MS_HDRLEN + n);
        sys$sndopr(&desc, 0);

    } else {
        dcl_error("DCL", 2, "SYNTAX",
                  "REPLY requires /ENABLE, /DISABLE, or /TO qualifier");
        return SS$_BADPARAM;
    }

    return SS$_NORMAL;
}

/*
 * REQUEST "message" - Send a request message to the operator.
 *
 * Usage:
 *   REQUEST "message text"
 *   REQUEST /REPLY "message text"   - wait for operator reply (not implemented)
 *
 * Sends a message to OPCOM (logs to OPERATOR.LOG).
 * Prints a confirmation showing the request was sent.
 */
int cmd_request(struct dcl_command *cmd)
{
    const char *msg_text = "";
    if (cmd->param_count >= 1 && cmd->params[0][0] != '\0') {
        msg_text = cmd->params[0];
    } else {
        dcl_error("DCL", 2, "SYNTAX",
                  "REQUEST requires a message string parameter");
        return SS$_BADPARAM;
    }

    /* Build OPC message buffer */
    struct {
        struct opcdef hdr;
        char text[128];
    } msgbuf;
    memset(&msgbuf, 0, sizeof(msgbuf));

    msgbuf.hdr.opc$b_ms_type   = OPC$_RQ_RQST;
    msgbuf.hdr.opc$b_ms_target = OPC$M_NM_CENTRL;

    int n = snprintf(msgbuf.hdr.opc$l_ms_text, sizeof(msgbuf.text),
                     "%s", msg_text);
    if (n > 127) n = 127;

    struct dsc$descriptor_s desc;
    desc.dsc$a_pointer = (char *)&msgbuf.hdr;
    desc.dsc$w_length  = (uint16_t)(OPC$K_MS_HDRLEN + n);

    uint32_t status = sys$sndopr(&desc, 0);
    if (!(status & 1)) {
        dcl_error("OPCOM", 2, "SNDOPR", "failed to send operator message");
        return (int)status;
    }

    printf("%%OPCOM-I-RQSTPEND, request sent to operator: %s\n", msg_text);
    return SS$_NORMAL;
}

/* ================================================================== */
/*                     ACCOUNTING Command                               */
/* ================================================================== */

/*
 * ACCOUNTING - Display login accounting information for the current user.
 *
 * Shows last login time read from /etc/ovmx/lastlogin/<USERNAME>.
 * Matches OpenVMS ACCOUNTING utility output style.
 */
int cmd_accounting(struct dcl_command *cmd)
{
    (void)cmd;

    struct dcl_context *ctx = dcl_get_context();
    /* No fabricated account name -- this one also picked the FILE whose
     * login record is reported (ovmx_accounting_get_lastlogin below), so
     * the fallback showed an unnamed process SYSTEM's login history.
     * Same deletion as REPLY above (vms-f42d). */
    const char *username = ctx->username;

    static const char *months[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };

    time_t last_login = 0;
    int found = (ovmx_accounting_get_lastlogin(username, &last_login) == 0);

    printf("\n  OVMX Accounting for user %s\n", username);
    printf("  %s\n\n", "----------------------------------------");

    if (found && last_login > 0) {
        struct tm tm;
        localtime_r(&last_login, &tm);
        printf("  Last interactive login: %02d-%s-%04d %02d:%02d:%02d\n",
               tm.tm_mday, months[tm.tm_mon], 1900 + tm.tm_year,
               tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        printf("  Last interactive login: (no previous login recorded)\n");
    }

    printf("\n");
    return SS$_NORMAL;
}

/* ================================================================== */
/*                         HELP Command                                */
/* ================================================================== */

int cmd_help(struct dcl_command *cmd)
{
    /* If a specific topic is requested */
    if (cmd->param_count >= 1 && cmd->params[0][0] != '\0') {
        /* Look up the command for its help text */
        const struct dcl_verb *verb = dcl_find_verb(cmd->params[0]);
        if (verb && verb->help) {
            printf("\n%s\n\n", verb->name);
            printf("  %s\n\n", verb->help);

            /* Provide some standard help structure */
            if (strcasecmp(verb->name, "SHOW") == 0) {
                printf("  Subcommands:\n\n");
                printf("    DEFAULT    Display current default directory\n");
                printf("    LOGICAL    Display logical name translations\n");
                printf("    PROCESS    Display current process information\n");
                printf("    PROTECTION Display process default protection\n");
                printf("    SYMBOL     Display symbol value(s)\n");
                printf("    SYSTEM     Display process list\n");
                printf("    TIME       Display current date/time\n");
                printf("    USERS      Display logged-in users\n");
                printf("    VERIFY     Display verification state\n");
                printf("\n");
            } else if (strcasecmp(verb->name, "SET") == 0) {
                printf("  Subcommands:\n\n");
                printf("    DEFAULT    Change default directory\n");
                printf("    PASSWORD   Change user password\n");
                printf("    PROMPT     Change DCL prompt\n");
                printf("    PROTECTION Set file protection\n");
                printf("    TERMINAL   Set terminal characteristics (stub)\n");
                printf("    [NO]VERIFY Set command verification on/off\n");
                printf("\n");
            } else if (strcasecmp(verb->name, "DIRECTORY") == 0) {
                printf("  Format:\n\n");
                printf("    DIRECTORY [filespec]\n\n");
                printf("  Qualifiers:\n\n");
                printf("    /BRIEF      Display filenames only\n");
                printf("    /COLUMNS=n  Display in n columns\n");
                printf("    /DATE       Display date/time\n");
                printf("    /FULL       Display all information\n");
                printf("    /SIZE       Display file sizes in blocks\n");
                printf("\n");
            }
            return SS$_NORMAL;
        }
        dcl_error("DCL", 0, "NOHELP",
                  "no help available for \"%s\"", cmd->params[0]);
        return SS$_NORMAL;
    }

    /* General help - list all commands */
    printf("\n");
    printf("Information available:\n\n");

    int count = 0;
    const struct dcl_verb *table = dcl_get_verb_table(&count);
    int col = 0;

    for (int i = 0; i < count; i++) {
        printf("  %-16s", table[i].name);
        col++;
        if (col >= 4) {
            printf("\n");
            col = 0;
        }
    }
    if (col > 0) printf("\n");

    printf("\nTopic? ");
    fflush(stdout);

    char topic[256];
    if (fgets(topic, sizeof(topic), stdin)) {
        size_t tlen = strlen(topic);
        if (tlen > 0 && topic[tlen - 1] == '\n') topic[tlen - 1] = '\0';
        if (topic[0] != '\0') {
            struct dcl_command help_cmd;
            memset(&help_cmd, 0, sizeof(help_cmd));
            strcpy(help_cmd.verb, "HELP");
            strncpy(help_cmd.params[0], topic, sizeof(help_cmd.params[0]) - 1);
            help_cmd.param_count = 1;
            return cmd_help(&help_cmd);
        }
    }

    printf("\n");
    return SS$_NORMAL;
}

/* ================================================================== */
/*                     RECALL Command                                  */
/* ================================================================== */

/*
 * RECALL - Show or re-execute commands from history.
 *
 * RECALL          — show last command
 * RECALL /ALL     — numbered history list
 * RECALL n        — re-execute command number n
 * RECALL string   — find and re-execute most recent match
 */
int cmd_recall(struct dcl_command *cmd)
{
#ifndef HAVE_READLINE
    (void)cmd;
    printf("%%DCL-W-RECALL, command recall requires readline support\n");
    return SS$_NORMAL;
#else
    int show_all = dcl_has_qualifier(cmd, "ALL");

    if (show_all) {
        /* RECALL /ALL — print numbered history */
        HIST_ENTRY **hist = history_list();
        if (!hist || !hist[0]) {
            printf("%%DCL-I-RECALL, no history available\n");
            return SS$_NORMAL;
        }
        int count = 0;
        while (hist[count]) count++;
        for (int i = 0; i < count; i++) {
            /* history_list() is 0-indexed; history_get() uses offset_history */
            printf("%5d  %s\n", history_base + i, hist[i]->line);
        }
        return SS$_NORMAL;
    }

    if (cmd->param_count == 0) {
        /* RECALL with no args — show most recent command */
        HIST_ENTRY *entry = current_history();
        /* Go to the most recent history entry */
        while (next_history() != NULL) { /* advance to end */ }
        entry = previous_history();
        if (!entry) {
            printf("%%DCL-I-RECALL, no history available\n");
            return SS$_NORMAL;
        }
        printf("%s\n", entry->line);
        return SS$_NORMAL;
    }

    /* Parameter given — check if it's a number */
    const char *param = cmd->params[0];
    int is_number = 1;
    for (size_t i = 0; param[i]; i++) {
        if (!isdigit((unsigned char)param[i])) { is_number = 0; break; }
    }

    if (is_number) {
        /* RECALL n — re-execute command number n */
        int n = (int)strtol(param, NULL, 10);
        HIST_ENTRY *entry = history_get(n);
        if (!entry) {
            printf("%%DCL-W-RECALL, no command number %d in history\n", n);
            return SS$_NORMAL;
        }
        printf("%s\n", entry->line);
        /* Feed the command back to the executor */
        return dcl_execute_line(entry->line);
    } else {
        /* RECALL string — find most recent command starting with string */
        HIST_ENTRY **hist = history_list();
        if (!hist) {
            printf("%%DCL-I-RECALL, no history available\n");
            return SS$_NORMAL;
        }
        int count = 0;
        while (hist[count]) count++;
        size_t plen = strlen(param);
        for (int i = count - 1; i >= 0; i--) {
            if (strncasecmp(hist[i]->line, param, plen) == 0) {
                printf("%s\n", hist[i]->line);
                return dcl_execute_line(hist[i]->line);
            }
        }
        printf("%%DCL-W-RECALL, no command matching \"%s\" in history\n", param);
        return SS$_NORMAL;
    }
#endif
}

/* ================================================================== */
/*                     TCPIP Commands                                  */
/* ================================================================== */

/*
 * Map a Linux network interface name to a VMS device name.
 * eth*, ens*, enp* → SE0, SE1, ...
 * lo              → LO0
 * wlan*, wlp*     → EW0, EW1, ...
 * tun*            → TN0, TN1, ...
 * Everything else → XX0, XX1, ...
 */
static const char *tcpip_map_interface(const char *linux_name,
                                        int *se_idx, int *ew_idx,
                                        int *tn_idx, int *xx_idx)
{
    static char vms_name[16];

    if (strcmp(linux_name, "lo") == 0) {
        snprintf(vms_name, sizeof(vms_name), "LO0");
    } else if (strncmp(linux_name, "eth", 3) == 0 ||
               strncmp(linux_name, "ens", 3) == 0 ||
               strncmp(linux_name, "enp", 3) == 0) {
        snprintf(vms_name, sizeof(vms_name), "SE%d", (*se_idx)++);
    } else if (strncmp(linux_name, "wlan", 4) == 0 ||
               strncmp(linux_name, "wlp", 3) == 0) {
        snprintf(vms_name, sizeof(vms_name), "EW%d", (*ew_idx)++);
    } else if (strncmp(linux_name, "tun", 3) == 0) {
        snprintf(vms_name, sizeof(vms_name), "TN%d", (*tn_idx)++);
    } else {
        snprintf(vms_name, sizeof(vms_name), "XX%d", (*xx_idx)++);
    }

    return vms_name;
}

/*
 * Build a mapping table of Linux interface names → VMS device names.
 * Scans /sys/class/net/ to enumerate interfaces.
 */
#define TCPIP_MAX_IFACES 32

struct tcpip_ifmap {
    char linux_name[IFNAMSIZ];
    char vms_name[16];
};

static int tcpip_build_ifmap(struct tcpip_ifmap *map, int max_entries)
{
    DIR *d = opendir("/sys/class/net");
    if (!d) return 0;

    /* First pass: collect interface names */
    char names[TCPIP_MAX_IFACES][IFNAMSIZ];
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < TCPIP_MAX_IFACES) {
        if (ent->d_name[0] == '.') continue;
        strncpy(names[count], ent->d_name, IFNAMSIZ - 1);
        names[count][IFNAMSIZ - 1] = '\0';
        count++;
    }
    closedir(d);

    /* Sort for stable ordering */
    qsort(names, (size_t)count, IFNAMSIZ, (int(*)(const void *, const void *))strcmp);

    /* Map names */
    int se_idx = 0, ew_idx = 0, tn_idx = 0, xx_idx = 0;
    int n = (count < max_entries) ? count : max_entries;
    for (int i = 0; i < n; i++) {
        strncpy(map[i].linux_name, names[i], IFNAMSIZ - 1);
        map[i].linux_name[IFNAMSIZ - 1] = '\0';
        const char *vn = tcpip_map_interface(names[i],
                                              &se_idx, &ew_idx,
                                              &tn_idx, &xx_idx);
        strncpy(map[i].vms_name, vn, sizeof(map[i].vms_name) - 1);
        map[i].vms_name[sizeof(map[i].vms_name) - 1] = '\0';
    }
    return n;
}

/* Look up VMS name for a Linux interface name in the map */
static const char *tcpip_lookup_vms_name(const struct tcpip_ifmap *map,
                                          int count, const char *linux_name)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(map[i].linux_name, linux_name) == 0)
            return map[i].vms_name;
    }
    return linux_name; /* fallback — should not happen */
}

/* Reverse lookup: VMS device name → Linux interface name */
static const char *tcpip_lookup_linux_name(const struct tcpip_ifmap *map,
                                            int count, const char *vms_name)
{
    for (int i = 0; i < count; i++) {
        if (strcasecmp(map[i].vms_name, vms_name) == 0)
            return map[i].linux_name;
    }
    return NULL;
}

/* Path for VMS TCPIP config files */
#define TCPIP_CONFIG_DIR VMS_SYSTEM_DIR
#define TCPIP_HOST_DAT    TCPIP_CONFIG_DIR "/TCPIP$HOST.DAT"
#define TCPIP_NS_DAT      TCPIP_CONFIG_DIR "/TCPIP$NAMESERVICE.DAT"
#define TCPIP_IF_DAT      TCPIP_CONFIG_DIR "/TCPIP$INTERFACE.DAT"
#define TCPIP_ROUTE_DAT   TCPIP_CONFIG_DIR "/TCPIP$ROUTE.DAT"

/*
 * TCPIP SHOW INTERFACE [/FULL] - Display network interfaces with VMS names.
 */
static int cmd_tcpip_show_interface(struct dcl_command *cmd)
{
    struct tcpip_ifmap ifmap[TCPIP_MAX_IFACES];
    int ifcount = tcpip_build_ifmap(ifmap, TCPIP_MAX_IFACES);

    int full = dcl_has_qualifier(cmd, "FULL");

    printf("\n");
    printf("%-12s%-17s%-17s%-7s%s\n",
           "Interface", "IP Address", "Network Mask", "MTU", "State");

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        printf("%%TCPIP-E-SOCKERR, cannot open socket\n");
        return SS$_BADPARAM;
    }

    for (int i = 0; i < ifcount; i++) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, ifmap[i].linux_name, IFNAMSIZ - 1);

        /* IP address */
        char ip_str[INET_ADDRSTRLEN] = "*";
        if (ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
            struct sockaddr_in *addr = (struct sockaddr_in *)&ifr.ifr_addr;
            inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str));
        }

        /* Netmask */
        char mask_str[INET_ADDRSTRLEN] = "*";
        if (ioctl(sock, SIOCGIFNETMASK, &ifr) == 0) {
            struct sockaddr_in *mask = (struct sockaddr_in *)&ifr.ifr_netmask;
            inet_ntop(AF_INET, &mask->sin_addr, mask_str, sizeof(mask_str));
        }

        /* MTU */
        int mtu = 0;
        if (ioctl(sock, SIOCGIFMTU, &ifr) == 0) {
            mtu = ifr.ifr_mtu;
        }

        /* Flags (up/down) */
        const char *state = "Down";
        if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
            if (ifr.ifr_flags & IFF_UP)
                state = "Up";
        }

        printf("%-12s%-17s%-17s%-7d%s\n",
               ifmap[i].vms_name, ip_str, mask_str, mtu, state);

        if (full) {
            /* Show hardware address */
            if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
                unsigned char *hw = (unsigned char *)ifr.ifr_hwaddr.sa_data;
                printf("             Hardware address: %02X-%02X-%02X-%02X-%02X-%02X\n",
                       hw[0], hw[1], hw[2], hw[3], hw[4], hw[5]);
            }
        }
    }

    close(sock);
    printf("\n");
    return SS$_NORMAL;
}

/*
 * TCPIP SHOW ROUTE - Display routing table with VMS device names.
 */
static int cmd_tcpip_show_route(struct dcl_command *cmd)
{
    (void)cmd;

    struct tcpip_ifmap ifmap[TCPIP_MAX_IFACES];
    int ifcount = tcpip_build_ifmap(ifmap, TCPIP_MAX_IFACES);

    FILE *fp = fopen("/proc/net/route", "r");
    if (!fp) {
        printf("%%TCPIP-E-NOROUTE, cannot read routing table\n");
        return SS$_BADPARAM;
    }

    printf("\n");
    printf("%-17s%-17s%-17s%s\n",
           "Destination", "Gateway", "Mask", "Interface");

    char line[256];
    /* Skip header */
    if (fgets(line, sizeof(line), fp) == NULL) {
        fclose(fp);
        return SS$_NORMAL;
    }

    while (fgets(line, sizeof(line), fp)) {
        char iface[IFNAMSIZ];
        unsigned long dest, gw, mask;
        unsigned int flags;
        int refs, use, metric, mtu, window, irtt;

        if (sscanf(line, "%s %lx %lx %x %d %d %d %lx %d %d %d",
                   iface, &dest, &gw, &flags, &refs, &use, &metric,
                   &mask, &mtu, &window, &irtt) < 8)
            continue;

        /* Skip non-UP routes */
        if (!(flags & 0x0001)) continue;

        /* Destination */
        char dest_str[32];
        if (dest == 0) {
            strcpy(dest_str, "default");
        } else {
            struct in_addr a;
            a.s_addr = (in_addr_t)dest;
            inet_ntop(AF_INET, &a, dest_str, sizeof(dest_str));
        }

        /* Gateway */
        char gw_str[32];
        if (gw == 0) {
            strcpy(gw_str, "*");
        } else {
            struct in_addr a;
            a.s_addr = (in_addr_t)gw;
            inet_ntop(AF_INET, &a, gw_str, sizeof(gw_str));
        }

        /* Mask */
        char mask_str[32];
        {
            struct in_addr a;
            a.s_addr = (in_addr_t)mask;
            inet_ntop(AF_INET, &a, mask_str, sizeof(mask_str));
        }

        /* VMS interface name */
        const char *vms_iface = tcpip_lookup_vms_name(ifmap, ifcount, iface);

        printf("%-17s%-17s%-17s%s\n", dest_str, gw_str, mask_str, vms_iface);
    }

    fclose(fp);
    printf("\n");
    return SS$_NORMAL;
}

/*
 * TCPIP SHOW HOST - Display host table entries.
 */
/* Track shown host entries to avoid duplicates across files */
#define TCPIP_MAX_HOST_ENTRIES 256

struct tcpip_host_entry {
    char addr[128];
    char name[256];
};

static int tcpip_host_already_shown(const struct tcpip_host_entry *shown,
                                     int count,
                                     const char *addr, const char *name)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(shown[i].addr, addr) == 0 &&
            strcasecmp(shown[i].name, name) == 0)
            return 1;
    }
    return 0;
}

static int tcpip_print_hosts_from_file(const char *path,
                                        struct tcpip_host_entry *shown,
                                        int count)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return count;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        char addr[128], hostname[256];
        if (sscanf(p, "%127s %255s", addr, hostname) >= 2) {
            if (count < TCPIP_MAX_HOST_ENTRIES &&
                !tcpip_host_already_shown(shown, count, addr, hostname)) {
                printf("%-16s%s\n", addr, hostname);
                strncpy(shown[count].addr, addr, sizeof(shown[count].addr) - 1);
                strncpy(shown[count].name, hostname, sizeof(shown[count].name) - 1);
                count++;
            }
        }
    }
    fclose(fp);
    return count;
}

static int cmd_tcpip_show_host(struct dcl_command *cmd)
{
    (void)cmd;
    printf("\n");
    printf("%-16s%s\n", "Host address", "Host name");

    struct tcpip_host_entry *shown = calloc(TCPIP_MAX_HOST_ENTRIES,
                                             sizeof(struct tcpip_host_entry));
    int count = 0;
    if (shown) {
        count = tcpip_print_hosts_from_file("/etc/hosts", shown, count);
        tcpip_print_hosts_from_file(TCPIP_HOST_DAT, shown, count);
        free(shown);
    }

    printf("\n");
    return SS$_NORMAL;
}

/*
 * TCPIP SHOW VERSION - Display TCP/IP Services version.
 */
static int cmd_tcpip_show_version(struct dcl_command *cmd)
{
    (void)cmd;
    printf("OVMX TCP/IP Services %s\n", ovmx_product_version());
    return SS$_NORMAL;
}

/*
 * Ensure the TCPIP config directory exists.
 */
static void tcpip_ensure_config_dir(void)
{
    mkdir(TCPIP_CONFIG_DIR, 0755);
}

/*
 * TCPIP SET HOST hostname /ADDRESS=ip
 * Adds an entry to TCPIP$HOST.DAT and /etc/hosts.
 */
static int cmd_tcpip_set_host(struct dcl_command *cmd)
{
    if (cmd->param_count < 3) {
        dcl_error("TCPIP", 2, "NOKEYW",
                  "missing hostname - usage: TCPIP SET HOST name /ADDRESS=ip");
        return SS$_BADPARAM;
    }

    const char *hostname = cmd->params[2];
    const char *address = dcl_qualifier_value(cmd, "ADDRESS");

    if (!address) {
        dcl_error("TCPIP", 2, "NOKEYW",
                  "missing /ADDRESS qualifier");
        return SS$_BADPARAM;
    }

    /* Validate IP address */
    struct in_addr test_addr;
    if (inet_pton(AF_INET, address, &test_addr) != 1) {
        dcl_error("TCPIP", 2, "BADPARAM",
                  "invalid IP address - \\%s\\", address);
        return SS$_BADPARAM;
    }

    tcpip_ensure_config_dir();

    /* Write to TCPIP$HOST.DAT */
    FILE *fp = fopen(TCPIP_HOST_DAT, "a");
    if (fp) {
        fprintf(fp, "%-16s%s\n", address, hostname);
        fclose(fp);
    }

    /* Also append to /etc/hosts for Linux DNS resolution */
    fp = fopen("/etc/hosts", "a");
    if (fp) {
        fprintf(fp, "%-16s%s\n", address, hostname);
        fclose(fp);
    }

    printf("%%TCPIP-I-INFO, host \"%s\" added\n", hostname);
    return SS$_NORMAL;
}

/*
 * TCPIP SET NAME_SERVICE /SYSTEM /SERVER=ip [/DOMAIN=domain]
 * Configures DNS resolver.
 */
static int cmd_tcpip_set_name_service(struct dcl_command *cmd)
{
    const char *server = dcl_qualifier_value(cmd, "SERVER");

    if (!server) {
        dcl_error("TCPIP", 2, "NOKEYW",
                  "missing /SERVER qualifier");
        return SS$_BADPARAM;
    }

    /* Validate IP address */
    struct in_addr test_addr;
    if (inet_pton(AF_INET, server, &test_addr) != 1) {
        dcl_error("TCPIP", 2, "BADPARAM",
                  "invalid server IP address - \\%s\\", server);
        return SS$_BADPARAM;
    }

    const char *domain = dcl_qualifier_value(cmd, "DOMAIN");

    tcpip_ensure_config_dir();

    /* Write to TCPIP$NAMESERVICE.DAT */
    FILE *fp = fopen(TCPIP_NS_DAT, "w");
    if (fp) {
        fprintf(fp, "SERVER=%s\n", server);
        if (domain)
            fprintf(fp, "DOMAIN=%s\n", domain);
        fclose(fp);
    }

    /* Also write /etc/resolv.conf */
    fp = fopen("/etc/resolv.conf", "w");
    if (fp) {
        if (domain)
            fprintf(fp, "domain %s\n", domain);
        fprintf(fp, "nameserver %s\n", server);
        fclose(fp);
    }

    printf("%%TCPIP-I-INFO, name service configured\n");
    return SS$_NORMAL;
}

/*
 * TCPIP SET INTERFACE ifname /HOST=ip /NETWORK_MASK=mask
 * Configure a network interface (requires root/NET_ADMIN).
 */
static int cmd_tcpip_set_interface(struct dcl_command *cmd)
{
    if (cmd->param_count < 3) {
        dcl_error("TCPIP", 2, "NOKEYW",
                  "missing interface name - usage: TCPIP SET INTERFACE name /HOST=ip /NETWORK_MASK=mask");
        return SS$_BADPARAM;
    }

    const char *ifname = cmd->params[2];
    const char *host_ip = dcl_qualifier_value(cmd, "HOST");
    const char *netmask = dcl_qualifier_value(cmd, "NETWORK_MASK");

    if (!host_ip) {
        dcl_error("TCPIP", 2, "NOKEYW",
                  "missing /HOST qualifier");
        return SS$_BADPARAM;
    }

    /* Validate IP address */
    struct in_addr test_addr;
    if (inet_pton(AF_INET, host_ip, &test_addr) != 1) {
        dcl_error("TCPIP", 2, "BADPARAM",
                  "invalid IP address - \\%s\\", host_ip);
        return SS$_BADPARAM;
    }

    /* Map VMS device name to Linux interface */
    struct tcpip_ifmap ifmap[TCPIP_MAX_IFACES];
    int ifcount = tcpip_build_ifmap(ifmap, TCPIP_MAX_IFACES);
    const char *linux_if = tcpip_lookup_linux_name(ifmap, ifcount, ifname);

    if (!linux_if) {
        dcl_error("TCPIP", 2, "NOSUCHDEV",
                  "unknown interface - \\%s\\", ifname);
        return SS$_BADPARAM;
    }

    /* Check for root/NET_ADMIN privilege */
    if (geteuid() != 0) {
        printf("%%TCPIP-W-PRIVREQ, operation requires NET_ADMIN privilege\n");
        /* Still persist to config file */
    } else {
        /* Apply with ioctl */
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, linux_if, IFNAMSIZ - 1);

            /* Set IP address */
            struct sockaddr_in *addr = (struct sockaddr_in *)&ifr.ifr_addr;
            addr->sin_family = AF_INET;
            inet_pton(AF_INET, host_ip, &addr->sin_addr);
            if (ioctl(sock, SIOCSIFADDR, &ifr) < 0) {
                printf("%%TCPIP-W-IOERR, failed to set address: %s\n",
                       strerror(errno));
            }

            /* Set netmask if provided */
            if (netmask) {
                struct sockaddr_in *mask = (struct sockaddr_in *)&ifr.ifr_netmask;
                mask->sin_family = AF_INET;
                inet_pton(AF_INET, netmask, &mask->sin_addr);
                if (ioctl(sock, SIOCSIFNETMASK, &ifr) < 0) {
                    printf("%%TCPIP-W-IOERR, failed to set netmask: %s\n",
                           strerror(errno));
                }
            }

            close(sock);
        }
    }

    /* Persist to TCPIP$INTERFACE.DAT */
    tcpip_ensure_config_dir();
    FILE *fp = fopen(TCPIP_IF_DAT, "a");
    if (fp) {
        fprintf(fp, "%s %s", ifname, host_ip);
        if (netmask)
            fprintf(fp, " %s", netmask);
        fprintf(fp, "\n");
        fclose(fp);
    }

    printf("%%TCPIP-I-INFO, interface configured\n");
    return SS$_NORMAL;
}

/*
 * TCPIP SET ROUTE /GATEWAY=ip /DEFAULT
 *   or  /DESTINATION=dest /GATEWAY=gw /NETWORK_MASK=mask
 * Configure a network route (requires root/NET_ADMIN).
 */
static int cmd_tcpip_set_route(struct dcl_command *cmd)
{
    const char *gateway = dcl_qualifier_value(cmd, "GATEWAY");

    if (!gateway) {
        dcl_error("TCPIP", 2, "NOKEYW",
                  "missing /GATEWAY qualifier");
        return SS$_BADPARAM;
    }

    /* Validate gateway IP */
    struct in_addr test_addr;
    if (inet_pton(AF_INET, gateway, &test_addr) != 1) {
        dcl_error("TCPIP", 2, "BADPARAM",
                  "invalid gateway IP address - \\%s\\", gateway);
        return SS$_BADPARAM;
    }

    int is_default = dcl_has_qualifier(cmd, "DEFAULT");
    const char *destination = dcl_qualifier_value(cmd, "DESTINATION");
    const char *netmask = dcl_qualifier_value(cmd, "NETWORK_MASK");

    if (!is_default && !destination) {
        dcl_error("TCPIP", 2, "NOKEYW",
                  "specify /DEFAULT or /DESTINATION");
        return SS$_BADPARAM;
    }

    /* Check for root/NET_ADMIN privilege */
    if (geteuid() != 0) {
        printf("%%TCPIP-W-PRIVREQ, operation requires NET_ADMIN privilege\n");
        /* Still persist to config file */
    } else {
        /* Use ip route add command */
        char route_cmd[512];
        if (is_default) {
            snprintf(route_cmd, sizeof(route_cmd),
                     "ip route replace default via %s 2>/dev/null", gateway);
        } else {
            if (netmask) {
                /* Convert dotted netmask to CIDR prefix length */
                struct in_addr mask_addr;
                inet_pton(AF_INET, netmask, &mask_addr);
                uint32_t mask_val = ntohl(mask_addr.s_addr);
                int prefix = 0;
                while (mask_val & 0x80000000) {
                    prefix++;
                    mask_val <<= 1;
                }
                snprintf(route_cmd, sizeof(route_cmd),
                         "ip route replace %s/%d via %s 2>/dev/null",
                         destination, prefix, gateway);
            } else {
                snprintf(route_cmd, sizeof(route_cmd),
                         "ip route replace %s via %s 2>/dev/null",
                         destination, gateway);
            }
        }
        (void)system(route_cmd);
    }

    /* Persist to TCPIP$ROUTE.DAT */
    tcpip_ensure_config_dir();
    FILE *fp = fopen(TCPIP_ROUTE_DAT, "a");
    if (fp) {
        if (is_default) {
            fprintf(fp, "DEFAULT %s\n", gateway);
        } else {
            fprintf(fp, "%s %s", destination, gateway);
            if (netmask)
                fprintf(fp, " %s", netmask);
            fprintf(fp, "\n");
        }
        fclose(fp);
    }

    printf("%%TCPIP-I-INFO, route added\n");
    return SS$_NORMAL;
}

/*
 * TCPIP - TCP/IP Services command with SHOW and SET subcommands.
 */
int cmd_tcpip(struct dcl_command *cmd)
{
    if (cmd->param_count < 1) {
        dcl_error("TCPIP", 2, "NOKEYW", "missing keyword - supply a TCPIP subcommand");
        return SS$_BADPARAM;
    }

    const char *subcmd = cmd->params[0];

    if (dcl_match_command(subcmd, "SHOW", 2)) {
        /* TCPIP SHOW <what> */
        if (cmd->param_count < 2) {
            dcl_error("TCPIP", 2, "NOKEYW",
                      "missing SHOW keyword - supply what you want to show");
            return SS$_BADPARAM;
        }

        const char *what = cmd->params[1];

        if (dcl_match_command(what, "INTERFACE", 3))
            return cmd_tcpip_show_interface(cmd);
        if (dcl_match_command(what, "ROUTE", 3))
            return cmd_tcpip_show_route(cmd);
        if (dcl_match_command(what, "HOST", 3))
            return cmd_tcpip_show_host(cmd);
        if (dcl_match_command(what, "VERSION", 3))
            return cmd_tcpip_show_version(cmd);

        dcl_error("TCPIP", 2, "IVKEYW",
                  "unrecognized TCPIP SHOW keyword - \\%s\\", what);
        return SS$_IVKEYW;
    }

    if (dcl_match_command(subcmd, "SET", 2)) {
        /* TCPIP SET <what> */
        if (cmd->param_count < 2) {
            dcl_error("TCPIP", 2, "NOKEYW",
                      "missing SET keyword - supply what you want to set");
            return SS$_BADPARAM;
        }

        const char *what = cmd->params[1];

        if (dcl_match_command(what, "HOST", 3))
            return cmd_tcpip_set_host(cmd);
        if (dcl_match_command(what, "NAME_SERVICE", 4))
            return cmd_tcpip_set_name_service(cmd);
        if (dcl_match_command(what, "INTERFACE", 3))
            return cmd_tcpip_set_interface(cmd);
        if (dcl_match_command(what, "ROUTE", 3))
            return cmd_tcpip_set_route(cmd);

        dcl_error("TCPIP", 2, "IVKEYW",
                  "unrecognized TCPIP SET keyword - \\%s\\", what);
        return SS$_IVKEYW;
    }

    dcl_error("TCPIP", 2, "IVKEYW",
              "unrecognized TCPIP keyword - \\%s\\", subcmd);
    return SS$_IVKEYW;
}

/* ================================================================== */
/*                    MOUNT / DISMOUNT Commands                        */
/* ================================================================== */

/*
 * Compute the mount point a device name uses. A PURE FUNCTION of the name --
 * any process can compute it without asking anyone, exactly like
 * SYSDISK_MOUNT is a compile-time constant for DKA0: (ovmx_layout.h). This is
 * what lets "is this unit mounted" be answered from /proc/mounts (real,
 * global, kernel-reported truth) instead of a per-process table entry.
 * "/mnt/<devnam>" matches the vmsfs mount-point convention the QEMU kernel
 * tests already use (tests/qemu/test_kmod_vmsfs*.c).
 */
void mount_point_for_device(const char *log_name, char *buf, size_t sz)
{
    char lower[16];
    size_t i;
    for (i = 0; log_name[i] && i < sizeof(lower) - 1; i++)
        lower[i] = (char)tolower((unsigned char)log_name[i]);
    lower[i] = '\0';
    snprintf(buf, sz, "/mnt/%s", lower);
}

/*
 * Is `mount_point` present in the kernel's own mount table right now?
 * Cross-process, kernel-reported truth -- not a field only this process
 * could see.
 *
 * Parsed by hand with fopen/fgets rather than glibc's getmntent(3):
 * DECC$SHR's symbol vector (src/vmslink/mk_decc_shr.sh) exports the plain
 * stdio family DCL already links against everywhere, but not
 * setmntent/getmntent/endmntent -- those are a glibc-only convenience this
 * tree has never needed before, and pulling them in here broke the
 * VMS-native LINK.EXE build of DCL.EXE (%LINK-F-ERROR, unresolved external
 * symbol 'setmntent'; measured building distro/Dockerfile.bootable).
 */
int mount_point_is_mounted(const char *mount_point)
{
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp)
        return 0;

    /* /proc/mounts format: "<source> <mount_point> <fstype> <opts> <freq>
     * <passno>\n", space-separated. Compare the SECOND field's exact
     * extent, not a substring -- "/mnt/dka1" must not match
     * "/mnt/dka100". */
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

/*
 * Fork, continue THIS session's VMS identity onto the child, and exec the
 * setuid-root mount helper (tools/vms_mount_helper.c, vms-651) -- the one
 * thing on the node still able to mount(2)/umount(2) once LOGINOUT has
 * setuid()/setgid()'d the session onto its SYSUAF UIC. See that file's
 * header comment for the full reasoning (including why a kernel-mediated
 * mount does not work on this platform) and for why the helper re-derives
 * its own authorization rather than trusting argv.
 *
 * vms_kif_register_continue() is called in the CHILD, before execv() --
 * the SAME "forked child before doing privileged work inherits the
 * parent's authenticated identity" shape image activation already uses
 * (its own doc comment in src/libvmssys/vms_kif.h says so explicitly) --
 * so the helper's PRV$M_MOUNT check sees THIS session, not a fresh,
 * unauthenticated registration: pid/tgid survive exec(), uid/gid/
 * capabilities do not, and vms.ko's registration is keyed on the former.
 *
 * Returns the helper's exit code (0 success, 1 mount/umount itself failed,
 * 2 usage error, 3 PRV$M_MOUNT not held), or -1 if the helper could not be
 * forked/exec'd/waited at all. On exit code 1, *helper_errno receives the
 * errno the helper's mount(2)/umount(2) call failed with.
 */
static int run_mount_helper(char *const argv[], int *helper_errno)
{
    if (helper_errno)
        *helper_errno = 0;

    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        (void)vms_kif_open();
        (void)vms_kif_register_continue();
        execv(VMS_MOUNT_HELPER_PATH, argv);
        _exit(127);
    }

    close(pipefd[1]);
    char errbuf[256];
    ssize_t n = read(pipefd[0], errbuf, sizeof(errbuf) - 1);
    close(pipefd[0]);
    errbuf[n > 0 ? n : 0] = '\0';

    int wstatus;
    if (waitpid(pid, &wstatus, 0) < 0 || !WIFEXITED(wstatus))
        return -1;

    int rc = WEXITSTATUS(wstatus);
    if (rc == 1 && helper_errno) {
        int e = 0;
        if (sscanf(errbuf, "ERRNO %d", &e) == 1)
            *helper_errno = e;
    }
    return rc;
}

/*
 * Read the volume label off `backing`'s vmsfs home block through the setuid-
 * root helper's `readlabel` mode (vms-f83) -- the session dropped its Linux
 * capability at LOGINOUT and cannot read the raw block device itself, exactly
 * the reason mount(2) is delegated to the helper. Captures the helper's stdout
 * ("LABEL <name>") the same fork/continue-identity/exec shape run_mount_helper
 * uses, but reads STDOUT rather than STDERR. On success copies the label to
 * `label` and returns 1; returns 0 if the label could not be read (the helper
 * failed, was not authorized, or the device is not a vmsfs volume) -- in which
 * case MOUNT defines no DISK$ logical rather than fabricating one (INV-DCL).
 */
static int mount_helper_readlabel(const char *backing, char *label,
                                  size_t label_size)
{
    if (!backing || !label || label_size == 0)
        return 0;
    label[0] = '\0';

    int pipefd[2];
    if (pipe(pipefd) != 0)
        return 0;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return 0;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        (void)vms_kif_open();
        (void)vms_kif_register_continue();
        char *argv[] = {
            (char *)VMS_MOUNT_HELPER_PATH, (char *)"readlabel",
            (char *)backing, NULL
        };
        execv(VMS_MOUNT_HELPER_PATH, argv);
        _exit(127);
    }

    close(pipefd[1]);
    char outbuf[64];
    ssize_t n = read(pipefd[0], outbuf, sizeof(outbuf) - 1);
    close(pipefd[0]);
    outbuf[n > 0 ? n : 0] = '\0';

    int wstatus;
    if (waitpid(pid, &wstatus, 0) < 0 || !WIFEXITED(wstatus) ||
        WEXITSTATUS(wstatus) != 0)
        return 0;

    /* "LABEL <name>\n" -- take the token after the first space, stop at
     * whitespace. */
    char parsed[32];
    if (sscanf(outbuf, "LABEL %31s", parsed) != 1)
        return 0;
    strncpy(label, parsed, label_size - 1);
    label[label_size - 1] = '\0';
    return 1;
}

/*
 * MOUNT - mount(2) a VMS disk unit's backing block device as vmsfs (vms-651).
 *
 * Syntax: MOUNT device: label [/SYSTEM]
 *
 * KILLED (vms-651, docs/design-vms-faithful-install.md sec 3.1/3.3): the
 * facade this replaced never called mount(2). It wrote a per-process
 * userspace device table (struct vms_device / vms_device_table[] in
 * src/vmsdcl/dcl_builtin.c -- deleted with this change, its only two
 * readers), used getcwd() as the "mount path", and printed
 * %MOUNT-I-MOUNTED unconditionally: success reported, nothing shared with
 * any other process or the disk -- the Rule 9 defect class exactly.
 *
 * WHAT THIS DOES NOW:
 *   - PRV$M_MOUNT is checked through the executive (vms_kif_chkpriv, which
 *     asks vms_ioctl_chkpriv to read proc->cur_privs -- kernel-resident
 *     state, not a userspace getuid() check nothing here could forge).
 *   - The unit is resolved to its backing Linux block device through the
 *     executive (vms_kif_disk_resolve, vms-3e8) -- this process never
 *     scans /sys/block itself (Rule 11).
 *   - "Already mounted" is answered by the kernel's OWN mount table
 *     (/proc/mounts), not a struct field only this process could see.
 *   - The unit is claimed in the executive's device table (vms_kif_alloc,
 *     wired here for the first time) so a second process cannot mount the
 *     same unit out from under this one.
 *   - mount(2) attaches "/dev/<backing>" as vmsfs at the unit's mount
 *     point, and the per-process VMS-filespec translator (vmsfs_device_add,
 *     src/vmsfs/vmsfs_device.c -- the same mechanism DKA0:/SYSDISK_MOUNT
 *     already uses) learns the mapping so CREATE/OPEN against this device
 *     resolve for the rest of this session.
 *   - A mount(2) failure has no oracle-pinned VMS status (VMS has no
 *     Linux mount(2) underneath it), so it is reported as an honest OVMX
 *     facility (Rule 10), the same shape ovmx_sysinit_halt() uses in
 *     src/ovmx_init/ovmx_init.c for the system disk's own mount failure.
 */
int cmd_mount(struct dcl_command *cmd)
{
    if (cmd->param_count < 1) {
        dcl_error("MOUNT", 2, "NODEVICE",
                  "no device specified");
        return SS$_BADPARAM;
    }

    const char *device = cmd->params[0];

    /* Validate device name */
    size_t dlen = strlen(device);
    if (dlen < 2) {
        dcl_error("MOUNT", 2, "IVDEVNAM",
                  "invalid device name - \\%s\\", device);
        return SS$_IVDEVNAM;
    }

    /* Build canonical device name (uppercase, with colon) */
    char dev_name[16];
    size_t nlen = dlen;
    if (nlen >= sizeof(dev_name) - 1) nlen = sizeof(dev_name) - 2;
    for (size_t i = 0; i < nlen; i++)
        dev_name[i] = (char)toupper((unsigned char)device[i]);
    dev_name[nlen] = '\0';
    if (dev_name[nlen - 1] != ':') {
        dev_name[nlen] = ':';
        dev_name[nlen + 1] = '\0';
    }

    /* Strip trailing colon for the logical name / filespec-translator key */
    char log_name[16];
    strncpy(log_name, dev_name, sizeof(log_name) - 1);
    log_name[sizeof(log_name) - 1] = '\0';
    size_t lnlen = strlen(log_name);
    if (lnlen > 0 && log_name[lnlen - 1] == ':')
        log_name[lnlen - 1] = '\0';

    char mount_point[64];
    mount_point_for_device(log_name, mount_point, sizeof(mount_point));

    (void)vms_kif_open();

    /* PRIVILEGE (vms-651 constraint): ask the executive, not getuid(). */
    uint32_t pst = vms_kif_chkpriv(PRV$M_MOUNT);
    if (pst == SS$_NOPRIV) {
        dcl_error("SYSTEM", 4, "NOPRIV",
                  "insufficient privilege or object protection violation");
        return SS$_NOPRIV;
    }
    if (!(pst & 1)) {
        /* Executive-unreachable / ioctl-level failure -- $STATUS carries
         * it, nothing rendered (Rule 10; see cmd_show_device's identical
         * default: case in dcl_cmd_show.c for the reasoning). */
        return pst;
    }

    /* Resolve the unit to its backing block device through the executive
     * (vms_kif_disk_resolve, vms-3e8) -- the process never scans /sys/block
     * itself (Rule 11). */
    char backing[VMS_BACKING_SIZE];
    memset(backing, 0, sizeof(backing));
    uint32_t rst = vms_kif_disk_resolve(dev_name, backing, sizeof(backing),
                                         NULL, NULL);
    switch (rst) {
    case SS$_NORMAL:
        break;
    case SS$_NOSUCHDEV:
        dcl_error("SYSTEM", 0, "NOSUCHDEV", "no such device available");
        return SS$_NOSUCHDEV;
    case SS$_IVDEVNAM:
        dcl_error("SYSTEM", 0, "IVDEVNAM", "invalid device name");
        return SS$_IVDEVNAM;
    default:
        return rst;
    }

    if (mount_point_is_mounted(mount_point)) {
        dcl_error("MOUNT", 2, "DEVMOUNT",
                  "device already mounted - _%s", dev_name);
        return SS$_DEVMOUNT;
    }

    /* Claim the unit in the executive's device table (vms-651 wires
     * vms_kif_alloc for the first time). */
    uint32_t ast = vms_kif_alloc(dev_name);
    if (ast == SS$_DEVALLOC) {
        dcl_error("SYSTEM", 0, "DEVALLOC",
                  "device already allocated to another user");
        return SS$_DEVALLOC;
    }
    if (!(ast & 1))
        return ast;

    /* Volume label -- informational only; vmsfs does not read it back.
     * Kept for command-line compatibility. */
    char label[16] = "OVMX";
    if (cmd->param_count >= 2) {
        size_t llen = strlen(cmd->params[1]);
        if (llen >= sizeof(label)) llen = sizeof(label) - 1;
        for (size_t k = 0; k < llen; k++)
            label[k] = (char)toupper((unsigned char)cmd->params[1][k]);
        label[llen] = '\0';
    }

    /*
     * The actual mount(2), performed by the setuid-root helper
     * (tools/vms_mount_helper.c, vms-651): LOGINOUT setuid()/setgid()'s
     * every VMS session onto its SYSUAF UIC (tools/vms_login.c), so THIS
     * process holds no Linux capability regardless of the PRV$M_MOUNT it
     * was just found to hold -- mount(2) itself requires CAP_SYS_ADMIN
     * unconditionally. The helper re-derives PRV$M_MOUNT from the
     * executive itself (it does not trust this process's claim) before
     * doing anything privileged -- see its header comment for why a
     * kernel-mediated mount is not available on this platform and for the
     * full security reasoning. The mount point already exists --
     * src/ovmx_init/ovmx_init.c's provision_disk_mount_points() created it,
     * as root, before any session dropped privilege.
     */
    char backing_path[VMS_BACKING_SIZE + 8];
    snprintf(backing_path, sizeof(backing_path), "/dev/%s", backing);
    char *helper_argv[] = {
        (char *)VMS_MOUNT_HELPER_PATH, (char *)"mount",
        backing_path, mount_point, NULL
    };
    int helper_errno = 0;
    int hrc = run_mount_helper(helper_argv, &helper_errno);
    if (hrc == 3) {
        vms_kif_dalloc(dev_name);
        dcl_error("SYSTEM", 4, "NOPRIV",
                  "insufficient privilege or object protection violation");
        return SS$_NOPRIV;
    }
    if (hrc != 0) {
        vms_kif_dalloc(dev_name);
        dcl_error("OVMX", 4, "MOUNTFAIL",
                  "%s would not mount as vmsfs: %s", dev_name,
                  hrc == 1 ? strerror(helper_errno) : "mount helper did not run");
        return SS$_BUGCHECK;
    }

    /* Tell the per-process VMS-filespec translator (the same mechanism
     * DKA0:/SYSDISK_MOUNT already uses) so filespecs against this device
     * resolve for the rest of this session. */
    vmsfs_device_add(log_name, mount_point);

    /* Create logical name for device -> linux path */
    const char *table = LNM_PROCESS_TABLE;
    if (dcl_has_qualifier(cmd, "SYSTEM"))
        table = LNM_SYSTEM_TABLE;

    lnm_manager_t *mgr = lnm_get_manager();
    if (mgr) {
        lnm_create(mgr, table, log_name, mount_point,
                   LNM_ATTR_TERMINAL, LNM_MODE_USER);
    }

    /*
     * DISK$<volume-label> (vms-f83): VMS MOUNT defines a logical name from the
     * volume's own label so the disk can be addressed by label independent of
     * the physical unit -- DISK$MYVOL:[DIR]FILE resolves wherever MYVOL is
     * mounted (VSI OpenVMS System Manager's Manual, Vol. 1, "Mounting
     * Volumes"; VSI OpenVMS DCL Dictionary, MOUNT). The label is READ FROM THE
     * VOLUME (its vmsfs home block, via the setuid helper), not the command-
     * line token, so the DISK$ name always reflects the disk actually mounted;
     * if it cannot be read, no DISK$ logical is defined rather than
     * fabricating one (INV-DCL). The equivalence is the device (DKA100:), so
     * DISK$<label>:[dir]file translates through the device the mount just
     * established. DISMOUNT removes it. Placed in the same table as the device
     * logical above so it is resolvable in exactly the same scope.
     */
    char vol_label[16] = "";
    if (mgr && mount_helper_readlabel(backing_path, vol_label,
                                      sizeof(vol_label))) {
        (void)dcl_mount_define_disk(mgr, table, vol_label, dev_name);
    }

    printf("%%MOUNT-I-MOUNTED, %s mounted on _%s\n", label, dev_name);
    return SS$_NORMAL;
}


/*
 * DISMOUNT - umount(2) a VMS disk unit dismounted by MOUNT (vms-651).
 *
 * Syntax: DISMOUNT device:
 *
 * Mirrors cmd_mount(): PRV$M_MOUNT through the executive, "is it mounted"
 * from /proc/mounts, umount(2), then release the executive's claim
 * (vms_kif_dalloc) and the filespec-translator entry.
 */
int cmd_dismount(struct dcl_command *cmd)
{
    if (cmd->param_count < 1) {
        dcl_error("DISMOUNT", 2, "NODEVICE",
                  "no device specified");
        return SS$_BADPARAM;
    }

    const char *device = cmd->params[0];

    /* Build canonical device name */
    char dev_name[16];
    size_t nlen = strlen(device);
    if (nlen >= sizeof(dev_name) - 1) nlen = sizeof(dev_name) - 2;
    for (size_t i = 0; i < nlen; i++)
        dev_name[i] = (char)toupper((unsigned char)device[i]);
    dev_name[nlen] = '\0';
    if (dev_name[nlen - 1] != ':') {
        dev_name[nlen] = ':';
        dev_name[nlen + 1] = '\0';
    }

    char log_name[16];
    strncpy(log_name, dev_name, sizeof(log_name) - 1);
    log_name[sizeof(log_name) - 1] = '\0';
    size_t lnlen = strlen(log_name);
    if (lnlen > 0 && log_name[lnlen - 1] == ':')
        log_name[lnlen - 1] = '\0';

    char mount_point[64];
    mount_point_for_device(log_name, mount_point, sizeof(mount_point));

    (void)vms_kif_open();

    uint32_t pst = vms_kif_chkpriv(PRV$M_MOUNT);
    if (pst == SS$_NOPRIV) {
        dcl_error("SYSTEM", 4, "NOPRIV",
                  "insufficient privilege or object protection violation");
        return SS$_NOPRIV;
    }
    if (!(pst & 1))
        return pst;

    if (!mount_point_is_mounted(mount_point)) {
        dcl_error("DISMOUNT", 2, "DEVNOTMNT",
                  "device is not mounted - _%s", dev_name);
        return SS$_DEVNOTMOUNT;
    }

    /*
     * Read the volume label WHILE STILL MOUNTED (vms-f83), so the DISK$<label>
     * logical MOUNT defined can be removed after umount. The label is read
     * from the volume, the same source MOUNT built the DISK$ name from, rather
     * than tracked in per-session state (there is none to trust). Best-effort:
     * if the label cannot be read there is no DISK$ logical to remove either.
     */
    char vol_label[16] = "";
    {
        char dbacking[VMS_BACKING_SIZE];
        memset(dbacking, 0, sizeof(dbacking));
        if (vms_kif_disk_resolve(dev_name, dbacking, sizeof(dbacking),
                                 NULL, NULL) == SS$_NORMAL) {
            char dbacking_path[VMS_BACKING_SIZE + 8];
            snprintf(dbacking_path, sizeof(dbacking_path), "/dev/%s", dbacking);
            (void)mount_helper_readlabel(dbacking_path, vol_label,
                                         sizeof(vol_label));
        }
    }

    /* The actual umount(2), performed by the setuid-root helper -- see the
     * matching comment on the mount(2) call in cmd_mount(). */
    char *helper_argv[] = {
        (char *)VMS_MOUNT_HELPER_PATH, (char *)"umount", mount_point, NULL
    };
    int helper_errno = 0;
    int hrc = run_mount_helper(helper_argv, &helper_errno);
    if (hrc == 3) {
        dcl_error("SYSTEM", 4, "NOPRIV",
                  "insufficient privilege or object protection violation");
        return SS$_NOPRIV;
    }
    if (hrc != 0) {
        dcl_error("OVMX", 4, "DISMOUNTFAIL",
                  "%s would not unmount: %s", dev_name,
                  hrc == 1 ? strerror(helper_errno) : "mount helper did not run");
        return SS$_BUGCHECK;
    }

    vms_kif_dalloc(dev_name);
    vmsfs_device_remove(log_name);

    /* Remove logical name */
    lnm_manager_t *mgr = lnm_get_manager();
    if (mgr) {
        lnm_delete(mgr, LNM_PROCESS_TABLE, log_name, LNM_MODE_USER);
        lnm_delete(mgr, LNM_SYSTEM_TABLE, log_name, LNM_MODE_USER);
        /* Remove the DISK$<label> logical MOUNT defined (vms-f83). Both
         * tables, mirroring the device-logical removal above -- MOUNT places
         * DISK$ in the process table by default and the system table under
         * /SYSTEM. */
        if (vol_label[0]) {
            (void)dcl_mount_remove_disk(mgr, LNM_PROCESS_TABLE, vol_label);
            (void)dcl_mount_remove_disk(mgr, LNM_SYSTEM_TABLE, vol_label);
        }
    }

    printf("%%DISMOUNT-I-DISMOUNTED, _%s dismounted\n", dev_name);
    return SS$_NORMAL;
}

/* ================================================================== */
/*                     EDIT Command                                    */
/* ================================================================== */

/* External EDT editor entry point (dcl_editor.c) */
extern int edt_run(const char *filepath);

/*
 * EDIT - Launch EDT line-mode editor on a file.
 *
 * Format: EDIT filespec
 */
int cmd_edit(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("EDIT", 2, "NOFILE", "missing file specification");
        return SS$_BADPARAM;
    }

    /* Resolve filespec to Linux path (file may not exist yet) */
    char linux_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], linux_path, sizeof(linux_path));

    return edt_run(linux_path);
}

/* ================================================================== */
/*                     ATTACH Command                                  */
/* ================================================================== */

/*
 * ATTACH - Transfer terminal control to another process.
 *
 * ATTACH [process-name]    — switch to named subprocess
 * ATTACH /ID=hex-pid       — switch by PID
 *
 * For now, uses the interrupted_pid from Ctrl-Y if available.
 */
int cmd_attach(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* Check /IDENTIFICATION=hex-pid qualifier. The CLD table (q_attach,
     * dcl_builtin.c) declares the canonical DCL Dictionary name
     * /IDENTIFICATION; dcl_validate_qualifiers() canonicalises the /ID
     * abbreviation to it before this read, so we read the full name. */
    const char *id_val = dcl_qualifier_value(cmd, "IDENTIFICATION");
    if (id_val && id_val[0]) {
        pid_t target = (pid_t)strtol(id_val, NULL, 16);
        if (target <= 0) {
            dcl_error("DCL", 4, "ATTFAIL", "invalid process id - %s", id_val);
            return SS$_BADPARAM;
        }
        /* Check if process exists */
        if (kill(target, 0) != 0) {
            dcl_error("DCL", 4, "ATTFAIL", "no such process");
            return SS$_NONEXPR;
        }
        /* Send SIGCONT and wait */
        kill(target, SIGCONT);
        extern volatile sig_atomic_t dcl_running_child;
        dcl_running_child = (sig_atomic_t)target;
        int wstatus;
        waitpid(target, &wstatus, WUNTRACED);
        dcl_running_child = 0;
        if (WIFSTOPPED(wstatus)) {
            printf("\nInterrupt\n");
            ctx->interrupted_pid = target;
            return SS$_ABORT;
        }
        ctx->interrupted_pid = 0;
        return SS$_NORMAL;
    }

    /* Check for process-name parameter */
    if (cmd->param_count >= 1 && cmd->params[0][0] != '\0') {
        /* Named process attach — for now, only support interrupted process */
        if (ctx->interrupted_pid > 0) {
            pid_t pid = ctx->interrupted_pid;
            if (kill(pid, SIGCONT) != 0) {
                dcl_error("DCL", 4, "ATTFAIL", "no such process");
                ctx->interrupted_pid = 0;
                return SS$_NONEXPR;
            }
            extern volatile sig_atomic_t dcl_running_child;
            dcl_running_child = (sig_atomic_t)pid;
            int wstatus;
            waitpid(pid, &wstatus, WUNTRACED);
            dcl_running_child = 0;
            if (WIFSTOPPED(wstatus)) {
                printf("\nInterrupt\n");
                /* Keep interrupted_pid */
            } else {
                ctx->interrupted_pid = 0;
            }
            return SS$_NORMAL;
        }
        dcl_error("DCL", 4, "ATTFAIL", "no such process");
        return SS$_NONEXPR;
    }

    /* No parameter and no /ID — try interrupted process */
    if (ctx->interrupted_pid > 0) {
        pid_t pid = ctx->interrupted_pid;
        if (kill(pid, SIGCONT) != 0) {
            dcl_error("DCL", 4, "ATTFAIL", "no such process");
            ctx->interrupted_pid = 0;
            return SS$_NONEXPR;
        }
        extern volatile sig_atomic_t dcl_running_child;
        dcl_running_child = (sig_atomic_t)pid;
        int wstatus;
        waitpid(pid, &wstatus, WUNTRACED);
        dcl_running_child = 0;
        if (WIFSTOPPED(wstatus)) {
            printf("\nInterrupt\n");
        } else {
            ctx->interrupted_pid = 0;
        }
        return SS$_NORMAL;
    }

    dcl_error("DCL", 4, "ATTFAIL", "no process specified");
    return SS$_BADPARAM;
}

/* ================================================================== */
/*                     CONVERT Command                                 */
/* ================================================================== */

/*
 * CONVERT - Convert file format (basic file copy with record counting).
 *
 * CONVERT input-file output-file
 * /FDL=fdl-file — accepted but ignored with informational message
 */
int cmd_convert(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("CONVERT", 2, "NOINPFIL", "missing input and/or output file");
        return SS$_BADPARAM;
    }

    /* Check /FDL qualifier — accepted but not implemented */
    if (dcl_has_qualifier(cmd, "FDL")) {
        printf("%%CONVERT-I-FDL, /FDL qualifier accepted but ignored in this implementation\n");
    }

    char src_path[1024], dst_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], src_path, sizeof(src_path));
    dcl_resolve_path(ctx, cmd->params[1], dst_path, sizeof(dst_path));

    FILE *src = fopen(src_path, "r");
    if (!src) {
        dcl_error("CONVERT", 2, "OPENIN", "error opening %s as input", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    FILE *dst = fopen(dst_path, "w");
    if (!dst) {
        fclose(src);
        dcl_error("CONVERT", 2, "OPENOUT", "error opening %s as output", cmd->params[1]);
        return SS$_FILACCERR;
    }

    /* Copy line by line, counting records */
    char line[4096];
    long records = 0;
    while (fgets(line, sizeof(line), src)) {
        fputs(line, dst);
        records++;
    }

    fclose(src);
    fclose(dst);

    printf("%%CONVERT-S-CONVERTED, %ld records converted\n", records);
    return SS$_NORMAL;
}

/* ================================================================== */
/*                     INSTALL Command                                 */
/* ================================================================== */

/*
 * INSTALL - Manage known image list.
 *
 * INSTALL ADD image [/SHARED] [/OPEN] [/HEADER_RESIDENT]
 * INSTALL LIST [/FULL]
 * INSTALL REMOVE image
 *
 * WRAPS SYS$SYSTEM:INSTALL.EXE (src/install/install.c, bead vms-913.5) --
 * same pattern as cmd_analyze/cmd_mail/cmd_sysgen/cmd_sysman in this file:
 * the builtin verb just parses DCL syntax and re-execs the real external
 * utility via dcl_exec_utility(), it does not implement image-database
 * semantics itself.
 *
 * THIS REPLACES an earlier version that maintained its own flat-text list
 * at SYS$MANAGER:INSTALL_LIST.DAT (vms-913.7). That list was never read by
 * anything: IMGACT.EXE's known-image search path (src/imgact/known_images.c)
 * mmaps SYS$SYSTEM:VMS$KNOWN_IMAGES.DAT, the binary KFE database only
 * SYS$SYSTEM:INSTALL.EXE writes. So SYSTARTUP_VMS.COM's `INSTALL ADD`
 * commands were silently landing in a file nothing consulted, while image
 * activation always fell through to the Priority 2 filesystem-search path
 * (docs/design-image-activation.md section 4) -- functionally harmless
 * (activation still worked) but the Known Image Database (Priority 1) was
 * never actually populated, contradicting install.c's own header comment
 * ("deliberately NOT wired as a DCL builtin verb"). Fixed by making the
 * builtin do what that comment already assumed.
 */
int cmd_install(struct dcl_command *cmd)
{
    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("INSTALL", 2, "NOCMD", "missing subcommand (ADD, LIST, or REMOVE)");
        return SS$_BADPARAM;
    }

    char subcmd[32];
    strncpy(subcmd, cmd->params[0], sizeof(subcmd) - 1);
    subcmd[sizeof(subcmd) - 1] = '\0';
    for (int i = 0; subcmd[i]; i++)
        subcmd[i] = (char)toupper((unsigned char)subcmd[i]);

    if (strcmp(subcmd, "ADD") != 0 &&
        strcmp(subcmd, "LIST") != 0 &&
        strcmp(subcmd, "REMOVE") != 0) {
        dcl_error("INSTALL", 2, "INVCMD", "invalid subcommand - %s", subcmd);
        return SS$_BADPARAM;
    }

    if ((strcmp(subcmd, "ADD") == 0 || strcmp(subcmd, "REMOVE") == 0) &&
        (cmd->param_count < 2 || cmd->params[1][0] == '\0')) {
        dcl_error("INSTALL", 2, "NOIMAGE", "missing image name");
        return SS$_BADPARAM;
    }

    /* Build argv: [placeholder, SUBCMD, image-filespec?, /QUAL, /QUAL, ...]
     * install.c's main() expects argv[1]=verb, argv[2..]=cmd_add/cmd_list/
     * cmd_remove's own argv (filespec first for ADD/REMOVE, qualifiers with
     * a leading '/' throughout -- the parser already strips the '/' into
     * qualifiers[].name, so it is re-added here). */
    char *argv[40] = {NULL};
    int argc = 0;
    argv[argc++] = NULL; /* placeholder for resolved binary path */
    argv[argc++] = subcmd;
    if (cmd->param_count >= 2 && cmd->params[1][0] != '\0')
        argv[argc++] = cmd->params[1];

    char qual_bufs[32][66];
    int nquals = 0;
    for (int i = 0; i < cmd->qualifier_count && nquals < 32 && argc < 39; i++) {
        snprintf(qual_bufs[nquals], sizeof(qual_bufs[nquals]), "/%s",
                 cmd->qualifiers[i].name);
        argv[argc++] = qual_bufs[nquals];
        nquals++;
    }
    argv[argc] = NULL;

    return dcl_exec_utility("INSTALL.EXE", "INSTALL", argv, argc);
}

/* ================================================================== */
/*                     LINK Command                                    */
/* ================================================================== */

/*
 * LINK - Link object modules into an executable image.
 *
 * LINK file1[,file2,...]
 * /EXECUTABLE=name — output executable name
 * /MAP — produce link map
 *
 * Wraps the system linker (cc).
 */
int cmd_link(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("LINK", 2, "NOFILES", "no input files specified");
        return SS$_BADPARAM;
    }

    /* Collect input files — params may be comma-separated */
    char *input_files[64];
    int input_count = 0;
    char resolved[64][1024];

    for (int i = 0; i < cmd->param_count && input_count < 64; i++) {
        if (cmd->params[i][0] == '\0') continue;

        /* Split on commas */
        char temp[DCL_MAX_LINE];
        strncpy(temp, cmd->params[i], sizeof(temp) - 1);
        temp[sizeof(temp) - 1] = '\0';

        char *tok = strtok(temp, ",");
        while (tok && input_count < 64) {
            while (*tok == ' ') tok++;
            if (*tok) {
                dcl_resolve_path(ctx, tok, resolved[input_count],
                                 sizeof(resolved[input_count]));
                input_files[input_count] = resolved[input_count];
                input_count++;
            }
            tok = strtok(NULL, ",");
        }
    }

    if (input_count == 0) {
        dcl_error("LINK", 2, "NOFILES", "no input files specified");
        return SS$_BADPARAM;
    }

    /* Determine output name */
    char output_name[1024];
    const char *exe_val = dcl_qualifier_value(cmd, "EXECUTABLE");
    if (exe_val && exe_val[0]) {
        dcl_resolve_path(ctx, exe_val, output_name, sizeof(output_name));
    } else {
        /* Default: first input name without extension + .EXE */
        strncpy(output_name, input_files[0], sizeof(output_name) - 1);
        output_name[sizeof(output_name) - 1] = '\0';
        char *dot = strrchr(output_name, '.');
        if (dot) *dot = '\0';
        strncat(output_name, ".EXE", sizeof(output_name) - strlen(output_name) - 1);
    }

    /* Check /MAP qualifier */
    int want_map = dcl_has_qualifier(cmd, "MAP");
    char map_name[1024] = {0};
    if (want_map) {
        strncpy(map_name, output_name, sizeof(map_name) - 1);
        map_name[sizeof(map_name) - 1] = '\0';
        char *dot = strrchr(map_name, '.');
        if (dot) *dot = '\0';
        strncat(map_name, ".MAP", sizeof(map_name) - strlen(map_name) - 1);
    }

    /* Print linking message */
    printf("%%LINK-I-LINK, linking %s...\n", output_name);

    /* Build cc command: cc -o output input1 input2 ... [-Wl,-Map,mapfile] */
    char *argv[128];
    int argc = 0;
    argv[argc++] = "cc";
    argv[argc++] = "-o";
    argv[argc++] = output_name;
    for (int i = 0; i < input_count && argc < 120; i++)
        argv[argc++] = input_files[i];
    if (want_map) {
        static char map_flag[1100];
        snprintf(map_flag, sizeof(map_flag), "-Wl,-Map,%s", map_name);
        argv[argc++] = map_flag;
    }
    argv[argc] = NULL;

    pid_t pid = fork();
    if (pid == 0) {
        execvp("cc", argv);
        _exit(127);
    } else if (pid > 0) {
        int wstatus;
        waitpid(pid, &wstatus, 0);
        if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0) {
            printf("%%LINK-I-DONE, %s linked successfully\n", output_name);
            return SS$_NORMAL;
        } else {
            dcl_error("LINK", 2, "FAILED", "error linking image");
            return SS$_ABORT;
        }
    } else {
        dcl_error("LINK", 4, "CREPRC", "cannot create linker process");
        return SS$_ABORT;
    }
}

/* ================================================================== */
/*                     PHONE Command                                   */
/* ================================================================== */

/*
 * PHONE - Phone utility for interactive conversation.
 * Not available in OVMX — stub only.
 */
int cmd_phone(struct dcl_command *cmd)
{
    (void)cmd;
    dcl_error("PHONE", 0, "NOTAVAIL", "PHONE facility is not available");
    return SS$_NORMAL;
}

/* ================================================================== */
/*                     PRODUCT Command                                 */
/* ================================================================== */

/*
 * PRODUCT - Software product management (PCSI-equivalent, bead vms-df9).
 *
 * PRODUCT INSTALL <name> /SOURCE=<kit-filespec> [/DESTINATION=<devdir>]
 * PRODUCT SHOW PRODUCT [/DESTINATION=<devdir>]  — list installed products
 * PRODUCT SHOW HISTORY [/DESTINATION=<devdir>]  — show installation dates
 *
 * WRAPS SYS$SYSTEM:PRODUCT.EXE (src/product/product.c) -- same pattern as
 * cmd_analyze/cmd_install/cmd_sysgen/cmd_sysman/cmd_mail in this file: the
 * builtin verb only parses DCL syntax and re-execs the real external
 * utility via dcl_exec_utility(), it does not implement kit reading, the
 * product database, or file placement itself (vms-df9 constraint #1 --
 * DCL stays a shell). THIS REPLACES an earlier version that implemented
 * PRODUCT SHOW PRODUCT/HISTORY directly here (a fat builtin) and had no
 * PRODUCT.EXE at all; every other operation was a bare %PCSI-E-NOTIMPL.
 *
 * /SOURCE is resolved through dcl_resolve_path() before being handed to
 * PRODUCT.EXE (same as COPY/LINK's own input-file resolution), so it
 * accepts both an ordinary VMS filespec and a quoted literal Linux path.
 * /DESTINATION is a bare device name (canonicalized upper-case with a
 * trailing colon, like cmd_mount's own device-name handling) -- not a
 * filespec, so it is passed through uncooked; PRODUCT.EXE resolves it to
 * a mount point itself (src/product/product.c's pd_resolve_destination(),
 * the same "pure function of the device name" cmd_mount()/cmd_dismount()
 * already rely on, duplicated there rather than shared so PRODUCT.EXE
 * carries no dependency on this process's per-fork state).
 */
int cmd_product(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("PCSI", 0, "NOTIMPL", "operation not implemented");
        return SS$_NORMAL;
    }

    char subcmd[32];
    strncpy(subcmd, cmd->params[0], sizeof(subcmd) - 1);
    subcmd[sizeof(subcmd) - 1] = '\0';
    for (int i = 0; subcmd[i]; i++)
        subcmd[i] = (char)toupper((unsigned char)subcmd[i]);

    char *argv[8] = {NULL};
    int argc = 0;
    argv[argc++] = NULL;      /* placeholder for resolved binary path */
    argv[argc++] = subcmd;

    /* Declared at function scope (not nested inside the if/else below) so
     * the pointers stashed into argv[] stay valid until dcl_exec_utility()
     * actually reads them -- a buffer scoped to a nested block would have
     * its lifetime end before that call. */
    char source_arg[1200];
    char dest_arg[32];
    char showwhat[32];
    char dev[16];

    if (strcmp(subcmd, "INSTALL") == 0) {
        if (cmd->param_count < 2 || cmd->params[1][0] == '\0') {
            dcl_error("PCSI", 2, "NOPROD", "missing product name");
            return SS$_BADPARAM;
        }
        argv[argc++] = cmd->params[1];

        const char *src = dcl_qualifier_value(cmd, "SOURCE");
        if (!src || !src[0]) {
            dcl_error("PCSI", 2, "NOSOURCE", "missing /SOURCE kit filespec");
            return SS$_BADPARAM;
        }
        char linuxpath[1024];
        dcl_resolve_path(ctx, src, linuxpath, sizeof(linuxpath));
        snprintf(source_arg, sizeof(source_arg), "/SOURCE=%s", linuxpath);
        argv[argc++] = source_arg;

        const char *dst = dcl_qualifier_value(cmd, "DESTINATION");
        if (dst && dst[0]) {
            size_t n = strlen(dst);
            if (n >= sizeof(dev) - 1) n = sizeof(dev) - 2;
            size_t i;
            for (i = 0; i < n; i++)
                dev[i] = (char)toupper((unsigned char)dst[i]);
            dev[i] = '\0';
            if (i == 0 || dev[i - 1] != ':') { dev[i] = ':'; dev[i + 1] = '\0'; }
            snprintf(dest_arg, sizeof(dest_arg), "/DESTINATION=%s", dev);
            argv[argc++] = dest_arg;
        }
    } else if (strcmp(subcmd, "SHOW") == 0) {
        if (cmd->param_count < 2 || cmd->params[1][0] == '\0') {
            dcl_error("PCSI", 0, "NOTIMPL", "operation not implemented");
            return SS$_NORMAL;
        }
        strncpy(showwhat, cmd->params[1], sizeof(showwhat) - 1);
        showwhat[sizeof(showwhat) - 1] = '\0';
        for (int i = 0; showwhat[i]; i++)
            showwhat[i] = (char)toupper((unsigned char)showwhat[i]);
        argv[argc++] = showwhat;

        const char *dst = dcl_qualifier_value(cmd, "DESTINATION");
        if (dst && dst[0]) {
            size_t n = strlen(dst);
            if (n >= sizeof(dev) - 1) n = sizeof(dev) - 2;
            size_t i;
            for (i = 0; i < n; i++)
                dev[i] = (char)toupper((unsigned char)dst[i]);
            dev[i] = '\0';
            if (i == 0 || dev[i - 1] != ':') { dev[i] = ':'; dev[i + 1] = '\0'; }
            snprintf(dest_arg, sizeof(dest_arg), "/DESTINATION=%s", dev);
            argv[argc++] = dest_arg;
        }
    } else {
        dcl_error("PCSI", 0, "NOTIMPL", "operation not implemented");
        return SS$_NORMAL;
    }

    argv[argc] = NULL;
    return dcl_exec_utility("PRODUCT.EXE", "PCSI", argv, argc);
}
