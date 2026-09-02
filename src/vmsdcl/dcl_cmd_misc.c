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
/* NOTE: no <mntent.h>. The mount table is parsed by hand from /proc/mounts with
 * fopen/fgets (dcl_unit_is_mounted below) -- getmntent(3)/setmntent(3) are a
 * glibc-only convenience with no musl-static symbol, and <mntent.h> does not
 * exist at all in the NetBSD/vax sysroot (netbsd-vax cross gate, vms-1cb2). The
 * include was vestigial: no mntent symbol, struct, or macro is referenced. */
#include <sys/statvfs.h>

#include "dcl/context.h"
#include "dcl/terminal.h"
#include "dcl/parser.h"
#include "dcl/symbol.h"
#include "dcl/cdu.h"
#include "dcl/dcl_cmd.h"
#include "dcl/disk_logical.h"
#include "dcl/help.h"
#include "dcl/dcl_rms.h"     /* dcl_rms_read_* -- the HELP ACP read seam (vms-4ac) */
#include "ssdef.h"
#include "ovmx_layout.h"
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

/* TELNET / FTP client engines (vms-dbb) -- the SAME header the QEMU proof
 * (tests/qemu/test_syssvc_tcpip_client.c) drives, so the shipped verb and the
 * proven code are one. Relative path: dcl_cmd_misc.c compiles in-place under
 * src/vmsdcl/ in both the CMake and the VMS-native (mk_dcl.sh) builds, so no
 * new -I is needed and no new compiled object joins DCL.EXE's native link. */
#include "../vmstcpip/services/tcpip_client.h"

/* PING (ICMP echo) engine (vms-80b) -- the SAME header the QEMU proof
 * (tests/qemu/test_syssvc_tcpip_ping.c) drives against a real /dev/vms, so the
 * shipped PING verb and the proven code are identical. */
#include "../vmstcpip/services/tcpip_ping.h"

/* TCP/IP Services CONFIG plane (vms-67f) -- the SAME single-header engine the
 * QEMU proof (tests/qemu/test_syssvc_tcpip_config.c) drives against a real
 * /dev/vms. It records the VMS-faithful TCPIP$* SYSTEM logical names in the
 * executive-resident LNM$SYSTEM table (honest, cross-process; SS$_NOSUCHDEV
 * with no executive), so `TCPIP SET INTERFACE` durably records the host
 * address the ordinary VMS way and `TCPIP SHOW CONFIGURATION` reads it back. */
#include "../vmstcpip/mgmt/tcpip_config.h"

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
 * SORT /KEY=(POSITION:n,SIZE:m) field-based sort (vms-e76). qsort's comparator
 * takes no context, so the parsed key spec lives in these file-scope statics for
 * the duration of the single-threaded SORT command. POSITION is the 1-based
 * VMS column; the sort compares only the [pos,size] field, not the whole line.
 */
static int sort_key_pos;      /* 1-based start column                          */
static int sort_key_size;     /* field width in bytes                          */
static int sort_key_reverse;  /* 1 => descending                              */

/* Copy the key field [pos-1 .. pos-1+size) of `line` into buf (NUL-terminated,
 * capped), stopping at end-of-line. A short line yields a short/empty field. */
static void sort_key_field(const char *line, char *buf, size_t bufsz)
{
    size_t start = (size_t)(sort_key_pos > 0 ? sort_key_pos - 1 : 0);
    /* Length up to end-of-line, computed without strcspn (not in the VMS-native
     * DECC$SHR symbol vector -- vms-61f/vms-e76). */
    size_t linelen = 0;
    while (line[linelen] && line[linelen] != '\n' && line[linelen] != '\r')
        linelen++;
    size_t n = 0;
    for (size_t i = start; i < linelen && (int)n < sort_key_size && n < bufsz - 1; i++)
        buf[n++] = line[i];
    buf[n] = '\0';
}

static int sort_cmp_key(const void *a, const void *b)
{
    char ka[256], kb[256];
    sort_key_field(*(const char **)a, ka, sizeof(ka));
    sort_key_field(*(const char **)b, kb, sizeof(kb));
    int c = strcasecmp(ka, kb);
    return sort_key_reverse ? -c : c;
}

/* Read the first integer that follows keyword `kw` in the upper-cased string
 * `up` (so "POSITION:5", "POS=5" and "POSITION 5" all yield 5). 0 if absent. */
static int sort_num_after(const char *up, const char *kw)
{
    const char *q = strstr(up, kw);
    if (!q) return 0;
    q += strlen(kw);
    while (*q && (*q < '0' || *q > '9')) q++;
    return atoi(q);
}

/* Parse a /KEY qualifier value ("(POSITION:n,SIZE:m[,DESCENDING])", parens and
 * spacing optional) into pos/size/desc. Returns 1 if a usable (pos,size) was
 * found. */
static int sort_parse_key(const char *val, int *pos, int *size, int *desc)
{
    char up[256];
    size_t i;
    *pos = 0; *size = 0; *desc = 0;
    if (!val) return 0;
    for (i = 0; val[i] && i < sizeof(up) - 1; i++)
        up[i] = (char)toupper((unsigned char)val[i]);
    up[i] = '\0';
    *pos  = sort_num_after(up, "POS");   /* POSITION or POS */
    *size = sort_num_after(up, "SIZE");
    *desc = (strstr(up, "DESC") != NULL);
    return (*pos > 0 && *size > 0);
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

    /* Sort: /KEY=(POSITION:n,SIZE:m[,DESCENDING]) sorts on the [pos,size] field;
     * otherwise the whole line. /REVERSE flips the order (XORed with a key's own
     * DESCENDING). vms-e76: previously /KEY was accepted but ignored -- the sort
     * was always whole-line, so a keyed SORT silently produced wrong output. */
    int reverse = dcl_has_qualifier(cmd, "REVERSE");
    const char *key_val = dcl_qualifier_value(cmd, "KEY");
    int kpos = 0, ksize = 0, kdesc = 0;

    if (key_val && sort_parse_key(key_val, &kpos, &ksize, &kdesc)) {
        sort_key_pos = kpos;
        sort_key_size = ksize;
        sort_key_reverse = (reverse ^ kdesc);
        qsort(lines, line_count, sizeof(char *), sort_cmp_key);
    } else if (!reverse) {
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
    sys_path[0] = '\0';                /* stays empty unless resolve fills it */
    int resolved_ok = (ctx &&
        dcl_resolve_path(ctx, vms_spec, sys_path, sizeof(sys_path)) == 0);
    if (!resolved_ok)
        sys_path[0] = '\0';           /* a failed resolve leaves no valid path */
    if (resolved_ok && access(sys_path, X_OK) == 0)
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

    /*
     * ATOMIC FLIP (vms-37e): with the /vms POSIX passthrough retired, the
     * SYS$SYSTEM utility images have no /vms home -- both access() probes above
     * fail on the real runtime. PID 1 stages the shipped utilities off the
     * genuine ODS-2 volume THROUGH THE ACP into OVMX_BOOT_STAGE_DIR (a tmpfs)
     * exactly as it stages the first-hop boot images (stage_boot_images(),
     * src/ovmx_init/ovmx_init.c); the bytes came from the ACP, never /vms
     * (INV-6). A utility is fork()+execve()d (not in-process activated) because
     * it must receive P1-P8 argv, so it needs that POSIX handoff. Rewrite the
     * resolved SYS$SYSTEM:<exe> path to its staged location and use it if the
     * stage exists. If the utility was never staged (not installed), fall
     * through -- the child's execvp() then fails HONESTLY with %-F-NOIMG, never
     * a /vms read. Host ctest / plain-container tooling keeps /vms, so `bin` is
     * already set there and this is never reached (behaviour unchanged). */
    char staged_path[PATH_MAX];
    if (!bin) {
        const char *resolved = (sys_path[0] ? sys_path : def_path);
        if (ovmx_boot_stage_exec_path(resolved, staged_path, sizeof(staged_path)) &&
            access(staged_path, X_OK) == 0)
            bin = staged_path;
    }

    /* Set argv[0] to resolved path or exe_name for PATH search */
    argv[0] = (char *)(bin ? bin : exe_name);

    /* SYS$INPUT-from-procedure (vms-1a9): a utility RUN from a .COM
     * (e.g. `$ RUN SYS$SYSTEM:AUTHORIZE` followed by ADD/MODIFY/EXIT data
     * lines) reads SYS$INPUT from the procedure, not the terminal. Set up
     * before the fork so the child inherits fd 0; restore on every return. */
    struct dcl_sysinput si;
    dcl_sysinput_setup(ctx, &si);

    uint32_t status = SS$_NORMAL;
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
            status = SS$_ABORT;
        } else if (WIFEXITED(wstatus)) {
            status = (WEXITSTATUS(wstatus) == 0) ? SS$_NORMAL : SS$_ABORT;
        }
    } else {
        dcl_error("DCL", 4, "CREPRC", "cannot create process for %s", facility);
        status = SS$_ABORT;
    }
    dcl_sysinput_restore(&si);
    return status;
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

