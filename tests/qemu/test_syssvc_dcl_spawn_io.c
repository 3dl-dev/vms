/*
 * test_syssvc_dcl_spawn_io.c - SPAWN /INPUT and /OUTPUT name the subprocess's
 * SYS$INPUT and SYS$OUTPUT, as on VMS (rd vms-691d).
 *
 * THE BUG (OVMX V0.7-14, seen in the vms-1b68 lab re-fire):
 *   - SPAWN/NOWAIT/INPUT=NL:/OUTPUT=X.LOG ... answered %DCL-W-IVQUAL \INPUT\;
 *   - SPAWN/NOWAIT/OUTPUT=SYS$LOGIN:ARMA.LOG @ARMA.COM spawned, but the
 *     subprocess wrote on the creator's terminal and ARMA.LOG never existed:
 *     lib$spawn translated the VMS file spec to a host path of the retired /vms
 *     passthrough, $CREPRC could not open it, and a subprocess kept its
 *     creator's stream.
 *
 * WHAT VMS DOES (OpenVMS VAX V7.3, tests/lab/captures/vms-691d-type-spawn-
 * 20261102/console.log; DCL Dictionary, SPAWN):
 *   - SPAWN/INPUT=NL:/OUTPUT=S1.LOG SHOW SYMBOL TCX  -> S1.LOG holds the output;
 *   - SPAWN/NOWAIT/OUTPUT=S3.LOG @TC.COM             -> S3.LOG holds TC's output;
 *   - SPAWN/OUTPUT=S2.LOG FOOBAR                     -> S2.LOG holds
 *     "%DCL-W-IVVERB ..." and " \FOOBAR\" (the subprocess's SYS$ERROR is the file);
 *   - SPAWN/INPUT=IN.COM/OUTPUT=S5.LOG WRITE SYS$OUTPUT "from CMD"
 *                                                    -> "from CMD", then IN.COM's output;
 *   - SPAWN/INPUT=NOSUCH.COM SHOW TIME               -> "%RMS-E-FNF, file not found",
 *     $STATUS %X00018292, no process.
 *
 * Every file here is made and read through RMS on the ODS-2 volume VDA0: --
 * never a host path. Requires a real, insmod'd vms.ko; without one it SKIPs
 * (77), never a fake pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <sys/wait.h>

#include "starlet.h"
#include "ssdef.h"
#include "rms/rms.h"
#include "rms_textfile.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"

#define EXIT_SKIP 77
#define DCL_PATH  "/bin/DCL.EXE"
#define RW_UNIT   "VDA0:"
#define DIR       RW_UNIT "[OVMXDIR]"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static char *const dcl_env[] = { (char *)"PATH=/bin", NULL };

static int read_file(const char *spec, char *buf, size_t bufsz);

/* Run DCL.EXE (a login process) over `script`; its terminal output -> out.
 * When `hold_spec` is given, DCL's input stays open -- the creator stays logged
 * in, as a /NOWAIT subprocess needs (VMS deletes a subprocess with its creator)
 * -- until that file holds `hold_text` or ~30 s pass. */
