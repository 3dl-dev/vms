/*
 * test_syssvc_showproc.c - SHOW SYSTEM's column set and SHOW PROCESS's
 * TARGET, through the real DCL.EXE against a real executive (vms-6a7).
 *
 * WHY THIS SUITE EXISTS, AND WHAT IT IS GUARDING AGAINST.
 *
 * tests/qemu/test_syssvc_procnam.c proved that the executive's process
 * table exists, that $GETJPI resolves another process by name and by pid,
 * and that SHOW SYSTEM enumerates it. It proved nothing about the DISPLAY:
 * what columns SHOW SYSTEM prints and where, and whether SHOW PROCESS can
 * report a process other than the one running it.
 *
 * Before this item SHOW PROCESS could not. It described the caller out of
 * values that process had written about ITSELF -- the DCL context's
 * process_name, getpid(), getgid()/getuid(), a privilege list read from the
 * VMS_PRIVILEGES environment variable and seven hardcoded quota lines -- and
 * it ignored any parameter, so "SHOW PROCESS AUDIT_SERVER" printed the
 * caller. That is the facade class CLAUDE.md Rule 11 names, and it passes
 * every single-process test perfectly. So every positive assertion below is
 * A-WRITES / B-READS: the subject is created by one process, named in the
 * executive, and then reported by a DIFFERENT process through the real
 * user-visible DCL command.
 *
 * THE LAYOUT ASSERTIONS ARE ORACLE-PINNED, NOT SELF-CERTIFIED. VAX1
 * (OpenVMS VAX V7.3) was booted under vms-6a7 and every form asserted here
 * was captured through `cat -A` so the columns were counted:
 * docs/oracle/vax73-show-system-process.md Sections 1.1, 2.1, 3.1-3.3. A
 * byte position in this file that is not in that document is a bug in this
 * file.
 *
 * THE SUBJECT PROCESSES. The named subject is exec'd by $CREPRC with its
 * stdin redirected to a script that sleeps, so the running image is /bin/sh
 * -- a program that knows nothing about VMS and never opens /dev/vms. When
 * DCL then reports it BY NAME, the name can only have come from the
 * executive's table. The cross-group subject is a fork()ed helper that
 * setgid()s BEFORE its first kernel-interface call (the executive derives
 * the UIC from the task's credentials at registration, so it cannot declare
 * one later) and then names itself.
 *
 * Requires a real, insmod'd vms.ko at /dev/vms. If /dev/vms cannot be opened
 * -- which happens ONLY in the CI negative-control rig, never in the product
 * (vms-0ff: PID 1 refuses to boot without the executive) -- it exercises the
 * no-fabricated-success checks in device_absent_checks() and exits with
 * EXIT_SKIP (77), never a fake pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "jpidef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77

static int pass = 0;
static int fail = 0;

#define CHECK(cond, msg) do {                                 \
        if (cond) { pass++; printf("  PASS: %s\n", (msg)); }   \
        else      { fail++; printf("  FAIL: %s\n", (msg)); }   \
    } while (0)

/* Names are unique across the whole initramfs run: every suite in
 * tests/qemu shares one booted executive and one process table. */
#define SUBJECT_NAME   "OVMX6A7SUBJ"
#define ABSENT_NAME    "OVMX6A7NONE"
#define XGRP_NAME      "OVMX6A7XGRP"
#define DCLGRP_NAME    "OVMX6A7DCLR"
#define SETUP_FAIL_MARK "OVMX6A7SETUPFAIL"

#define HOLD_SCRIPT    "/tmp/ovmx6a7_hold.sh"
#define SUBJECT_IMAGE  "/bin/sh"

/* Two UIC groups, neither of them the parent's (0) and neither shared with
 * tests/qemu/test_syssvc_procnam.c's 300/301/302. The unprivileged DCL
 * caller lives in DCL_UIC_GROUP; the process it may not read lives in
 * XGRP_UIC_GROUP. */
#define XGRP_UIC_GROUP 312
#define DCL_UIC_GROUP  311

/*
 * ORACLE-PINNED COLUMN POSITIONS (0-based).
 * docs/oracle/vax73-show-system-process.md Sections 1.1 and 2.1.
 */
#define SYS_COL_NAME        9    /* SHOW SYSTEM: Process Name field start */
#define SYS_NAME_WIDTH      15   /* SHOW SYSTEM: Process Name field width */
#define PROC_COL_LEFTLABEL  26   /* SHOW PROCESS: "User:" / "Node:"        */
#define PROC_COL_RIGHTLABEL 49   /* SHOW PROCESS: "Process ID:" / "name:"  */
#define PROC_COL_VALUE      63   /* SHOW PROCESS: the pid / quoted name    */
#define PROC_LABEL_WIDTH    20   /* SHOW PROCESS: body label field width   */