/* INITIALIZE — format a volume with VMSFS */
int cmd_initialize(struct dcl_command *cmd)
{
    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("INIT", 2, "NODEV", "missing device specification");
        return SS$_BADPARAM;
    }
    if (cmd->param_count < 2 || cmd->params[1][0] == '\0') {
        dcl_error("INIT", 2, "NOLABEL", "missing volume label");
        return SS$_BADPARAM;
    }

    /* Build argv: [placeholder, device, label, size?]
     * INITIALIZE.EXE expects: argv[1]=device, argv[2]=label, argv[3]=size-in-MB (optional)
     */
    char *argv[5] = {NULL};
    int argc = 0;
    argv[argc++] = NULL; /* placeholder for resolved binary path */
    argv[argc++] = cmd->params[0];  /* device */
    argv[argc++] = cmd->params[1];  /* label */
    if (cmd->param_count >= 3 && cmd->params[2][0] != '\0')
        argv[argc++] = cmd->params[2];  /* optional size in MB */
    argv[argc] = NULL;

    return dcl_exec_utility("INITIALIZE.EXE", "INIT", argv, argc);
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

/*
 * Build the ordered HELP library search list on disk (the HLP$LIBRARY search
 * list). Each element is a readable Linux path to a .HLB (compiled) or .HLP
 * (source) library; help_open_libraries() merges them so a key defined in an
 * earlier library wins -- the documented VMS HELP search order.
 *
 * Order (VSI OpenVMS DCL Dictionary, HELP):
 *   1. $OVMX_HELPLIB  -- a LOCATOR override (a Linux path), the OVMX analogue of
 *      "HELP/LIBRARY=file"; content always comes from the real file. Used by the
 *      test harness and advanced setups.
 *   2. HLP$LIBRARY, HLP$LIBRARY_1 .. HLP$LIBRARY_n -- the process/system HELP
 *      library search-list logicals, translated in order. A translation with no
 *      file type defaults to .HLB.
 *   3. SYS$HELP:HELPLIB.HLB, then SYS$HELP:HELPLIB.HLP -- the default HELP
 *      library, always searched last.
 *
 * Duplicate resolved paths are dropped. Returns the element count.
 */
#define HELP_MAX_LIBS 18

/* Append a VMS spec (or Linux path) to the search list if it resolves to a
 * readable file not already present. `linux_in` marks `spec` as already a Linux
 * path (the $OVMX_HELPLIB locator). */
static void help_add_library(struct dcl_context *ctx,
                             char list[][1024], int *count,
                             const char *spec, int linux_in)
{
    if (*count >= HELP_MAX_LIBS)
        return;

    /*
     * vms-4ac: keep a VMS filespec AS A VMS SPEC here -- do NOT translate it to
     * a /vms Linux path. library_source_text() (dcl_help.c) reads it over the
     * Files-11 ACP on the product runtime (falling back to a POSIX read of the
     * /vms tree only where /dev/vms is absent -- host tooling / netbsd cross).
     * The old dcl_resolve_path() translation produced a /vms path that no longer
     * exists on the flip runtime, so HELP answered %HELP-E-OPENIN even though
     * SYS$HELP:HELPLIB.HLP is mastered on the volume. The $OVMX_HELPLIB locator
     * (linux_in) is already a Linux path and is kept as one.
     */
    char resolved[1024];
    snprintf(resolved, sizeof(resolved), "%s", spec);

    for (int i = 0; i < *count; i++)
        if (strcmp(list[i], resolved) == 0)
            return; /* already present */

    if (linux_in) {
        /* Locator override: a Linux path -- confirm it opens before adding. */
        FILE *fp = fopen(resolved, "rb");
        if (!fp)
            return;
        fclose(fp);
    }
    /* A VMS spec is added unconditionally; help_open_libraries() skips any that
     * cannot be read (ACP miss and POSIX-fallback miss both), and cmd_help
     * reports the honest %HELP-E-OPENIN when NONE resolve. */

    snprintf(list[*count], 1024, "%s", resolved);
    (*count)++;
    (void)ctx;
}

/* Give a VMS spec a default .HLB file type if it names no type (the last
 * path element -- after any ']' ':' '/' -- contains no '.'). */
static void help_default_type(const char *spec, char *out, size_t outsz)
{
    const char *base = spec;
    for (const char *p = spec; *p; p++)
        if (*p == ']' || *p == ':' || *p == '/' || *p == '>')
            base = p + 1;
    if (strchr(base, '.'))
        snprintf(out, outsz, "%s", spec);
    else
        snprintf(out, outsz, "%s.HLB", spec);
}

