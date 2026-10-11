/*
 * test_syssvc_image_privs.c - an image installed /PRIVILEGED runs with its
 * installed privileges, granted by the executive at activation and gone with
 * the image (rd vms-bdc1), against a real /dev/vms.
 *
 * On VMS an installed privileged image adds its privileges to the process's
 * current privileges while it runs, and image rundown returns CURPRIV to the
 * process privileges (INSTALL /PRIVILEGED; observed on the lab VAX V7.3 and
 * Alpha V8.4, tests/lab/captures/install-priv-20261009/). OVMX decides it in
 * the executive, at exec, from the file the kernel actually exec'd: the
 * executive's own pinned copy of the installed image (rd vms-220). Nothing the
 * process says counts, and no other file -- the source it was installed from,
 * a new file that reuses a removed copy's inode -- gets anything.
 *
 * The test installs a copy of ITSELF /PRIVILEGED=(SYSNAM,OPER) and runs it as
 * an unprivileged [100,100] process (default privileges TMPMBX, NETMBX only):
 *   --image <src>   runs as the installed image: must hold SYSNAM+OPER, then
 *                   execs <src> --after (an ordinary image, the next exec)
 *   --after         must NOT hold them any more (the previous image's went)
 *   --plain         an image that is not the executive's copy: must not
 * Exit codes: 0 as expected, 10 grant missing, 11 not dropped at the next
 * exec, 12 granted to a file that is not the installed copy, 2/3/4 setup.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "ssdef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77
#define UNPRIV_GID 100
#define UNPRIV_UID 100
#define IMG_NAME "TEST_SYSSVC_IMAGE_PRIVS.EXE"
#define IMG_PRIVS (VMS_PRV_M_SYSNAM | VMS_PRV_M_OPER)
#define REUSE "/tmp/test_syssvc_image_privs.reuse"

static int passed, failed;
static void check(int c, const char *m)
{
    if (c) { passed++; printf("  PASS: %s\n", m); }
    else   { failed++; printf("  FAIL: %s\n", m); }
}

static uint64_t my_cur_privs(void)
{
    struct vms_procinfo info;
    memset(&info, 0, sizeof info);
    if (!(vms_kif_getjpi_self(&info) & 1))
        return ~0ULL;               /* unreadable: fails every expectation */
    return info.cur_privs;
}

static int copy_file(const char *from, const char *to)
{
    char buf[65536];
    int in = open(from, O_RDONLY), out = open(to, O_CREAT | O_TRUNC | O_WRONLY, 0755);
    ssize_t n;
    int ok = in >= 0 && out >= 0;
    while (ok && (n = read(in, buf, sizeof buf)) > 0)
        ok = write(out, buf, (size_t)n) == n;
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    return ok;
}

/* Run `path mode [arg]` as [100,100] and return its exit code. */
static int run_unpriv(const char *path, const char *mode, const char *arg)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (setgid(UNPRIV_GID) != 0 || setuid(UNPRIV_UID) != 0)
            _exit(3);
        /* Registered before the exec, as DCL's activation child is: the
         * executive decides at the exec. */
        if (vms_kif_open() < 0 || my_cur_privs() & IMG_PRIVS)
            _exit(2);
        execl(path, path, mode, arg, (char *)NULL);
        _exit(4);
    }
    int ws = 0;
    if (pid < 0 || waitpid(pid, &ws, 0) != pid || !WIFEXITED(ws))
        return -1;
    return WEXITSTATUS(ws);
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "--image") == 0) {
        if ((my_cur_privs() & IMG_PRIVS) != IMG_PRIVS)
            return 10;
        execl(argv[2], argv[2], "--after", (char *)NULL);
        return 4;
    }
    if (argc >= 2 && strcmp(argv[1], "--after") == 0)
        return (my_cur_privs() & IMG_PRIVS) ? 11 : 0;
    if (argc >= 2 && strcmp(argv[1], "--plain") == 0)
        return (my_cur_privs() & IMG_PRIVS) ? 12 : 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_image_privs: installed-image privileges at activation ===\n");
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_image_privs: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }
    (void)mkdir("/run", 0755);
    (void)mkdir("/run/ovmx-boot", 0755);
    (void)mkdir(VMS_KFE_DIR, 0711);

    /* The source: a copy of this program the unprivileged child may run. */
    char src[256];
    snprintf(src, sizeof src, "/tmp/test_syssvc_image_privs.%d", (int)getpid());
    check(copy_file(argv[0], src), "a source image (a copy of this test) is written");

    struct vms_kfe_args a;
    memset(&a, 0, sizeof a);
    a.op = VMS_KFE_OP_ADD;
    a.fd = open(src, O_RDONLY);
    a.privs = IMG_PRIVS;
    a.flags = VMS_KFE_F_PRIV;
    snprintf(a.name, sizeof a.name, "%s", IMG_NAME);
    uint32_t st = vms_kif_kfe(&a);
    close(a.fd);
    check(st == SS$_NORMAL, "INSTALL ADD /PRIVILEGED=(SYSNAM,OPER) is SS$_NORMAL");
    char copy[256];
    snprintf(copy, sizeof copy, "%s", a.path);

    int rc = run_unpriv(copy, "--image", src);
    printf("  (installed image run: exit %d)\n", rc);
    /* negctl: image-privs-not-granted */
    check(rc != 10 && rc >= 0,
          "the installed image runs with its installed privileges (SYSNAM, OPER) in an unprivileged process");
    /* negctl: image-privs-not-dropped */
    check(rc == 0,
          "the next image activated in that process no longer holds them");

    rc = run_unpriv(src, "--plain", NULL);
    /* negctl: image-privs-any-file */
    check(rc == 0,
          "the file the image was installed from is not the installed image: no privileges");

    memset(&a, 0, sizeof a);
    a.op = VMS_KFE_OP_REMOVE;
    a.fd = -1;
    snprintf(a.name, sizeof a.name, "%s", IMG_NAME);
    check(vms_kif_kfe(&a) == SS$_NORMAL, "INSTALL REMOVE deletes the entry and its copy");
    /* A new file, very likely on the removed copy's inode number. */
    check(copy_file(src, REUSE), "a new file is written after the copy is gone");
    rc = run_unpriv(REUSE, "--plain", NULL);
    check(rc == 0,
          "a new file (possibly reusing the removed copy's inode) gets no privileges");
    rc = run_unpriv(copy, "--plain", NULL);
    check(rc == 4, "the removed copy cannot be run at all");

    unlink(REUSE);
    unlink(src);
    printf("=== test_syssvc_image_privs: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
