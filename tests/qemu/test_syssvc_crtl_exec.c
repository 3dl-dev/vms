/*
 * test_syssvc_crtl_exec.c - the DEC C RTL's exec*()/wait()/getpid() create and
 * collect a VMS SUBPROCESS through the executive (rd vms-e70f), proven against
 * a real /dev/vms.
 *
 * THE BUG. DECC$SHR's exec*() in a vfork context forked the task and execve()d
 * the image; wait()/waitpid() were the substrate's wait4 and getpid() its task
 * id. The "subprocess" had a VMS PID only as a side record, the wait status was
 * the Linux one, and nothing of the child's VMS completion reached its creator.
 *
 * THE PROPERTY (src/vmsrms/crtl_exec.c, the code DECC$SHR's exec/wait/getpid
 * entry points run): a creator, its streams redirected the way a DEC C program
 * redirects them between vfork() and exec*(), creates this binary as a
 * subprocess with arguments and an environment.
 *
 *   A  the subprocess got exactly the arguments and environment passed, through
 *      the C RTL's exec mailbox; its getpid() is the VMS PID the creator got
 *      back, and $GETJPI of that PID answers while it lives. It exits with the
 *      ERROR completion %X1000002C; the _VMS_WAIT wait returns that code.
 *   B  a subprocess ending with __posix_exit(3)'s completion (%X0035A019) gives
 *      the POSIX wait status WEXITSTATUS 3.
 *   C  a subprocess ending SS$_NORMAL gives status 0, through wait() for any.
 *   D  with no subprocess left, wait() fails ECHILD.
 *   E  the creator's own getpid() is its VMS PID ($GETJPI JPI$_PID).
 *   F  an IMAGE TASK (CONTINUE: DCL's RUN shape, sharing its creator's VMS PID)
 *      that execs a subprocess is the one told how it ended: its completion
 *      and termination record go to the task that created it, not to another
 *      row holding the same VMS PID.
 *   G  a subprocess whose creating task ends BEFORE its image task (the image
 *      task shares the subprocess's VMS PID) is collected once, when the whole
 *      process is gone, with the image's completion -- not SS$_ABORT from the
 *      row that ended first with nothing recorded (rd vms-ff24).
 *
 * NEGATIVE CONTROL: tests/qemu/defects.d/crtl-exec-substrate-fork.defect puts
 * the substrate fork/execve/wait back; A's $GETJPI, getpid and completion code
 * and B's status go red.
 *
 * Requires a real, insmod'd vms.ko; without one it SKIPs (77), never a fake pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <libgen.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "ssdef.h"
#include "descrip.h"
#include "prcdef.h"
#include "lnmdef.h"
#include "starlet.h"
#include "vms_kif.h"
#include "vms/pcb.h"
#include "ovmx_test_fork.h"
#include "crtl_exec.h"

#define EXIT_SKIP 77
#define DIR_      "/tmp/ovmx_e70f"
#define CHILD_ERR 0x1000002Cu           /* SS$_ABORT, message inhibited */
#define POSIX3    0x0035A019u           /* __posix_exit(3), measured on Alpha V8.4 */
#define IMAGE_ST  0x10000014u           /* %X10000014: an error, message inhibited */

static int pass, fail;
#define CHECK(c, m) do { if (c) { printf("  PASS: %s\n", m); pass++; } \
                         else   { printf("  FAIL: %s\n", m); fail++; } } while (0)

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

static uint32_t jpi_pid(uint32_t *target)
{
    uint32_t pid = 0;
    uint16_t len = 0;
    struct item_list_3 il[2];
    memset(il, 0, sizeof il);
    il[0].buflen = 4;
    il[0].item_code = JPI$_PID;
    il[0].bufaddr = &pid;
    il[0].retlen = &len;
    uint32_t st = sys$getjpiw(0, target, NULL, il, NULL, NULL, 0);
    return (st & 1) ? pid : 0;
}