static int help_build_searchlist(struct dcl_context *ctx,
                                 char list[][1024], int max)
{
    int count = 0;
    (void)max;

    /* 1. $OVMX_HELPLIB locator override. */
    const char *env = getenv("OVMX_HELPLIB");
    if (env && env[0])
        help_add_library(ctx, list, &count, env, 1);

    /* 2. HLP$LIBRARY, HLP$LIBRARY_1 .. HLP$LIBRARY_n. */
    for (int n = 0; n <= 9 && count < HELP_MAX_LIBS; n++) {
        char logname[32];
        if (n == 0)
            snprintf(logname, sizeof(logname), "HLP$LIBRARY");
        else
            snprintf(logname, sizeof(logname), "HLP$LIBRARY_%d", n);

        char equiv[1024];
        if (dcl_translate_logical(logname, equiv, sizeof(equiv)) != 0 ||
            equiv[0] == '\0')
            continue;

        char spec[1024];
        help_default_type(equiv, spec, sizeof(spec));
        help_add_library(ctx, list, &count, spec, 0);
    }

    /* 3. The default library, always searched last (HLB preferred, then HLP). */
    help_add_library(ctx, list, &count, "SYS$HELP:HELPLIB.HLB", 0);
    help_add_library(ctx, list, &count, VMS_HELPLIB_PATH, 0);

    return count;
}

/* ================================================================== */
/*        HELP <-> Engine A CDU command tables (vms-01b)               */
/* ================================================================== */
/*
 * Fold the per-command parameter/qualifier information carried by the Engine A
 * command definitions (the CDU tables in dcl_builtin.c: struct dcl_verb with
 * its NULL-terminated struct dcl_qual_def array) into the HELP library tree, so
 * "HELP <verb>" surfaces EXACTLY the qualifiers the parser accepts instead of a
 * hand-maintained -- and, as the HELPLIB drift shows, already stale -- string.
 *
 * Clean-room provenance (project Rule 8): the qualifier value-forms rendered
 * below ("/QUAL", "/QUAL=value", "/QUAL=(value[,...])", "/QUAL=keyword", the
 * "/[NO]QUAL" negatable form) are the documented DCL qualifier syntaxes from
 * the public VSI OpenVMS DCL Dictionary per-command entries and the VSI OpenVMS
 * Command Definition Utility Manual (the .CLD "qualifier" statement's VALUE(...)
 * / NEGATABLE / DEFAULT attributes). The exact wording of the synthesized
 * "Format:/Keywords:/Default:" body lines is an OVMX presentation choice,
 * labeled as such -- no VMS byte/text layout is copied. The qualifier NAMES and
 * value-types are not invented: they are read straight from the live command
 * tables, which are themselves oracle-grounded (dcl_builtin.c).
 *
 * Only verbs that declare an explicit qualifier table are touched:
 *   - a NON-EMPTY table  -> the verb's "Qualifiers" subtopic is (re)built from
 *     the table, one "/NAME" key per qualifier; a verb absent from the library
 *     gets a minimal node synthesized from its brief help so a defined command
 *     still has authentic qualifier help.
 *   - an EXPLICIT-EMPTY table (the verb honours no qualifier) -> any stale
 *     "Qualifiers" listing is dropped, because listing qualifiers the parser
 *     would reject with %DCL-W-IVQUAL is precisely the out-of-sync lie this
 *     slice removes.
 *   - verb->quals == NULL (legacy accept-all: SET/SHOW umbrellas, pure
 *     delegators) -> left untouched; there is no authoritative CDU list to
 *     derive, so the hand-authored library entry stands (INV-DCL: never fake a
 *     definitive list we do not have).
 */

/* Render one qualifier's accepted-syntax token, e.g. "/[NO]DATE=keyword". */
static void cdu_qual_format(const struct dcl_qual_def *q, char *out, size_t n)
{
    const char *valpart = "";
    switch (q->vtype) {
    case CDU_VT_NONE:    valpart = "";               break;
    case CDU_VT_VALUE:   valpart = (q->qflags & CDU_Q_VALREQ) ? "=value"
                                                              : "[=value]"; break;
    case CDU_VT_LIST:    valpart = "=(value[,...])";  break;
    case CDU_VT_KEYWORD: valpart = "=keyword";        break;
    }
    snprintf(out, n, "/%s%s%s",
             (q->qflags & CDU_Q_NEGATABLE) ? "[NO]" : "", q->name, valpart);
}

/* Build the body text shown under "HELP <verb> QUALIFIERS /NAME". */
static void cdu_qual_body(const struct dcl_qual_def *q, char *out, size_t n)
{
    char fmt[96];
    cdu_qual_format(q, fmt, sizeof(fmt));

    size_t o = 0;
    int w;
    w = snprintf(out + o, n - o, " Format: %s\n", fmt);
    if (w > 0 && (size_t)w < n - o) o += (size_t)w;

    if (q->vtype == CDU_VT_KEYWORD && q->keywords) {
        w = snprintf(out + o, n - o, " Keywords:");
        if (w > 0 && (size_t)w < n - o) o += (size_t)w;
        for (int i = 0; q->keywords[i]; i++) {
            w = snprintf(out + o, n - o, " %s", q->keywords[i]);
            if (w > 0 && (size_t)w < n - o) o += (size_t)w;
        }
        w = snprintf(out + o, n - o, "\n");
        if (w > 0 && (size_t)w < n - o) o += (size_t)w;
    }
    if (q->deflt) {
        w = snprintf(out + o, n - o, " Default: %s\n", q->deflt);
        if (w > 0 && (size_t)w < n - o) o += (size_t)w;
    }
    if (q->qflags & CDU_Q_DEFAULT) {
        w = snprintf(out + o, n - o, " Enabled by default.\n");
        if (w > 0 && (size_t)w < n - o) o += (size_t)w;
    }
}

/* Graft the Engine A CDU qualifier tables onto an open HELP library. */
static void dcl_help_apply_cdu(help_lib_t *lib)
{
    if (!lib) return;

    int nverbs = 0;
    const struct dcl_verb *tbl = dcl_get_verb_table(&nverbs);
    if (!tbl) return;

    for (int i = 0; i < nverbs; i++) {
        const struct dcl_verb *v = &tbl[i];
        if (!v->name || !v->quals)
            continue; /* legacy accept-all: no authoritative CDU list */

        int has_quals = (v->quals[0].name != NULL);

        const char *p1[1] = { v->name };
        help_node_t *vnode = help_find(lib, p1, 1);

        if (!has_quals) {
            /* Command honours no qualifier: drop any stale listing. */
            if (vnode) {
                help_node_t *qn = help_node_find_child(vnode, "Qualifiers");
                if (qn) help_node_remove_child(vnode, qn);
            }
            continue;
        }

        if (!vnode) {
            /* Verb not documented in the library: synthesize a minimal node
             * from the CDU brief help so its qualifiers are still reachable. */
            vnode = help_node_add_child(lib->root, 1, v->name);
            if (!vnode) continue;
            if (v->help && v->help[0]) {
                char body[512];
                snprintf(body, sizeof(body), " %s\n", v->help);
                help_node_set_text(vnode, body);
            }
        }

        help_node_t *qnode = help_node_find_child(vnode, "Qualifiers");
        if (!qnode) qnode = help_node_add_child(vnode, 2, "Qualifiers");
        if (!qnode) continue;

        /* Authoritative rebuild: the table is the single source of truth. */
        help_node_clear_children(qnode);
        for (int j = 0; v->quals[j].name; j++) {
            char key[80];
            snprintf(key, sizeof(key), "/%s", v->quals[j].name);
            help_node_t *qn = help_node_add_child(qnode, 3, key);
            if (!qn) continue;
            char body[256];
            cdu_qual_body(&v->quals[j], body, sizeof(body));
            help_node_set_text(qn, body);
        }
    }
}