static int run_dcl_hold(const char *script, char *out, size_t outsz,
                        const char *hold_spec, const char *hold_text)
{
    int in_pipe[2], out_pipe[2];
    out[0] = '\0';
    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) return -1;
    fflush(NULL);
    pid_t pid = ovmx_fork_login();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execle(DCL_PATH, "DCL.EXE", (char *)NULL, dcl_env);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    if (write(in_pipe[1], script, strlen(script)) < 0) { /* best effort */ }
    if (hold_spec) {
        static char held[8192];
        for (int w = 0; w < 300; w++) {
            if (read_file(hold_spec, held, sizeof held) && strstr(held, hold_text))
                break;
            struct timespec ts = { 0, 100 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
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

/* The records of `spec`, read through RMS, one per line, into buf. 0 when the
 * file cannot be opened. */
static int read_file(const char *spec, char *buf, size_t bufsz)
{
    buf[0] = '\0';
    rms_textfile_t *tf = rms_textfile_open(spec);
    if (!tf)
        return 0;
    char rec[1024];
    int too_long = 0;
    size_t used = 0;
    while (rms_textfile_getline(tf, rec, sizeof rec - 1, &too_long)) {
        size_t n = strlen(rec);
        if (n == 0 || rec[n - 1] != '\n')
            rec[n++] = '\n', rec[n] = '\0';
        if (used + n + 1 > bufsz)
            break;
        memcpy(buf + used, rec, n + 1);
        used += n;
    }
    rms_textfile_close(tf);
    return 1;
}

static void erase(const char *spec)
{
    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    (void)sys$erase(&fab, 0, 0);
}

static void erase_all(const char *spec)
{
    char s[128];
    snprintf(s, sizeof s, "%s;*", spec);
    for (int i = 0; i < 8; i++)
        erase(s);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_dcl_spawn_io (SPAWN /INPUT /OUTPUT: the subprocess's SYS$INPUT/SYS$OUTPUT) ===\n");

    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_spawn_io: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }
    CHECK($VMS_STATUS_SUCCESS(vms_kif_acp_mount(RW_UNIT)), "MOUNT " RW_UNIT " (writable)");

    unsigned id = (unsigned)getpid() % 100000u;
    char a_log[64], b_log[64], e_log[64], f_log[64], c_com[64], in_com[64];
    snprintf(a_log, sizeof a_log, DIR "SPA%u.LOG", id);
    snprintf(b_log, sizeof b_log, DIR "SPB%u.LOG", id);
    snprintf(e_log, sizeof e_log, DIR "SPE%u.LOG", id);
    snprintf(f_log, sizeof f_log, DIR "SPF%u.LOG", id);
    snprintf(c_com, sizeof c_com, DIR "SPC%u.COM", id);
    snprintf(in_com, sizeof in_com, DIR "SPI%u.COM", id);

    CHECK(rms_textfile_write_line(c_com, "$ WRITE SYS$OUTPUT \"SPAWN_NOWAIT_COM_RAN\"") == 0,
          "a command procedure is laid down on the ODS-2 volume through RMS");
    CHECK(rms_textfile_write_line(in_com, "$ WRITE SYS$OUTPUT \"SPAWN_INPUT_FILE_RAN\"") == 0,
          "an input file is laid down on the ODS-2 volume through RMS");

    static char script[2048], out[32768], body[8192];
    snprintf(script, sizeof script,
             "SET DEFAULT " DIR "\n"
             "SPAWN/INPUT=NL:/OUTPUT=SPA%u.LOG WRITE SYS$OUTPUT \"SPAWN_OUTPUT_TO_FILE\"\n"
             "WRITE SYS$OUTPUT \"WAIT_STATUS=\",$STATUS\n"
             "SPAWN/OUTPUT=SPE%u.LOG FOOBARNOVERB\n"
             "SPAWN/INPUT=SPI%u.COM/OUTPUT=SPF%u.LOG WRITE SYS$OUTPUT \"SPAWN_CMD_FIRST\"\n"
             "SPAWN/INPUT=NOSUCH%u.COM WRITE SYS$OUTPUT \"NEVER\"\n"
             "WRITE SYS$OUTPUT \"FNF_STATUS=\",$STATUS\n"
             "SPAWN/NOWAIT/INPUT=NL:/OUTPUT=SPB%u.LOG @SPC%u.COM\n",
             id, id, id, id, id, id, id);
    CHECK(run_dcl_hold(script, out, sizeof out, b_log, "SPAWN_NOWAIT_COM_RAN") == 0,
          "DCL ran the SPAWN script");
    printf("  ---- DCL ----\n%s  ---- end ----\n", out);

    /* (1) /INPUT is a SPAWN qualifier */
    CHECK(strstr(out, "IVQUAL") == NULL, "SPAWN accepts /INPUT (no %DCL-W-IVQUAL)");

    /* (2) /OUTPUT=file: the subprocess's SYS$OUTPUT is that file */
    int have = read_file(a_log, body, sizeof body);
    printf("  ---- %s ----\n%s  ---- end ----\n", a_log, body);
    /* negctl: creprc-subprocess-output-file-on-creator-terminal */
    CHECK(have && strstr(body, "SPAWN_OUTPUT_TO_FILE") != NULL,
          "SPAWN/OUTPUT=file: the subprocess's output is a record of that file (read through RMS)");
    CHECK(strstr(out, "SPAWN_OUTPUT_TO_FILE") == NULL,
          "...and nothing of it reached the creator's terminal");

    /* (3) the subprocess's error messages go to the file too */
    have = read_file(e_log, body, sizeof body);
    printf("  ---- %s ----\n%s  ---- end ----\n", e_log, body);
    CHECK(have && strstr(body, "%DCL-W-IVVERB") != NULL && strstr(body, "\\FOOBARNOVERB\\") != NULL,
          "SPAWN/OUTPUT=file FOOBAR: the subprocess's %DCL-W-IVVERB and \\FOOBAR\\ are in the file");
    CHECK(strstr(out, "\\FOOBARNOVERB\\") == NULL,
          "...not on the creator's terminal");

    /* (4) a command string and /INPUT: the command, then the input file */
    have = read_file(f_log, body, sizeof body);
    printf("  ---- %s ----\n%s  ---- end ----\n", f_log, body);
    {
        const char *c = strstr(body, "SPAWN_CMD_FIRST");
        const char *i = strstr(body, "SPAWN_INPUT_FILE_RAN");
        CHECK(have && c && i && c < i,
              "SPAWN/INPUT=file cmd: the command ran, then the input file's commands");
    }

    /* (5) a missing /INPUT file: RMS's status, no process */
    CHECK(strstr(out, "%RMS-E-FNF, file not found") != NULL,
          "SPAWN/INPUT=<missing> says %RMS-E-FNF, file not found");
    CHECK(strstr(out, "FNF_STATUS=%X00018292") != NULL,
          "...and leaves $STATUS %X00018292 (RMS$_FNF), as VAX V7.3 does");
    CHECK(strstr(out, "NEVER") == NULL, "...and ran nothing");

    /* (6) /NOWAIT/OUTPUT=file @procedure: the procedure's output is the file's */
    CHECK(strstr(out, "%DCL-S-SPAWNED, process ") != NULL, "SPAWN/NOWAIT reported %DCL-S-SPAWNED");
    have = read_file(b_log, body, sizeof body);
    printf("  ---- %s ----\n%s  ---- end ----\n", b_log, body);
    CHECK(have && strstr(body, "SPAWN_NOWAIT_COM_RAN") != NULL,
          "SPAWN/NOWAIT/OUTPUT=file @X.COM: the procedure's output is in the file");
    CHECK(strstr(out, "SPAWN_NOWAIT_COM_RAN") == NULL,
          "...not on the creator's terminal");

    erase_all(a_log); erase_all(b_log); erase_all(e_log); erase_all(f_log);
    erase_all(c_com); erase_all(in_com);
    (void)vms_kif_acp_dmount(RW_UNIT);
    printf("=== test_syssvc_dcl_spawn_io: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