/* SUBJECT MODE (argv[0] = DIR_/<role>): take the exec context, report, end. */
static int subject_main(const char *role)
{
    (void)vms_pcb_init(0xFFFFFFFFFFFFFFFFULL);
    int xc = 0;
    char **xa = NULL, **xe = NULL;
    int have = ovmx_crtl_exec_context(&xc, &xa, &xe);
    if (strcmp(role, "ARGS") == 0) {
        int ok = have && xc == 3 && strcmp(xa[0], "child") == 0 &&
                 strcmp(xa[1], "alpha") == 0 && strcmp(xa[2], "42 with space") == 0 &&
                 xe[0] && strcmp(xe[0], "OVMX_E70F=probe") == 0 && xe[1] == NULL;
        printf("%s %08X\n", ok ? "ARGS-OK" : "ARGS-BAD", (unsigned)ovmx_crtl_getpid());
        fflush(stdout);
        char go = 0;
        (void)read(0, &go, 1);           /* held until the creator has looked */
        (void)sys$exit(CHILD_ERR);
    } else if (strcmp(role, "POSIX3") == 0) {
        (void)sys$exit(POSIX3);
    } else if (strcmp(role, "TWOROW") == 0) {
        /* The image runs in a task of this process (CONTINUE: the same VMS
         * PID) and outlives the creating task, which ends recording nothing. */
        pid_t t = ovmx_fork_kind_st(VMS_CREPRC_PCB_CONTINUE, NULL);
        if (t == 0) {
            usleep(300000);
            (void)sys$exit(IMAGE_ST);
        }
        _exit(0);
    }
    (void)sys$exit(SS$_NORMAL);
    return 0;
}

static int creator_main(void)
{
    (void)vms_pcb_init(0xFFFFFFFFFFFFFFFFULL);
    uint32_t me = (uint32_t)ovmx_crtl_getpid();
    CHECK(me != 0 && me == jpi_pid(NULL),
          "E: the creator's getpid() is its VMS PID ($GETJPI JPI$_PID)");

    /* A: streams redirected "in the child", as between vfork() and exec*(). */
    int out[2], in[2];
    if (pipe(out) != 0 || pipe(in) != 0) {
        printf("  FAIL: pipe\n");
        return 1;
    }
    int save0 = dup(0), save1 = dup(1);
    dup2(out[1], 1);
    dup2(in[0], 0);
    char *args[] = { "child", "alpha", "42 with space" };
    char *env[] = { "OVMX_E70F=probe", NULL };
    uint32_t pid = ovmx_crtl_exec_create(DIR_ "/ARGS", args, 3, env);
    int e = errno;
    dup2(save0, 0);
    dup2(save1, 1);
    close(save0); close(save1); close(out[1]); close(in[0]);
    CHECK(pid != 0, "A: exec created the subprocess");
    if (!pid)
        printf("  INFO: errno %d\n", e);
    char line[64] = "";
    int n = 0, r;
    while (n < 16 && (r = (int)read(out[0], line + n, 16 - n)) > 0)
        n += r;
    line[n] = 0;
    uint32_t target = pid;
    /* negctl: crtl-exec-substrate-fork */
    CHECK(pid && jpi_pid(&target) == pid,
          "A: $GETJPI answers the subprocess's VMS PID while it lives");
    CHECK(strncmp(line, "ARGS-OK ", 8) == 0,
          "A: the subprocess got exactly the arguments and environment passed");
    CHECK(n == 16 && strtoul(line + 8, NULL, 16) == pid,
          "A: the subprocess's getpid() is the VMS PID exec returned");
    (void)write(in[1], "g", 1);
    close(in[1]);
    close(out[0]);
    int st = 0;
    int w = pid ? ovmx_crtl_vms_waitpid((int)pid, &st, 0) : -1;
    CHECK(w == (int)pid && (uint32_t)st == CHILD_ERR,
          "A: the _VMS_WAIT wait returns the subprocess's error completion %X1000002C");
    printf("  INFO: wait=%d status=%08X\n", w, (unsigned)st);

    /* B */
    pid = ovmx_crtl_exec_create(DIR_ "/POSIX3", args, 0, env);
    st = -1;
    w = pid ? ovmx_crtl_waitpid((int)pid, &st, 0) : -1;
    CHECK(w == (int)pid && WIFEXITED(st) && WEXITSTATUS(st) == 3,
          "B: a C-facility completion (__posix_exit(3)) waits as WEXITSTATUS 3");

    /* C */
    pid = ovmx_crtl_exec_create(DIR_ "/OK", args, 0, env);
    st = -1;
    w = pid ? ovmx_crtl_wait(&st) : -1;
    CHECK(w == (int)pid && st == 0, "C: an SS$_NORMAL completion waits as status 0 (wait for any)");

    /* G */
    pid = ovmx_crtl_exec_create(DIR_ "/TWOROW", args, 0, env);
    /* Collect only once the WHOLE process is gone (both rows), so the answer
     * is the termination record the executive posted, not a live row's. */
    for (int i = 0; pid && i < 400; i++) {
        target = pid;
        if (jpi_pid(&target) == 0)
            break;
        usleep(50000);
    }
    st = -1;
    w = pid ? ovmx_crtl_vms_waitpid((int)pid, &st, 0) : -1;
    printf("  INFO: G wait=%d status=%08X\n", w, (unsigned)st);
    /* negctl: process-ends-with-first-row */
    CHECK(w == (int)pid && (uint32_t)st == IMAGE_ST,
          "G: a subprocess whose creating task ends first is collected with its image task's completion");

    /* D */
    errno = 0;
    w = ovmx_crtl_wait(&st);
    CHECK(w == -1 && errno == ECHILD, "D: with no subprocess left, wait() fails ECHILD");
    return fail ? 1 : 0;
}