/*
 * THE HELP-LIBRARY ACP READ SEAM (vms-4ac). dcl_help.c is the pure HELP engine
 * and reaches these two functions through WEAK references, so the hermetic
 * engine unit test can link dcl_help.c alone. They live HERE -- in a TU already
 * part of DCL.EXE (native LINK.EXE build + cmake) -- and read the library
 * through DCL's OWN RMS reader (dcl_rms_read_*, the same path TYPE/COPY use over
 * the Files-11 ACP), so the runtime HELP reads SYS$HELP:HELPLIB.HLP over the
 * executive ACP instead of the fopen() of the retired /vms passthrough that
 * answered %HELP-E-OPENIN. No new native TU and no new shareable import (DCL
 * already imports sys$open/$get and vmsfs_to_linux_path), so the native DCL.EXE
 * image is unchanged in shape. HELP.EXE (the standalone image) carries its own
 * copy of this seam in dcl_help_acp.c (rms_textfile), since it is not DCL.
 *
 * A raw .HLP is line-oriented, so a record-by-record read reconstructs it; a
 * binary .HLB is taken through the POSIX/.HLB-reconstruct path by dcl_help.c
 * (help_type_is_hlb), never this text reader.
 */
char *help_acp_library_text(const char *vms_spec)
{
    struct dcl_context *ctx = dcl_get_context();
    uint32_t st = 0;
    struct dcl_rms_reader *r = dcl_rms_read_open(ctx, vms_spec, &st);
    if (!r)
        return NULL;

    char *text = NULL;
    size_t len = 0, cap = 0;
    char rec[4096];
    int eof = 0, n;

    /* Non-NULL "" for a readable-but-empty library. */
    { char *nb = malloc(1); if (!nb) { dcl_rms_read_close(r); return NULL; }
      nb[0] = '\0'; text = nb; cap = 1; }

    while ((n = dcl_rms_read_record(r, rec, sizeof(rec), &eof)) >= 0) {
        size_t need = len + (size_t)n + 2;
        if (need > cap) {
            size_t nc = cap ? cap : 256;
            while (need > nc) nc *= 2;
            char *nb = realloc(text, nc);
            if (!nb) { free(text); dcl_rms_read_close(r); return NULL; }
            text = nb; cap = nc;
        }
        if (n > 0) { memcpy(text + len, rec, (size_t)n); len += (size_t)n; }
        text[len++] = '\n';
        text[len] = '\0';
        if (eof) break;
    }
    dcl_rms_read_close(r);
    return text;
}

int help_acp_vms_to_linux(const char *vms_spec, char *buf, size_t bufsz)
{
    return $VMS_STATUS_SUCCESS(vmsfs_to_linux_path(vms_spec, buf, bufsz)) ? 1 : 0;
}

/*
 * HELP - hierarchical topic help, read from the HELP library (vms-01b).
 *
 * Walks the real topic tree parsed from the library data (src/vmsdcl/dcl_help.c)
 * -- "HELP", "HELP topic", "HELP topic subtopic ...", the "Additional
 * information available:" subtopic listing, and (interactively) the
 * "Topic?" / "<path> Subtopic?" prompt loop.  No topic content is hardcoded.
 * Before rendering, the Engine A CDU command tables are folded in
 * (dcl_help_apply_cdu) so per-command qualifiers track the accepted syntax.
 */
int cmd_help(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    static char lib_paths[HELP_MAX_LIBS][1024];
    int nlibs = help_build_searchlist(ctx, lib_paths, HELP_MAX_LIBS);
    if (nlibs == 0) {
        /* Honest failure -- no fake fallback (Rule 9 / INV-DCL). */
        dcl_error("HELP", 2, "OPENIN",
                  "error opening help library %s", VMS_HELPLIB_PATH);
        return SS$_NOSUCHFILE;
    }

    const char *paths[HELP_MAX_LIBS];
    for (int i = 0; i < nlibs; i++)
        paths[i] = lib_paths[i];

    help_lib_t *lib = help_open_libraries(paths, nlibs);
    if (!lib) {
        dcl_error("HELP", 2, "OPENIN",
                  "error opening help library %s", VMS_HELPLIB_PATH);
        return SS$_NOSUCHFILE;
    }

    /* Fold the Engine A CDU command tables in: per-command qualifiers now
     * track the actually-accepted syntax instead of a hand-maintained string. */
    dcl_help_apply_cdu(lib);

    /* Gather the requested topic path from the positional parameters. */
    const char *path[DCL_MAX_PARAMS];
    int npath = 0;
    for (int i = 0; i < cmd->param_count && npath < DCL_MAX_PARAMS; i++) {
        if (cmd->params[i][0] != '\0')
            path[npath++] = cmd->params[i];
    }

    /*
     * A slash token after HELP (e.g. "HELP DIRECTORY QUALIFIERS /EXCLUDE") is a
     * command-qualifier TOPIC, not a HELP qualifier -- VMS HELP looks it up as
     * the next topic-path component. DCL has already split those into
     * cmd->qualifiers (HELP is accept-all, so they are not IVQUAL'd); fold them
     * back onto the end of the topic path as "/NAME" keys, matching how the CDU
     * tables key qualifier subtopics. DCL loses the params/qualifiers
     * interleaving, so they land after the positional topics -- correct for the
     * usual "HELP verb [subtopic] /qual" form. */
    static char qual_key[32][72];
    for (int i = 0; i < cmd->qualifier_count && npath < DCL_MAX_PARAMS &&
                    i < 32; i++) {
        if (cmd->qualifiers[i].name[0] == '\0')
            continue;
        snprintf(qual_key[i], sizeof(qual_key[i]), "/%s%s",
                 cmd->qualifiers[i].negated ? "NO" : "",
                 cmd->qualifiers[i].name);
        path[npath++] = qual_key[i];
    }

    int status;
    if (npath > 0) {
        /*
         * A topic was named: show that node (text + subtopic listing) ONCE and
         * return to the DCL prompt. This deliberately does NOT open the
         * "<topic> Subtopic?" prompt loop, so that (a) HELP always returns to
         * "$" after a single command -- the contract every scripted/console
         * session relies on -- and (b) the built-in behaves identically to the
         * HELP.EXE image, which one-shots when given a topic and is gated on
         * exactly that (tests/qemu/test_product_install_e2e.sh). The subtopics
         * are listed under "Additional information available:"; the user drills
         * in with "HELP <topic> <subtopic>". (Opening the prompt loop even for
         * a fully-specified topic, as VMS does at a terminal, is a deferred
         * fidelity item under epic vms-01b.)
         */
        status = help_render(lib, path, npath, stdout);
    } else if (isatty(fileno(stdin))) {
        /* Bare HELP at a terminal: the interactive Topic? browser. */
        help_interactive(lib, NULL, 0, stdin, stdout);
        status = SS$_NORMAL;
    } else {
        /* Bare HELP, non-interactive input: list the top level once. */
        status = help_render(lib, NULL, 0, stdout);
    }

    help_close(lib);
    return status;
}