/* The heading OVMX prints, byte for byte. VMS's is
 * "  Pid    Process Name    State  Pri      I/O       CPU       Page flts  Pages";
 * (Section 1.1). vms-a7e made the executive hold CPU time, PAGE FAULTS and
 * RESIDENT PAGES (sourced in-kernel from the task the process table pins,
 * struct vms_procinfo.cputim/pageflts/pages), so OVMX now sources and prints
 * the CPU, Page flts and Pages columns at VMS's own widths. It still cannot
 * source State, Pri or the I/O split (no VMS scheduler, no VMS priority, no
 * direct/buffered I/O split), so those three columns are removed WHOLE and
 * the survivors close up (Section 5.1's rule, now with more survivors):
 *   Pid 0-7, Name 9-23, CPU 25-40, Page flts 42-51, Pages 53-59. */
#define SYS_HEADING \
    "  Pid    Process Name          CPU         Page flts   Pages"

/* Oracle-pinned messages (Sections 3.2, 3.3). */
#define MSG_NONEXPR "%SYSTEM-W-NONEXPR, nonexistent process"
#define MSG_NOPRIV  "%SYSTEM-F-NOPRIV, insufficient privilege or object " \
                    "protection violation"

static struct dsc$descriptor_s str_dsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

static int read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
    size_t put = 0;
    while (put < n) {
        ssize_t w = write(fd, (const char *)buf + put, n - put);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        put += (size_t)w;
    }
    return 0;
}

struct helper_report {
    uint32_t ok;
    uint32_t vms_pid;
};

/* ---------------------------------------------------------------------
 * Line helpers over captured DCL output.
 * --------------------------------------------------------------------- */

/* line_starting - the first line whose text begins with `prefix`. */
static const char *line_starting(const char *text, const char *prefix)
{
    size_t n = strlen(prefix);
    const char *line = text;

    while (line && *line) {
        if (strncmp(line, prefix, n) == 0)
            return line;
        line = strchr(line, '\n');
        if (line) line++;
    }
    return NULL;
}

/* line_containing - the first line containing `needle`. */
static const char *line_containing(const char *text, const char *needle)
{
    const char *line = text;

    while (line && *line) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        const char *hit = strstr(line, needle);
        if (hit && (size_t)(hit - line) < len)
            return line;
        line = eol ? eol + 1 : NULL;
    }
    return NULL;
}

/* line_is - is the line at `line` exactly `want` (ignoring CR)? */
static int line_is(const char *line, const char *want)
{
    size_t n;

    if (!line) return 0;
    n = strlen(want);
    if (strncmp(line, want, n) != 0) return 0;
    return line[n] == '\n' || line[n] == '\r' || line[n] == '\0';
}

/* has_line - does the output contain a line exactly equal to `want`? */
static int has_line(const char *text, const char *want)
{
    const char *line = text;

    while (line && *line) {
        if (line_is(line, want)) return 1;
        line = strchr(line, '\n');
        if (line) line++;
    }
    return 0;
}

/*
 * token_at - does `tok` START at exactly column `col` of this line?
 *
 * This is how every geometry assertion is made: the column is checked, not
 * merely the presence of the word. A layout that prints the right labels at
 * the wrong offsets fails here, which is the whole point of pinning the
 * oracle's `cat -A` capture rather than its prose.
 *
 * The column before the token must be a space, so the check pins the START
 * of the field and not merely a substring that happens to straddle it: a
 * label shifted one place left or right fails. It does NOT require every
 * preceding column to be blank -- on SHOW PROCESS's first header line the
 * columns before "User:" hold the date, and the columns before
 * "Process ID:" hold the user name field.
 */
static int token_at(const char *line, int col, const char *tok)
{
    if (!line) return 0;
    for (int i = 0; i < col; i++)
        if (line[i] == '\0' || line[i] == '\n')
            return 0;                          /* line is shorter than col */
    if (col > 0 && line[col - 1] != ' ') return 0;
    return strncmp(line + col, tok, strlen(tok)) == 0;
}

/*
 * label_value_at - a body line "<label>" padded to `width`, then `value`.
 * Returns 1 when the label is at column 0 and the value starts at exactly
 * column `width`.
 */
static int label_value_at(const char *line, const char *label, int width,
                          const char *value)
{
    size_t l;

    if (!line) return 0;
    l = strlen(label);
    if (strncmp(line, label, l) != 0) return 0;
    for (size_t i = l; i < (size_t)width; i++)
        if (line[i] != ' ') return 0;
    return strncmp(line + width, value, strlen(value)) == 0;
}

/* sys_row_for - the SHOW SYSTEM row naming `name`, or NULL.
 *
 * A row is "%08X %-15s %s": the pid starts at COLUMN ZERO (oracle-pinned,
 * Section 1.1 -- VMS has no leading space there) and the name field is
 * columns 9-23. */