/* F: the image task creates and waits; bounded, so a lost completion is a
 * FAIL, not a hang. */
static int image_task_main(void)
{
    (void)vms_pcb_init(0xFFFFFFFFFFFFFFFFULL);
    char *args[] = { NULL };
    char *env[] = { NULL };
    uint32_t pid = ovmx_crtl_exec_create(DIR_ "/POSIX3", args, 0, env);
    if (!pid)
        return 2;
    for (int i = 0; i < 400; i++) {
        uint32_t cond = 0;
        int r = ovmx_crtl_wait_cond((int)pid, &cond, 1);
        if (r == (int)pid)
            return cond == POSIX3 ? 0 : 3;
        if (r < 0)
            return 4;
        usleep(50000);
    }
    return 5;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc >= 1 && strncmp(argv[0], DIR_ "/", strlen(DIR_) + 1) == 0)
        return subject_main(basename(argv[0]));

    printf("=== test_syssvc_crtl_exec (DEC C exec*/wait/getpid over $CREPRC and the executive, rd vms-e70f) ===\n");
    if (!executive_present()) {
        printf("=== test_syssvc_crtl_exec: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }
    char self[512];
    ssize_t sl = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (sl <= 0) {
        printf("  FAIL: cannot resolve this test's own image\n");
        return 1;
    }
    self[sl] = '\0';
    mkdir(DIR_, 0777);
    int staged = 1;
    const char *roles[] = { "ARGS", "POSIX3", "OK", "TWOROW" };
    for (int i = 0; i < 4; i++) {
        char p[128];
        snprintf(p, sizeof p, DIR_ "/%s", roles[i]);
        unlink(p);
        if (symlink(self, p) != 0) staged = 0;
    }
    CHECK(staged, "staged the subject images (this binary, in subject mode)");

    /* The creator is a VMS process the executive created; its result comes
     * back as its exit code and its CHECK lines on this stdout. */
    pid_t c = ovmx_fork_subprocess();
    if (c == 0)
        _exit(creator_main());
    int ws = 0;
    waitpid(c, &ws, 0);
    CHECK(c > 0 && WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "the creator's checks all held");

    uint32_t kst = 0;
    pid_t t = ovmx_fork_kind_st(VMS_CREPRC_PCB_CONTINUE, &kst);
    if (t == 0)
        _exit(image_task_main());
    ws = 0;
    if (t > 0)
        waitpid(t, &ws, 0);
    printf("  INFO: image task rc=%d (creprc_pcb %08X)\n", WIFEXITED(ws) ? WEXITSTATUS(ws) : -1, (unsigned)kst);
    CHECK(t > 0 && WIFEXITED(ws) && WEXITSTATUS(ws) == 0,
          "F: an image task sharing its creator's VMS PID gets its own subprocess's completion %X0035A019");
    printf("=== test_syssvc_crtl_exec: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