/* ================================================================== */
/*                     RECALL Command                                  */
/* ================================================================== */

/*
 * DCL command recall buffer (vms-7c7).
 *
 * VMS keeps the most recent commands you enter at the interactive command
 * level in a recall buffer, and RECALL replays them. The DCL Dictionary
 * (RECALL) documents the depth as the last 20 commands. This is a property of
 * the command interpreter, NOT of any terminal line-editing package: RECALL
 * works on a hardcopy terminal with no cursor keys at all.
 *
 * OVMX previously drove RECALL from GNU readline's history ring, so a build
 * without readline -- the static musl runtime that boots under QEMU -- reported
 *   %DCL-W-RECALL, command recall requires readline support
 * and RECALL did nothing. That is a build option leaking into VMS-visible
 * behaviour (INV-DCL: a facade, not an authentic VMS response). This DCL-owned
 * ring is the single source of truth for RECALL in every build; readline, when
 * present, still drives cursor-key line editing but no longer gates RECALL.
 *
 * The ring holds up to DCL_RECALL_MAX lines, oldest at index 0. RECALL numbers
 * them 1..N with 1 = the oldest still retained and N = the most recent, exactly
 * as RECALL/ALL prints them (DCL Dictionary, RECALL/ALL example).
 */
#define DCL_RECALL_MAX 20
static char *dcl_recall_ring[DCL_RECALL_MAX];
static int   dcl_recall_count;   /* valid entries, 0..DCL_RECALL_MAX */

void dcl_recall_push(const char *line)
{
    if (!line || !line[0])
        return;                  /* VMS does not record an empty line */

    char *dup = strdup(line);
    if (!dup)
        return;

    if (dcl_recall_count == DCL_RECALL_MAX) {
        /* Buffer full: drop the oldest, shift down, append at the end. */
        free(dcl_recall_ring[0]);
        memmove(&dcl_recall_ring[0], &dcl_recall_ring[1],
                (DCL_RECALL_MAX - 1) * sizeof(dcl_recall_ring[0]));
        dcl_recall_ring[DCL_RECALL_MAX - 1] = dup;
    } else {
        dcl_recall_ring[dcl_recall_count++] = dup;
    }
}

void dcl_recall_erase(void)
{
    for (int i = 0; i < dcl_recall_count; i++) {
        free(dcl_recall_ring[i]);
        dcl_recall_ring[i] = NULL;
    }
    dcl_recall_count = 0;
}

/*
 * RECALL - Show or re-execute commands from the recall buffer.
 *
 * RECALL          — show most recent command
 * RECALL /ALL     — numbered list of the buffer
 * RECALL /ERASE   — clear the buffer
 * RECALL n        — re-execute command number n
 * RECALL string   — find and re-execute most recent command matching string
 */
int cmd_recall(struct dcl_command *cmd)
{
    if (dcl_has_qualifier(cmd, "ERASE")) {
        /* RECALL/ERASE — empty the recall buffer (DCL Dictionary). */
        dcl_recall_erase();
        return SS$_NORMAL;
    }

    if (dcl_has_qualifier(cmd, "ALL")) {
        /* RECALL/ALL — numbered list, oldest (1) to most recent (N). */
        if (dcl_recall_count == 0) {
            printf("%%DCL-I-RECALL, no history available\n");
            return SS$_NORMAL;
        }
        for (int i = 0; i < dcl_recall_count; i++)
            printf("%5d  %s\n", i + 1, dcl_recall_ring[i]);
        return SS$_NORMAL;
    }

    if (cmd->param_count == 0) {
        /* RECALL with no args — show the most recent command. */
        if (dcl_recall_count == 0) {
            printf("%%DCL-I-RECALL, no history available\n");
            return SS$_NORMAL;
        }
        printf("%s\n", dcl_recall_ring[dcl_recall_count - 1]);
        return SS$_NORMAL;
    }

    /* Parameter given — a command number, or a matching-prefix string. */
    const char *param = cmd->params[0];
    int is_number = (param[0] != '\0');
    for (size_t i = 0; param[i]; i++) {
        if (!isdigit((unsigned char)param[i])) { is_number = 0; break; }
    }

    if (is_number) {
        /* RECALL n — re-execute command number n (1..count). */
        int n = (int)strtol(param, NULL, 10);
        if (n < 1 || n > dcl_recall_count) {
            printf("%%DCL-W-RECALL, no command number %d in history\n", n);
            return SS$_NORMAL;
        }
        const char *entry = dcl_recall_ring[n - 1];
        printf("%s\n", entry);
        return dcl_execute_line(entry);
    }

    /* RECALL string — most recent command whose start matches (case-blind). */
    size_t plen = strlen(param);
    for (int i = dcl_recall_count - 1; i >= 0; i--) {
        if (strncasecmp(dcl_recall_ring[i], param, plen) == 0) {
            printf("%s\n", dcl_recall_ring[i]);
            return dcl_execute_line(dcl_recall_ring[i]);
        }
    }
    printf("%%DCL-W-RECALL, no command matching \"%s\" in history\n", param);
    return SS$_NORMAL;
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
#if defined(__linux__)
        /* SIOCGIFNETMASK returns the mask in the ifr_netmask union member on
         * Linux. NetBSD's struct ifreq has no ifr_netmask; the OVMX TCP/IP
         * engine is the Linux substrate (vms-67f, AF_INET). On netbsd-vax the
         * netmask readout is not wired here -- shown honestly as "*" rather
         * than fabricated. */
        if (ioctl(sock, SIOCGIFNETMASK, &ifr) == 0) {
            struct sockaddr_in *mask = (struct sockaddr_in *)&ifr.ifr_netmask;
            inet_ntop(AF_INET, &mask->sin_addr, mask_str, sizeof(mask_str));
        }
#endif

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

#if defined(__linux__)
        /* SIOCGIFHWADDR + ifr_hwaddr are Linux-only (NetBSD reads the link-layer
         * address via getifaddrs/AF_LINK, not an ifreq ioctl). The Linux
         * substrate is the OVMX TCP/IP engine (vms-67f); on netbsd-vax the
         * /FULL hardware-address line is simply omitted -- honest, not faked. */
        if (full) {
            /* Show hardware address */
            if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
                unsigned char *hw = (unsigned char *)ifr.ifr_hwaddr.sa_data;
                printf("             Hardware address: %02X-%02X-%02X-%02X-%02X-%02X\n",
                       hw[0], hw[1], hw[2], hw[3], hw[4], hw[5]);
            }
        }
#else
        (void)full;
#endif
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

            /* Set netmask if provided. ifr_netmask is a Linux-only ifreq union
             * member; the OVMX TCP/IP engine is the Linux substrate (vms-67f).
             * On netbsd-vax the live SIOCSIFNETMASK apply is skipped -- the
             * netmask is still persisted to TCPIP$INTERFACE.DAT below. */
#if defined(__linux__)
            if (netmask) {
                struct sockaddr_in *mask = (struct sockaddr_in *)&ifr.ifr_netmask;
                mask->sin_family = AF_INET;
                inet_pton(AF_INET, netmask, &mask->sin_addr);
                if (ioctl(sock, SIOCSIFNETMASK, &ifr) < 0) {
                    printf("%%TCPIP-W-IOERR, failed to set netmask: %s\n",
                           strerror(errno));
                }
            }
