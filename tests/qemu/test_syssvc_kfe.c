/*
 * test_syssvc_kfe.c - the executive's known-file list (INSTALL, rd vms-7c64 /
 * vms-220) against a real /dev/vms.
 *
 * As on VMS (tests/lab/captures/install-priv-20261009/: INSTALL REPLACE/REMOVE
 * without CMKRNL fail %SYSTEM-F-NOCMKRNL): a CMKRNL process adds an entry for
 * a file it holds open, LIST and FIND read it back with its privileges, the
 * entry is keyed on the file itself (a second ADD of the same file is a
 * duplicate, another file is not found), REPLACE changes it, REMOVE deletes
 * it -- and a process without CMKRNL is refused every change but may LIST.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>

#include "ssdef.h"
#include "prvdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77
#define UNPRIV_GID 100
#define UNPRIV_UID 100

static int passed, failed;
static void check(int c, const char *m)
{
    if (c) { passed++; printf("  PASS: %s\n", m); }
    else   { failed++; printf("  FAIL: %s\n", m); }
}

static uint32_t kfe(uint32_t op, int fd, uint64_t privs, uint32_t flags, const char *name,
                    struct vms_kfe_args *out)
{
    struct vms_kfe_args a;
    memset(&a, 0, sizeof a);
    a.op = op;
    a.fd = fd;
    a.privs = privs;
    a.flags = flags;
    if (name)
        snprintf(a.name, sizeof a.name, "%s", name);
    uint32_t st = vms_kif_kfe(&a);
    if (out)
        *out = a;
    return st;
}

/* The [100,100] child: no CMKRNL. Exit code 0 when every expectation held. */
static int run_child(const char *self)
{
    int ok = 1, fd = open(self, O_RDONLY), other = open("/bin/sh", O_RDONLY);
    struct vms_kfe_args a;
    if (fd < 0 || other < 0 || !vms_pcb_init(0xFFFFFFFFFFFFFFFFULL))
        return 2;
    /* A file nobody installed, so a wrongly allowed ADD changes nothing the
     * parent checks afterwards. */
    ok &= kfe(VMS_KFE_OP_ADD, other, VMS_PRV_M_CMKRNL, VMS_KFE_F_PRIV, "CHILD", NULL) == SS$_NOPRIV;
    ok &= (kfe(VMS_KFE_OP_FIND, fd, 0, 0, NULL, &a) & 1) && a.privs == VMS_PRV_M_CMKRNL;
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--child") == 0)
        return run_child(argv[0]);

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_kfe: the executive known-file list (INSTALL) ===\n");
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_kfe: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }
    int fd = open(argv[0], O_RDONLY), other = open("/bin/sh", O_RDONLY);
    struct vms_kfe_args a;
    check(fd >= 0 && other >= 0, "open this image and another file");

    check(kfe(VMS_KFE_OP_ADD, fd, VMS_PRV_M_CMKRNL, VMS_KFE_F_PRIV | VMS_KFE_F_OPEN,
              "SYS$SYSTEM:TEST_SYSSVC_KFE.EXE", NULL) == SS$_NORMAL,
          "INSTALL ADD /PRIVILEGED=CMKRNL of an open file is SS$_NORMAL");
    check(kfe(VMS_KFE_OP_ADD, fd, 0, 0, "AGAIN", NULL) == SS$_DUPLNAM,
          "a second ADD of the same file is a duplicate (SS$_DUPLNAM)");
    /* negctl: kfe-keyed-on-name */
    check(kfe(VMS_KFE_OP_FIND, other, 0, 0, NULL, NULL) == SS$_NOSUCHFILE,
          "another file is not installed: FIND is SS$_NOSUCHFILE");
    check((kfe(VMS_KFE_OP_FIND, fd, 0, 0, NULL, &a) & 1) && a.privs == VMS_PRV_M_CMKRNL &&
              (a.flags & VMS_KFE_F_PRIV) && !strcmp(a.name, "SYS$SYSTEM:TEST_SYSSVC_KFE.EXE"),
          "FIND reads back the entry: CMKRNL, /PRIVILEGED, its file spec");
    {
        int seen = 0;
        uint32_t idx = 0;
        for (int n = 0; n < VMS_KFE_MAX; n++) {
            struct vms_kfe_args l;
            memset(&l, 0, sizeof l);
            l.op = VMS_KFE_OP_LIST;
            l.index = idx;
            if (!(vms_kif_kfe(&l) & 1))
                break;
            seen |= !strcmp(l.name, "SYS$SYSTEM:TEST_SYSSVC_KFE.EXE");
            idx = l.index;
        }
        check(seen, "INSTALL LIST walks the list and finds the entry");
    }

    /* A process without CMKRNL may not change the list but may read it. */
    pid_t pid = fork();
    if (pid == 0) {
        if (setgid(UNPRIV_GID) != 0 || setuid(UNPRIV_UID) != 0)
            _exit(3);
        execl(argv[0], argv[0], "--child", (char *)NULL);
        _exit(4);
    }
    int ws = 0;
    waitpid(pid, &ws, 0);
    /* negctl: kfe-add-cmkrnl-not-checked */
    check(WIFEXITED(ws) && WEXITSTATUS(ws) == 0,
          "a process without CMKRNL: ADD is SS$_NOPRIV, FIND still reads the entry");

    check(kfe(VMS_KFE_OP_REPLACE, fd, VMS_PRV_M_CMKRNL | VMS_PRV_M_SYSPRV, VMS_KFE_F_PRIV,
              "SYS$SYSTEM:TEST_SYSSVC_KFE.EXE", NULL) == SS$_NORMAL &&
              (kfe(VMS_KFE_OP_FIND, fd, 0, 0, NULL, &a) & 1) &&
              a.privs == (VMS_PRV_M_CMKRNL | VMS_PRV_M_SYSPRV),
          "INSTALL REPLACE /PRIVILEGED=(CMKRNL,SYSPRV) changes the entry");
    check(kfe(VMS_KFE_OP_REMOVE, fd, 0, 0, NULL, NULL) == SS$_NORMAL &&
              kfe(VMS_KFE_OP_FIND, fd, 0, 0, NULL, NULL) == SS$_NOSUCHFILE,
          "INSTALL REMOVE deletes it: FIND is SS$_NOSUCHFILE");

    printf("=== test_syssvc_kfe: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