static const char *sys_row_for(const char *text, const char *name)
{
    const char *line = text;
    size_t namelen = strlen(name);

    while (line && *line) {
        int i;
        for (i = 0; i < 8; i++)
            if (!isxdigit((unsigned char)line[i]))
                break;
        if (i == 8 && line[8] == ' ' &&
            strncmp(line + SYS_COL_NAME, name, namelen) == 0)
            return line;
        line = strchr(line, '\n');
        if (line) line++;
    }
    return NULL;
}

/* pid_on_line - the 8 hex digits at column `col` of this line, or 0. */
static uint32_t pid_on_line(const char *line, int col)
{
    char buf[9];

    if (!line) return 0;
    for (int i = 0; i < 8; i++) {
        if (!isxdigit((unsigned char)line[col + i])) return 0;
        buf[i] = line[col + i];
    }
    buf[8] = '\0';
    return (uint32_t)strtoul(buf, NULL, 16);
}

/* ---------------------------------------------------------------------
 * Driving the real DCL.EXE.
 * --------------------------------------------------------------------- */

/*
 * run_dcl - feed `script` to /bin/DCL.EXE and capture its output.
 *
 * DCL.EXE is staged into the initramfs at /bin by tests/qemu/Dockerfile
 * (absence there is a FATAL image-build error, not a skip). This is the only
 * place SHOW SYSTEM and SHOW PROCESS can be proven: they are readers of the
 * executive's process table, and ctest never runs anywhere one exists.
 *
 * `uic_group` >= 0 makes the child setgid() into that group and drop WORLD
 * from its own executive-held privilege mask BEFORE exec'ing DCL, so it
 * becomes a caller the executive will refuse cross-group identity reads for.
 * WORLD is the only privilege that authorises such a read (oracle-pinned,
 * docs/oracle/vax73-privileges.md Section 5.4); SETPRV is dropped with it so
 * the drop is one-way. All of that survives execve() because the executive's
 * entry is keyed by the pid, which exec does not change -- which is what
 * lets this hand DCL.EXE an identity it could not have given itself.
 *
 * On any setup failure the child prints SETUP_FAIL_MARK on the captured
 * stream instead of exec'ing, so a broken arrangement fails LOUDLY rather
 * than quietly running the command with full privilege and passing for the
 * wrong reason.
 */
static int run_dcl(const char *script, int uic_group, const char *prcnam,
                   char *out, size_t outsz)
{
    int in_pipe[2], out_pipe[2];

    out[0] = '\0';
    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) { close(in_pipe[0]); close(in_pipe[1]); return -1; }

    /*
     * Flush every stdio buffer -- including this process's own (parent)
     * stdout -- before forking. Without this, the child inherits a copy
     * of the PARENT's unflushed stdout buffer; the child's own
     * fflush(stdout) after dup2'ing the capture pipe then writes that
     * inherited parent data into the child's pipe, splicing a prior
     * run_dcl() call's transcript into this call's capture. Latent when
     * stdout is line-buffered (console); live the moment stdout is fully
     * buffered (redirected to a file) -- see vms-cdb.
     */
    fflush(NULL);

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);

        if (uic_group >= 0) {
            struct vms_procinfo info;
            uint32_t st;

            if (setgid((gid_t)uic_group) != 0) {
                printf(SETUP_FAIL_MARK " setgid\n"); fflush(stdout); _exit(126);
            }
            st = vms_kif_getjpi_self(&info);        /* first call: registers */
            if (!(st & 1)) {
                printf(SETUP_FAIL_MARK " register %08X\n", st);
                fflush(stdout); _exit(126);
            }
            if (prcnam && !(vms_kif_setprn(prcnam) & 1)) {
                printf(SETUP_FAIL_MARK " setprn\n"); fflush(stdout); _exit(126);
            }
            st = vms_kif_setident("OVMXDCLR", info.uic,
                                  info.perm_privs &
                                  ~(uint64_t)(VMS_PRV_M_WORLD |
                                              VMS_PRV_M_SETPRV));
            if (!(st & 1)) {
                printf(SETUP_FAIL_MARK " setident %08X\n", st);
                fflush(stdout); _exit(126);
            }
            if (!(vms_kif_getjpi_self(&info) & 1) ||
                (info.cur_privs & VMS_PRV_M_WORLD) != 0) {
                printf(SETUP_FAIL_MARK " world-still-held\n");
                fflush(stdout); _exit(126);
            }
        }

        execl("/bin/DCL.EXE", "DCL.EXE", (char *)NULL);
        printf(SETUP_FAIL_MARK " exec\n");
        fflush(stdout);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    if (write_full(in_pipe[1], script, strlen(script)) != 0) {
        /* fall through: the child's output still gets captured */
    }
    close(in_pipe[1]);

    size_t used = 0;
    for (;;) {
        ssize_t n = read(out_pipe[0], out + used, outsz - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
        if (used >= outsz - 1) break;
    }
    out[used] = '\0';
    close(out_pipe[0]);

    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return 0;
}