#endif

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

    /* Record the host address in the VMS-faithful TCPIP$INET_HOSTADDR SYSTEM
     * logical, the ordinary VMS way (TCPIP$CONFIG defines exactly this name).
     * This is executive-resident shared state, not the .DAT file above: any
     * process -- and the sockets veneer/tools -- reads the same value back via
     * TCPIP SHOW CONFIGURATION. INV-6: if /dev/vms is absent the define fails
     * SS$_NOSUCHDEV and we say so honestly rather than fake a durable logical. */
    {
        uint32_t lst = tcpip_cfg_define_system(TCPIP_LNM_INET_HOSTADDR, host_ip);
        if (!(lst & 1)) {
            if (lst == SS$_NOSUCHDEV)
                printf("%%TCPIP-W-NOEXEC, executive absent -- TCPIP$INET_HOSTADDR "
                       "not recorded (address applied to the interface only)\n");
            else
                printf("%%TCPIP-W-LNMERR, could not record TCPIP$INET_HOSTADDR "
                       "(status %u)\n", lst);
        }
    }

    printf("%%TCPIP-I-INFO, interface configured\n");
    return SS$_NORMAL;
}

/*
 * TCPIP SHOW CONFIGURATION - Display the core IP configuration recorded in the
 * VMS-faithful TCPIP$* SYSTEM logical names (host name, domain, host address).
 * Reads the executive-resident LNM$SYSTEM values through the config engine --
 * the same shared state TCPIP$CONFIG / TCPIP SET INTERFACE wrote, not a
 * per-process copy. With no executive it says so honestly.
 */