/* ---------------------------------------------------------------------
 * Subjects.
 * --------------------------------------------------------------------- */

static uint32_t spawn_named(const char *prcnam, uint32_t *out_pid)
{
    struct dsc$descriptor_s img = str_dsc(SUBJECT_IMAGE);
    struct dsc$descriptor_s in  = str_dsc(HOLD_SCRIPT);
    struct dsc$descriptor_s nd  = str_dsc(prcnam);

    *out_pid = 0;
    return sys$creprc(out_pid, &img, &in, NULL, NULL, NULL, NULL, &nd,
                      0, 0, 0, 0);
}

static uint32_t linux_pid_of(uint32_t vms_pid)
{
    struct vms_procinfo info;

    if (!(vms_kif_getjpi_pid(vms_pid, &info) & 1))
        return 0;
    return info.linux_pid;
}

static int comm_of(uint32_t pid, char *out, size_t outsz)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%u/comm", (unsigned)pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(out, (int)outsz, f)) { fclose(f); return -1; }
    fclose(f);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
    return 0;
}

/* Polls an OBSERVED condition -- /proc/<pid>/comm -- rather than sleeping a
 * fixed interval, so nothing here is paced by a guess about how fast an
 * emulated guest runs. A fixed-sleep test is a flaky test. */
static int wait_for_exec(uint32_t pid, const char *want)
{
    char comm[64];
    for (int i = 0; i < 2000; i++) {
        if (comm_of(pid, comm, sizeof(comm)) == 0 && strcmp(comm, want) == 0)
            return 0;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return -1;
}

/*
 * xgrp_helper - a NAMED process in a UIC group of its own. Never returns.
 *
 * setgid()s BEFORE its first vms_kif_* call: the executive derives the UIC
 * from the task's credentials AT REGISTRATION, so a process cannot declare
 * one later. It stays uid 0 so it keeps the privilege to open /dev/vms.
 */
static void xgrp_helper(int cmdfd, int repfd)
{
    struct helper_report rep;
    struct vms_procinfo info;

    memset(&rep, 0, sizeof(rep));

    if (setgid(XGRP_UIC_GROUP) != 0) {
        (void)write_full(repfd, &rep, sizeof(rep));
        _exit(1);
    }
    if (!(vms_kif_getjpi_self(&info) & 1)) {
        (void)write_full(repfd, &rep, sizeof(rep));
        _exit(1);
    }
    if (!(vms_kif_setprn(XGRP_NAME) & 1)) {
        (void)write_full(repfd, &rep, sizeof(rep));
        _exit(1);
    }

    rep.ok = 1;
    rep.vms_pid = info.vms_pid;
    if (write_full(repfd, &rep, sizeof(rep)) != 0)
        _exit(1);

    for (;;) {
        char cmd;
        if (read_full(cmdfd, &cmd, 1) != 0)
            _exit(0);
    }
}

/* ---------------------------------------------------------------------
 * The negative-control path: with no /dev/vms, NOTHING may report success.
 * --------------------------------------------------------------------- */
static int device_absent_checks(void)
{
    printf("  (no /dev/vms -- running device-absent assertions)\n");

    struct vms_procinfo info;
    uint32_t index = 0;
    uint32_t st = vms_kif_procscan(&index, &info);
    CHECK(!(st & 1),
          "the process-table scan does not report success with no executive");

    struct dsc$descriptor_s nd = str_dsc(SUBJECT_NAME);
    struct item_list_3 items[2];
    uint32_t pid = 0;
    uint16_t rl = 0;
    memset(items, 0, sizeof(items));
    items[0].buflen = sizeof(uint32_t);
    items[0].item_code = JPI$_PID;
    items[0].bufaddr = &pid;
    items[0].retlen = &rl;
    st = sys$getjpi(0, NULL, &nd, items, NULL, NULL, 0);
    CHECK(!(st & 1),
          "sys$getjpi by name does not report success with no executive");

    /*
     * The user-visible command must fabricate nothing either. With no row
     * to read, SHOW PROCESS must print none of the fields the deleted
     * fabrication printed unconditionally -- this is the same assertion
     * tests/dcl/test_show_process.sh makes on the host, made here against
     * the binary that ships in the image.
     *
     * GUARDED ON DCL.EXE ACTUALLY BEING THERE. Without the guard this
     * assertion passes when the exec fails, because a command that never
     * ran prints none of the forbidden lines either -- a vacuous pass, and
     * the negative-control rig is exactly where a vacuous pass would go
     * unnoticed. It is reported as skipped instead.
     */
    static char out[65536];
    if (access("/bin/DCL.EXE", X_OK) != 0) {
        printf("  (no /bin/DCL.EXE -- the SHOW PROCESS assertion cannot run here)\n");
    } else if (run_dcl("SHOW PROCESS\nLOGOUT\n", -1, NULL, out, sizeof(out)) == 0) {
        CHECK(strstr(out, SETUP_FAIL_MARK) == NULL,
              "DCL.EXE ran for the device-absent SHOW PROCESS check");
        CHECK(strstr(out, "Terminal:") == NULL &&
              strstr(out, "Base priority:") == NULL &&
              strstr(out, "Privileges:") == NULL &&
              strstr(out, "Process quotas:") == NULL,
              "SHOW PROCESS fabricates no process fields with no executive");
    }

    printf("=== test_syssvc_showproc: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
           pass, fail);
    return fail > 0 ? 1 : EXIT_SKIP;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);  /* vms-b5b: line-buffer stdout so a still-buffered write cannot splice into a child process output */
    static char out[65536];
    static char script[512];
    char msg[256];

    printf("=== test_syssvc_showproc: SHOW SYSTEM columns and SHOW PROCESS targeting ===\n");

    int devfd = open("/dev/vms", O_RDWR);
    if (devfd < 0)
        return device_absent_checks();
    close(devfd);

    /* The subject's script. It must block, or the subject exits before it
     * can be observed; `sleep` is a busybox applet installed by init.sh. */
    FILE *hs = fopen(HOLD_SCRIPT, "w");
    if (!hs) {
        printf("  FAIL: cannot write %s\n", HOLD_SCRIPT);
        printf("=== test_syssvc_showproc: 0 passed, 1 failed ===\n");
        return 1;
    }
    fprintf(hs, "sleep 600\n");
    fclose(hs);
    chmod(HOLD_SCRIPT, 0644);

    struct vms_procinfo selfinfo;
    uint32_t st = vms_kif_getjpi_self(&selfinfo);
    /* negctl: bind-client-no-register */
    CHECK(st & 1, "the caller has a row in the executive's process table");

    uint32_t subject = 0;
    st = spawn_named(SUBJECT_NAME, &subject);
    /* negctl-knockon: bind-client-no-register */
    CHECK(st & 1, "sys$creprc created the subject process");
    /* negctl-knockon: bind-client-no-register */
    CHECK(subject != 0, "sys$creprc returned the subject's VMS process ID");
    if (!(st & 1) || subject == 0) {
        printf("=== test_syssvc_showproc: %d passed, %d failed ===\n",
               pass, fail + 1);
        return 1;
    }

    uint32_t subject_lpid = linux_pid_of(subject);
    CHECK(subject_lpid != 0, "the subject's VMS process ID resolves to a row");
    CHECK(wait_for_exec(subject_lpid, "sh") == 0,
          "the subject reached its post-exec image (/bin/sh)");

    /* ---------------------------------------------------------------
     * P1. SHOW SYSTEM's HEADING is the oracle-pinned one, byte for byte.
     *
     *     Section 1.1 counted VMS's heading through `cat -A`; Section 5.1
     *     records which columns OVMX can source and the rule that the
     *     survivors keep VMS's own widths while the rest are removed
     *     whole. A heading that gains a column, loses one, or drifts by a
     *     single space fails here. vms-a7e added the CPU/Page-flts/Pages
     *     survivors; State/Pri/I/O stay removed.
     * --------------------------------------------------------------- */
    CHECK(run_dcl("SHOW SYSTEM\nLOGOUT\n", -1, NULL, out, sizeof(out)) == 0,
          "SHOW SYSTEM ran under the real DCL.EXE");
    CHECK(has_line(out, SYS_HEADING),
          "SHOW SYSTEM printed the oracle-pinned column heading exactly");
    /* The three columns OVMX still cannot source stay ABSENT -- no heading,
     * no placeholder. "State"/"Pri"/"I/O" must not appear; "---" must not
     * appear. Page flts and Pages ARE present now (sourced in the executive,
     * vms-a7e), so their absence is no longer required -- their presence is
     * pinned by SYS_HEADING above and the row geometry below. */
    CHECK(strstr(out, "State") == NULL && strstr(out, "Pri") == NULL &&
          strstr(out, "I/O") == NULL && strstr(out, "---") == NULL,
          "SHOW SYSTEM printed no still-unsourceable column (State/Pri/I/O) "
          "and no placeholder");
    CHECK(strstr(out, "Page flts") != NULL && strstr(out, "Pages") != NULL,
          "SHOW SYSTEM printed the now-sourced Page flts and Pages columns");

    /* ---------------------------------------------------------------
     * P2. THE ROW GEOMETRY, on a process THIS process did not create the
     *     display for and that DCL did not create at all.
     *
     *     The pid is at column 0 (VMS has no leading space there) and the
     *     name field is columns 9-23. Both were counted on the oracle.
     * --------------------------------------------------------------- */
    const char *row = sys_row_for(out, SUBJECT_NAME);
    CHECK(row != NULL,
          "SHOW SYSTEM listed the subject, which DCL did not create");
    if (row) {
        CHECK(pid_on_line(row, 0) == subject,
              "the subject's row carries its pid at column 0");
        CHECK(row[8] == ' ' && row[SYS_COL_NAME] == SUBJECT_NAME[0],
              "the subject's Process Name field starts at column 9");
        /*
         * THE CPU FIELD LANDS AT COLUMN 25, WHICH IS WHAT PINS THE NAME
         * FIELD'S WIDTH.
         *
         * The name field is 15 columns (9-23) and column 24 is the
         * separator, so VMS's 16-column CPU field
         * ("%4d %02d:%02d:%02d.%02d") occupies 25-40 and its "hh:mm:ss.cc"
         * begins at 30.
         *
         * MEASURED, not assumed to be sufficient: the assertion that stood
         * here checked only that column 24 was a space, and a mutation
         * narrowing the name field to %-14s did not trip it -- with an
         * 11-character subject name, column 24 fell inside the CPU field's
         * leading padding and was a space either way. The check now pins
         * the position of the field that MOVES when the one before it
         * changes width, so a one-column drift anywhere left of it fails.
         */
        CHECK(row[25] == ' ' && row[26] == ' ' && row[27] == ' ' &&
              isdigit((unsigned char)row[28]) && row[29] == ' ' &&
              isdigit((unsigned char)row[30]) &&
              isdigit((unsigned char)row[31]) && row[32] == ':' &&
              row[35] == ':' && row[38] == '.',
              "the CPU field starts at column 25, so the Process Name "
              "field is 15 columns wide");
        /*
         * THE NOW-SOURCED ACCOUNTING COLUMNS CARRY REAL DATA (vms-a7e).
         * Page flts is a 10-column right-justified field at 42-51 and Pages
         * a 7-column right-justified field at 53-59 (SYS_HEADING's geometry).
         * The subject is a live /bin/sh: it has faulted its image in and has
         * a resident set, so both fields end in a digit at their rightmost
         * column -- proof SHOW SYSTEM reads a real figure from the executive
         * row, not a blank or a fabricated placeholder. Column 41 (before
         * Page flts) and 52 (before Pages) are the single separators.
         */
        CHECK(row[41] == ' ' && isdigit((unsigned char)row[51]) &&
              row[52] == ' ' && isdigit((unsigned char)row[59]),
              "the subject's Page flts (42-51) and Pages (53-59) fields carry "
              "real digits at VMS geometry -- the executive sourced them");
    }

    /* ---------------------------------------------------------------
     * P3. SHOW PROCESS REPORTS THE TARGET, NOT THE CALLER.
     *
     *     One DCL session runs SHOW PROCESS (self) and then
     *     SHOW PROCESS <SUBJECT>. The two Process ID: values must DIFFER,
     *     and the second must be the subject's. Before this item the
     *     parameter was ignored entirely and both printed the caller.
     * --------------------------------------------------------------- */
    snprintf(script, sizeof(script),
             "SHOW PROCESS\nSHOW PROCESS %s\nLOGOUT\n", SUBJECT_NAME);
    CHECK(run_dcl(script, -1, NULL, out, sizeof(out)) == 0,
          "SHOW PROCESS ran under the real DCL.EXE");

    const char *hdr1 = line_containing(out, "Process ID:");
    const char *nl1  = hdr1 ? strchr(hdr1, '\n') : NULL;
    const char *hdr2 = nl1 ? line_containing(nl1 + 1, "Process ID:") : NULL;
    CHECK(hdr1 && hdr2, "SHOW PROCESS printed a header for both targets");
    if (!hdr1 || !hdr2)
        printf("  (captured output follows)\n%s\n  (end)\n", out);
    if (hdr1 && hdr2) {
        uint32_t self_shown = pid_on_line(hdr1, PROC_COL_VALUE);
        uint32_t tgt_shown  = pid_on_line(hdr2, PROC_COL_VALUE);
        CHECK(tgt_shown == subject,
              "SHOW PROCESS <name> reported the SUBJECT's process ID");
        CHECK(self_shown != 0 && self_shown != tgt_shown,
              "SHOW PROCESS with no target reported a DIFFERENT process");

        /* ------------------------------------------------------------
         * P4. HEADER GEOMETRY, counted on the oracle (Section 2.1):
         *     "User:"/"Node:" at column 26, "Process ID:"/"Process name:"
         *     at column 49, the value at column 63.
         * ------------------------------------------------------------ */
        CHECK(token_at(hdr2, PROC_COL_LEFTLABEL, "User:"),
              "the header's User: label sits at column 26");
        CHECK(token_at(hdr2, PROC_COL_RIGHTLABEL, "Process ID:"),
              "the header's Process ID: label sits at column 49");

        const char *nodeline = strchr(hdr2, '\n');
        if (nodeline) nodeline++;
        CHECK(token_at(nodeline, PROC_COL_LEFTLABEL, "Node:"),
              "the header's second line carries Node: at column 26");
        CHECK(token_at(nodeline, PROC_COL_RIGHTLABEL, "Process name:"),
              "the header's second line carries Process name: at column 49");
        if (nodeline) {
            char want[64];
            snprintf(want, sizeof(want), "\"%s\"", SUBJECT_NAME);
            CHECK(strncmp(nodeline + PROC_COL_VALUE, want, strlen(want)) == 0,
                  "the quoted process name is the SUBJECT's, at column 63");
        }
    }

    /* ---------------------------------------------------------------
     * P5. THE BODY: a 20-column label field, and VMS's OWN "Not
     *     available" literal for a target whose default directory this
     *     process cannot read (Section 3.1). The old code printed a
     *     19-column field and the CALLER's default directory for every
     *     process.
     * --------------------------------------------------------------- */
    {
        const char *uicline = line_starting(out, "User Identifier:");
        CHECK(uicline != NULL, "SHOW PROCESS printed User Identifier:");
        CHECK(uicline && uicline[PROC_LABEL_WIDTH] == '[' &&
              uicline[PROC_LABEL_WIDTH - 1] == ' ',
              "the body label field is 20 columns wide");

        const char *dfs = line_containing(out, "Not available");
        CHECK(dfs != NULL &&
              label_value_at(dfs, "Default file spec:", PROC_LABEL_WIDTH,
                             "Not available"),
              "a target that is not the caller reports Default file spec: "
              "Not available");
    }

    /* ---------------------------------------------------------------
     * P6. NOTHING FABRICATED. Every one of these lines was printed
     *     unconditionally by the deleted code, and none of them is
     *     printed by plain SHOW PROCESS on the oracle (Sections 2.2,
     *     3.1, and vax73-privileges.md Section 6 (1)).
     * --------------------------------------------------------------- */
    CHECK(strstr(out, "Terminal:") == NULL,
          "SHOW PROCESS prints no Terminal: line (VMS's value is not "
          "sourceable, and the env-var one is a facade)");
    CHECK(strstr(out, "Base priority:") == NULL,
          "SHOW PROCESS prints no invented Base priority:");
    CHECK(strstr(out, "Privileges:") == NULL,
          "SHOW PROCESS prints no Privileges: line -- VMS has none");
    CHECK(strstr(out, "Process quotas:") == NULL,
          "SHOW PROCESS prints no fabricated quota block");
    CHECK(strstr(out, "_FTA0:") == NULL,
          "SHOW PROCESS invents no _FTA0: process/terminal name");

    /* ---------------------------------------------------------------
     * P7. SELECTING BY PROCESS ID reaches the same row, and /ID=0 is the
     *     caller (Sections 3.1 and 3.2.1).
     * --------------------------------------------------------------- */
    snprintf(script, sizeof(script),
             "SHOW PROCESS/ID=%08X\nLOGOUT\n", subject);
    CHECK(run_dcl(script, -1, NULL, out, sizeof(out)) == 0,
          "SHOW PROCESS/ID ran under the real DCL.EXE");
    {
        const char *h = line_containing(out, "Process ID:");
        CHECK(h && pid_on_line(h, PROC_COL_VALUE) == subject,
              "SHOW PROCESS/ID=<pid> reported that pid's process");
    }

    /* ---------------------------------------------------------------
     * P8. A NAME THAT NAMES NOTHING -> the oracle's message, exactly.
     * --------------------------------------------------------------- */
    snprintf(script, sizeof(script),
             "SHOW PROCESS %s\nLOGOUT\n", ABSENT_NAME);
    CHECK(run_dcl(script, -1, NULL, out, sizeof(out)) == 0,
          "SHOW PROCESS <absent name> ran under the real DCL.EXE");
    /* negctl-knockon: proctab-getjpi-nonexpr-status-wrong */
    CHECK(has_line(out, MSG_NONEXPR),
          "an absent process name reports %SYSTEM-W-NONEXPR verbatim");
    CHECK(line_containing(out, "Process ID:") == NULL,
          "an absent process name prints no process header");

    /* ---------------------------------------------------------------
     * P9/P10. THE TWO REFUSALS, WHICH ARE DIFFERENT ANSWERS FOR THE SAME
     *     PROCESS -- measured on the oracle with the caller holding no
     *     privileges (Section 3.3):
     *
     *       by NAME, out of group -> %SYSTEM-W-NONEXPR (the name search
     *                                is group-scoped: not found at all)
     *       by PID,  out of group -> %SYSTEM-F-NOPRIV
     *
     *     Both are run by a DCL.EXE that setgid()s into its own UIC group
     *     and drops WORLD from its executive-held mask before exec, so
     *     the refusal comes from the executive and not from anything the
     *     test arranged in userspace.
     *
     *     EACH SELECTOR GETS ITS OWN DCL SESSION, and each asserts the
     *     message it must produce AND the message it must not. The point
     *     of this block is that the two selectors give DIFFERENT answers
     *     for the SAME process, so an assertion that merely asks whether
     *     each message appears SOMEWHERE cannot see it: run both commands
     *     in one session and an implementation with the two messages
     *     INVERTED emits the same set of lines and stays green. That was
     *     the shape here through vms-6a7 round 1 -- the product was
     *     measured correct, but the assertion could not tell. Anchoring
     *     each message to its own command's response is what makes the
     *     oracle's asymmetry falsifiable.
     * --------------------------------------------------------------- */
    {
        int cmdp[2], repp[2];
        struct helper_report rep;

        if (pipe(cmdp) != 0 || pipe(repp) != 0) {
            CHECK(0, "pipes for the cross-group helper");
        } else {
            pid_t hp = fork();
            if (hp == 0) {
                close(cmdp[1]); close(repp[0]);
                xgrp_helper(cmdp[0], repp[1]);
                _exit(0);
            }
            close(cmdp[0]); close(repp[1]);

            memset(&rep, 0, sizeof(rep));
            int got = read_full(repp[0], &rep, sizeof(rep));
            CHECK(got == 0 && rep.ok && rep.vms_pid != 0,
                  "a named process exists in another UIC group");

            if (got == 0 && rep.ok && rep.vms_pid != 0) {
                /* P9. BY PID -> %SYSTEM-F-NOPRIV, in its own session. */
                snprintf(script, sizeof(script),
                         "SHOW PROCESS/ID=%08X\nLOGOUT\n", rep.vms_pid);
                CHECK(run_dcl(script, DCL_UIC_GROUP, DCLGRP_NAME,
                              out, sizeof(out)) == 0,
                      "an unprivileged DCL.EXE ran SHOW PROCESS/ID in its "
                      "own UIC group");
                CHECK(strstr(out, SETUP_FAIL_MARK) == NULL,
                      "the unprivileged by-PID caller was set up as intended");
                /* negctl: proctab-crossgroup-identity */
                CHECK(has_line(out, MSG_NOPRIV),
                      "SHOW PROCESS/ID on an out-of-group process reports "
                      "%SYSTEM-F-NOPRIV verbatim");
                CHECK(!has_line(out, MSG_NONEXPR),
                      "... and NOT %SYSTEM-W-NONEXPR: selected by PID the "
                      "process IS found, and it is the identity read that "
                      "is refused");
                /* negctl-knockon: proctab-crossgroup-identity */
                CHECK(line_containing(out, "Process ID:") == NULL,
                      "the by-PID refusal printed no process header");

                /* P10. BY NAME -> %SYSTEM-W-NONEXPR, in a SEPARATE session
                 *      so the message is anchored to this command alone. */
                snprintf(script, sizeof(script),
                         "SHOW PROCESS %s\nLOGOUT\n", XGRP_NAME);
                CHECK(run_dcl(script, DCL_UIC_GROUP, DCLGRP_NAME,
                              out, sizeof(out)) == 0,
                      "an unprivileged DCL.EXE ran SHOW PROCESS <name> in "
                      "its own UIC group");
                CHECK(strstr(out, SETUP_FAIL_MARK) == NULL,
                      "the unprivileged by-name caller was set up as intended");
                /* negctl-knockon: proctab-getjpi-nonexpr-status-wrong */
                CHECK(has_line(out, MSG_NONEXPR),
                      "SHOW PROCESS <name> on an out-of-group process reports "
                      "%SYSTEM-W-NONEXPR -- the name search is group-scoped");
                /* negctl-knockon: proctab-getjpi-nonexpr-status-wrong */
                CHECK(!has_line(out, MSG_NOPRIV),
                      "... and NOT %SYSTEM-F-NOPRIV: the group-scoped name "
                      "search never reaches a process to be refused");
                CHECK(line_containing(out, "Process ID:") == NULL,
                      "the by-name refusal printed no process header");

                /* Enumeration is NOT privileged, identity is (oracle-
                 * pinned, vax73-privileges.md Section 5.5): the same
                 * caller that was just refused BOTH identity reads must
                 * still see that process listed, by name, in SHOW SYSTEM. */
                CHECK(run_dcl("SHOW SYSTEM\nLOGOUT\n", DCL_UIC_GROUP,
                              NULL, out, sizeof(out)) == 0,
                      "the unprivileged caller ran SHOW SYSTEM");
                CHECK(sys_row_for(out, XGRP_NAME) != NULL,
                      "SHOW SYSTEM still lists the row whose identity the "
                      "caller may not read");
            }

            close(cmdp[1]);          /* release the helper */
            close(repp[0]);
            int wst;
            while (waitpid(hp, &wst, 0) < 0 && errno == EINTR)
                ;
        }
    }

    /* Release the subject. */
    if (subject_lpid != 0) {
        kill((pid_t)subject_lpid, SIGKILL);
        int wst;
        while (waitpid((pid_t)subject_lpid, &wst, 0) < 0 && errno == EINTR)
            ;
    }

    (void)msg;
    printf("=== test_syssvc_showproc: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