static int cmd_tcpip_show_configuration(struct dcl_command *cmd)
{
    (void)cmd;
    struct { const char *lnm; const char *label; } items[] = {
        { TCPIP_LNM_INET_HOST,     "Host name" },
        { TCPIP_LNM_INET_DOMAIN,   "Domain"    },
        { TCPIP_LNM_INET_HOSTADDR, "Host address" },
    };
    char val[256];
    int any_noexec = 0;

    printf("\n");
    printf("%-16s%s\n", "Item", "Value");
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        uint32_t st = tcpip_cfg_translate_system(items[i].lnm, val, sizeof(val));
        if (st & 1)
            printf("%-16s%s\n", items[i].label, val);
        else if (st == SS$_NOSUCHDEV)
            any_noexec = 1;
        else
            printf("%-16s%s\n", items[i].label, "(not configured)");
    }
    if (any_noexec)
        printf("%%TCPIP-W-UNAVAIL, TCP/IP Services executive is not running -- "
               "the TCPIP$ SYSTEM logicals cannot be read\n");
    printf("\n");
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
        if (dcl_match_command(what, "CONFIGURATION", 4))
            return cmd_tcpip_show_configuration(cmd);

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
 * MOUNT - $MOUNT a VMS disk unit's ODS-2 volume through the executive ACP
 * (vms-651; vms-481 retired the old setuid mount(2)-as-vmsfs path).
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

    /* vms-481: the unit is resolved and its ODS-2 home block validated by the
     * executive ACP $MOUNT below (vms_kif_acp_mount / vms-127) -- the process no
     * longer resolves a backing block device or scans /proc/mounts for a /vms
     * passthrough. */

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
     * vms-481: MOUNT the ODS-2 volume EXECUTIVE-GLOBAL through the Files-11 ACP
     * ($MOUNT over /dev/vms, vms_kif_acp_mount / vms-127) -- not a setuid-root
     * mount(2) of a vmsfs passthrough. The executive reads and validates the
     * volume's ODS-2 home block + SCB and records it in the executive-global
     * mounted-volume table, so EVERY process that $ASSIGNs the unit sees the
     * same mounted volume (design §4.3). This retires the setuid mount(2)
     * helper, the /vms mount point, the per-process vmsfs_device_add filespec
     * translator, and the DISK$-label-via-helper path -- filespecs against the
     * unit now resolve by $ASSIGNing it through the ACP. Fail-honest (INV-6): no
     * ACP / no such device => the real SS$_ status, never a passthrough mount.
     */
    uint32_t mst = vms_kif_acp_mount(dev_name);
    if (mst == SS$_NOSUCHDEV) {
        vms_kif_dalloc(dev_name);
        dcl_error("SYSTEM", 0, "NOSUCHDEV", "no such device available");
        return SS$_NOSUCHDEV;
    }
    if (mst == SS$_DEVMOUNT) {
        vms_kif_dalloc(dev_name);
        dcl_error("MOUNT", 2, "DEVMOUNT",
                  "device already mounted - _%s", dev_name);
        return SS$_DEVMOUNT;
    }
    if (!(mst & 1)) {
        vms_kif_dalloc(dev_name);
        dcl_error("OVMX", 4, "MOUNTFAIL",
                  "%s would not mount as ODS-2", dev_name);
        return mst;
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

    (void)vms_kif_open();

    uint32_t pst = vms_kif_chkpriv(PRV$M_MOUNT);
    if (pst == SS$_NOPRIV) {
        dcl_error("SYSTEM", 4, "NOPRIV",
                  "insufficient privilege or object protection violation");
        return SS$_NOPRIV;
    }
    if (!(pst & 1))
        return pst;

    /*
     * vms-3a8: DISMOUNT the ODS-2 volume through the executive Files-11 ACP
     * ($DISMOUNT over /dev/vms, vms_kif_acp_dmount), MIRRORING cmd_mount()'s
     * vms-481 switch to vms_kif_acp_mount. The executive-global $MOUNT records
     * NO Linux VFS mount and NO /proc/mounts entry, so the retired
     * mount_point_is_mounted()/setuid-umount-helper path could never see an
     * ACP-mounted volume: it reported %DISMOUNT-E-DEVNOTMNT for a unit MOUNT had
     * just placed in the executive table, so the install's own DISMOUNT (after
     * PRODUCT INSTALL onto DKA100:) failed and the target volume was never
     * released. The ACP $DISMOUNT removes the executive-global volume entry;
     * because IO$_WRITEVBLK writes are synchronous (submit_bio_wait) and the
     * install rides cache=writethrough drives, the volume is already durable on
     * its backing device across the dismount (the kept vms-9b7 lesson).
     */
    uint32_t dst = vms_kif_acp_dmount(dev_name);
    if (dst == SS$_NOSUCHDEV) {
        /* No executive-global volume mounted on this unit -- fail-honest, the
         * ACP's own SS$_NOSUCHDEV for an unmounted unit (vms_ioctl_acp_dmount). */
        dcl_error("DISMOUNT", 2, "DEVNOTMNT",
                  "device is not mounted - _%s", dev_name);
        return SS$_DEVNOTMOUNT;
    }
    if (dst == SS$_DEVALLOC) {
        /* File-class channels are still $ASSIGNed to the volume (refcnt > 0):
         * an open file blocks the dismount, as VMS refuses to dismount a volume
         * with open files. */
        dcl_error("DISMOUNT", 2, "DEVNOTDISM",
                  "device cannot be dismounted -- files are still open on - _%s",
                  dev_name);
        return SS$_DEVALLOC;
    }
    if (!(dst & 1))
        return dst;

    /* Release the executive device-table claim MOUNT took (vms_kif_alloc). */
    vms_kif_dalloc(dev_name);

    /* Best-effort: drop any per-process / system device logical. The ACP MOUNT
     * defines none, so this is a no-op on the current path, kept idempotent. */
    lnm_manager_t *mgr = lnm_get_manager();
    if (mgr) {
        lnm_delete(mgr, LNM_PROCESS_TABLE, log_name, LNM_MODE_USER);
        lnm_delete(mgr, LNM_SYSTEM_TABLE, log_name, LNM_MODE_USER);
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
 * /SOURCE is passed through UNCOOKED (vms-3a8), NOT dcl_resolve_path()'d to a
 * Linux path: the kit lives on a MOUNTed distribution volume and PRODUCT.EXE
 * reads it by VMS filespec over the executive Files-11 ACP (do_install ->
 * rms_open_named_handle), the VMS way -- never a /vms POSIX passthrough, which
 * the atomic flip (vms-208) no longer materializes for a MOUNTed volume.
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
        /* vms-3a8: pass the /SOURCE VMS filespec through UNCOOKED, exactly like
         * /DESTINATION below. The kit lives on a MOUNTed distribution volume;
         * PRODUCT.EXE opens it by filespec over the executive Files-11 ACP
         * (do_install -> pd_kit_open_over_acp -> rms_open_named_handle), the
         * VMS way. It must NOT be dcl_resolve_path()'d to a /vms POSIX
         * passthrough: since the atomic flip (vms-208) a MOUNTed volume has no
         * /vms Linux mirror, so the resolved path named a file that does not
         * exist (%PCSI-E-OPENIN /vms/.../ovmx-os.kit) -- the exact Rule 9/INV-6
         * passthrough this converged flip excises. */
        snprintf(source_arg, sizeof(source_arg), "/SOURCE=%s", src);
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

/* ================================================================== */
/*             TELNET / FTP client tools (vms-dbb)                     */
/* ================================================================== */
/*
 * These verbs drive their connections over the INET pseudo-device BGn:
 * (vms-527) through the public $ASSIGN/$QIO/$DASSGN services -- the protocol
 * engines live in src/vmstcpip/services/tcpip_client.h (included above), the
 * SAME code tests/qemu/test_syssvc_tcpip_client.c proves against a real
 * /dev/vms. There is no userspace socket stack: if the executive is absent,
 * tcpip_connect() returns SS$_NOSUCHDEV and the verb reports it honestly
 * (CLAUDE.md Rule 9 / INV-6). Command grammar is from the public VSI OpenVMS
 * TCP/IP Services documentation; the wire is IETF-standard (Rule 8).
 *
 * SCOPE (vms-dbb): loopback / a reachable-peer client. FTP is single-transfer
 * (/GET, /PUT), passive-mode; TELNET is a half-duplex line client (a blocking
 * $QIO recv precludes a concurrent reader -- full-duplex AST-driven I/O and an
 * interactive FTP> shell are later increments). Host names need the BIND
 * resolver (a later phase), so a host argument must be a dotted-quad literal.
 */

/* Parse an unsigned decimal port (1..65535); 0 = empty/invalid. */
static uint16_t tcpip_tool_parse_port(const char *s)
{
    unsigned int n = 0;
    int any = 0;
    if (!s) return 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10u + (unsigned int)(*s - '0');
        if (n > 65535u) return 0;
        s++;
        any = 1;
    }
    if (*s != '\0' || !any) return 0;
    return (uint16_t)n;
}

/* Case-insensitive prefix test (avoids a strncasecmp dependency on DECC$SHR). */
static int tcpip_tool_iprefix(const char *line, const char *word)
{
    size_t i;
    for (i = 0; word[i]; i++)
        if (tolower((unsigned char)line[i]) != tolower((unsigned char)word[i]))
            return 0;
    return 1;
}

/* Resolve a host argument to a network-order IPv4 address. Name resolution (the
 * BIND resolver) is a later phase; only a dotted-quad literal is accepted. */
static int tcpip_tool_resolve(const char *facility, const char *host,
                              uint32_t *addr_be)
{
    if (tcpip_parse_ipv4(host, addr_be))
        return 1;
    dcl_error(facility, 2, "NORESOLVE",
              "host name resolution is not available yet (BIND resolver pending)"
              " - use a dotted-quad IP address");
    return 0;
}

int cmd_telnet(struct dcl_command *cmd)
{
    struct tcpip_conn conn;
    uint32_t addr_be = 0;
    uint16_t port = 23;
    char buf[1024];
    char line[1024];
    uint32_t got = 0;
    uint32_t st;

    if (cmd->param_count < 1) {
        dcl_error("TELNET", 2, "NOHOST", "missing host - supply an address to connect to");
        return SS$_BADPARAM;
    }
    if (!tcpip_tool_resolve("TELNET", cmd->params[0], &addr_be))
        return SS$_BADPARAM;
    if (cmd->param_count >= 2) {
        port = tcpip_tool_parse_port(cmd->params[1]);
        if (port == 0) {
            dcl_error("TELNET", 2, "IVPORT", "invalid port number - \\%s\\", cmd->params[1]);
            return SS$_BADPARAM;
        }
    }

    printf("%%TELNET-I-TRYING, trying %s port %u ...\n", cmd->params[0], (unsigned)port);
    st = tcpip_connect(&conn, addr_be, port);
    if (!(st & 1)) {
        if (st == SS$_NOSUCHDEV)
            dcl_error("TELNET", 2, "NONET",
                      "TCP/IP Services (BGn: executive device) is not available");
        else
            dcl_error("TELNET", 2, "NOCONN",
                      "cannot connect to %s port %u", cmd->params[0], (unsigned)port);
        return st;
    }
    printf("Connected to %s.\n", cmd->params[0]);

    /* Initial server banner, telnet option negotiation stripped. */
    st = tcpip_telnet_recv(&conn, buf, sizeof(buf) - 1, &got);
    if ((st & 1) && got) { fwrite(buf, 1, got, stdout); fflush(stdout); }

    /* Half-duplex line loop over SYS$INPUT. */
    while (fgets(line, sizeof(line), stdin)) {
        if (tcpip_tool_iprefix(line, "quit") || tcpip_tool_iprefix(line, "exit"))
            break;
        st = tcpip_telnet_send(&conn, line, (uint32_t)strlen(line));
        if (!(st & 1)) break;
        got = 0;
        st = tcpip_telnet_recv(&conn, buf, sizeof(buf) - 1, &got);
        if (!(st & 1) || got == 0) break;
        fwrite(buf, 1, got, stdout);
        fflush(stdout);
    }

    tcpip_close(&conn);
    printf("%%TELNET-I-SESSEND, connection to %s closed\n", cmd->params[0]);
    return SS$_NORMAL;
}

int cmd_ftp(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();
    uint32_t addr_be = 0;
    uint16_t port = 21;
    const char *user, *pass, *get, *put, *pq;

    if (cmd->param_count < 1) {
        dcl_error("FTP", 2, "NOHOST", "missing host - supply an address to connect to");
        return SS$_BADPARAM;
    }
    if (!tcpip_tool_resolve("FTP", cmd->params[0], &addr_be))
        return SS$_BADPARAM;

    pq = dcl_qualifier_value(cmd, "PORT");
    if (pq && pq[0]) {
        port = tcpip_tool_parse_port(pq);
        if (port == 0) { dcl_error("FTP", 2, "IVPORT", "invalid port - \\%s\\", pq); return SS$_BADPARAM; }
    }

    user = dcl_qualifier_value(cmd, "USER");
    pass = dcl_qualifier_value(cmd, "PASSWORD");
    if (!user || !user[0]) user = "anonymous";
    if (!pass || !pass[0]) pass = "ovmx@ovmx";

    get = dcl_qualifier_value(cmd, "GET");
    put = dcl_qualifier_value(cmd, "PUT");
    if ((!get || !get[0]) && (!put || !put[0])) {
        dcl_error("FTP", 2, "NOOP",
                  "specify /GET=remote-file or /PUT=local-file"
                  " (an interactive FTP> shell is a later increment)");
        return SS$_BADPARAM;
    }

    if (get && get[0]) {
        size_t cap = 1u << 20;                 /* 1 MiB transfer cap (increment) */
        unsigned char *buf = malloc(cap);
        uint32_t glen = 0;
        uint32_t st;
        const char *out;

        if (!buf) { dcl_error("FTP", 2, "INSFMEM", "insufficient memory"); return SS$_BADPARAM; }
        st = tcpip_ftp_get(addr_be, port, user, pass, get, buf, (uint32_t)cap, &glen);
        if (!(st & 1)) {
            free(buf);
            if (st == SS$_NOSUCHDEV)
                dcl_error("FTP", 2, "NONET", "TCP/IP Services (BGn: executive device) is not available");
            else
                dcl_error("FTP", 2, "GETERR", "RETR of %s failed", get);
            return st;
        }
        out = dcl_qualifier_value(cmd, "OUTPUT");
        if (out && out[0]) {
            char lpath[PATH_MAX];
            FILE *f;
            dcl_resolve_path(ctx, out, lpath, sizeof(lpath));
            f = fopen(lpath, "wb");
            if (!f) { free(buf); dcl_error("FTP", 2, "OPENOUT", "cannot open output file %s", out); return SS$_BADPARAM; }
            fwrite(buf, 1, glen, f);
            fclose(f);
            printf("%%FTP-S-RETR, %u bytes retrieved to %s\n", (unsigned)glen, out);
        } else {
            fwrite(buf, 1, glen, stdout);
            printf("\n%%FTP-S-RETR, %u bytes retrieved from %s\n", (unsigned)glen, get);
        }
        free(buf);
        return SS$_NORMAL;
    }

    /* /PUT */
    {
        char lpath[PATH_MAX];
        FILE *f;
        size_t cap = 1u << 20;
        unsigned char *buf;
        size_t n;
        const char *remote;
        uint32_t st;

        dcl_resolve_path(ctx, put, lpath, sizeof(lpath));
        f = fopen(lpath, "rb");
        if (!f) { dcl_error("FTP", 2, "OPENIN", "cannot open local file %s", put); return SS$_BADPARAM; }
        buf = malloc(cap);
        if (!buf) { fclose(f); dcl_error("FTP", 2, "INSFMEM", "insufficient memory"); return SS$_BADPARAM; }
        n = fread(buf, 1, cap, f);
        fclose(f);

        remote = dcl_qualifier_value(cmd, "REMOTE");
        if (!remote || !remote[0]) remote = put;

        st = tcpip_ftp_put(addr_be, port, user, pass, remote, buf, (uint32_t)n);
        free(buf);
        if (!(st & 1)) {
            if (st == SS$_NOSUCHDEV)
                dcl_error("FTP", 2, "NONET", "TCP/IP Services (BGn: executive device) is not available");
            else
                dcl_error("FTP", 2, "PUTERR", "STOR of %s failed", remote);
            return st;
        }
        printf("%%FTP-S-STOR, %u bytes stored to %s\n", (unsigned)n, remote);
        return SS$_NORMAL;
    }
}

/*
 * PING (ICMP echo) over TCP/IP Services BGn: (vms-80b). Drives the shared
 * tcpip_ping_echo() engine -- $ASSIGN TCPIP$DEVICE: + IO$_SETMODE(raw ICMP) +
 * $QIO -- the exact code path the QEMU proof exercises byte-exact against a real
 * /dev/vms. Host must be a dotted-quad IPv4 literal (BIND resolver pending, like
 * TELNET/FTP). /COUNT=n (default 4) sets how many echoes to send.
 */
int cmd_ping(struct dcl_command *cmd)
{
    /* A classic 56-byte ICMP data payload, seeded so an echo is verifiable. */
    unsigned char payload[56];
    unsigned char reply[sizeof(payload)];
    uint32_t addr_be = 0;
    uint16_t id;
    int count = 4;
    int i, replies = 0;
    const char *cq;
    size_t k;

    if (cmd->param_count < 1) {
        dcl_error("PING", 2, "NOHOST", "missing host - supply an address to ping");
        return SS$_BADPARAM;
    }
    if (!tcpip_tool_resolve("PING", cmd->params[0], &addr_be))
        return SS$_BADPARAM;

    cq = dcl_qualifier_value(cmd, "COUNT");
    if (cq && cq[0]) {
        int n = atoi(cq);
        if (n < 1 || n > 1000) {
            dcl_error("PING", 2, "IVCOUNT", "invalid count - \\%s\\", cq);
            return SS$_BADPARAM;
        }
        count = n;
    }

    for (k = 0; k < sizeof(payload); k++)
        payload[k] = (unsigned char)(0x40u + (k & 0x3fu));
    id = (uint16_t)(getpid() & 0xffffu);

    printf("PING %s: %u data bytes\n", cmd->params[0], (unsigned)sizeof(payload));

    for (i = 0; i < count; i++) {
        uint32_t rlen = 0;
        uint32_t st = tcpip_ping_echo(addr_be, id, (uint16_t)(i + 1),
                                      payload, (uint32_t)sizeof(payload),
                                      reply, (uint32_t)sizeof(reply), &rlen);
        if (st == SS$_NOSUCHDEV) {
            dcl_error("PING", 2, "NONET",
                      "TCP/IP Services (BGn: executive device) is not available");
            return st;
        }
        if ((st & 1) && rlen == sizeof(payload) &&
            memcmp(reply, payload, sizeof(payload)) == 0) {
            printf("%u bytes from %s: icmp_seq=%d\n",
                   (unsigned)rlen, cmd->params[0], i + 1);
            replies++;
        } else {
            printf("Request timeout for icmp_seq=%d\n", i + 1);
        }
    }

    printf("--- %s ping statistics ---\n", cmd->params[0]);
    printf("%d packets transmitted, %d packets received\n", count, replies);
    return replies > 0 ? SS$_NORMAL : SS$_ABORT;
}
