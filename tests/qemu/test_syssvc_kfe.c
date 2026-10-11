/*
 * test_syssvc_kfe.c - the executive's known-file list (INSTALL, rd vms-7c64 /
 * vms-220) against a real /dev/vms.
 *
 * As on VMS (tests/lab/captures/install-priv-20261009/: INSTALL REPLACE/REMOVE
 * without CMKRNL fail %SYSTEM-F-NOCMKRNL), and as Baron ruled (vms-96e7,
 * vms-220): a CMKRNL process installs an image it holds open; the executive
 * itself copies it into its own read-only directory and the entry names that
 * copy. The copy has the source's bytes, cannot be opened for write -- not
 * even by substrate root -- and does not follow later changes to the source;
 * the caller's source file is not an installed image. A process without
 * CMKRNL is refused every change but may LIST.
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
#include <dirent.h>

#include "ssdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77
#define UNPRIV_GID 100
#define UNPRIV_UID 100
#define SRC "/tmp/test_syssvc_kfe.src"

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

static int same_bytes(const char *a, const char *b)
{
    char x[256], y[256];
    int fa = open(a, O_RDONLY), fb = open(b, O_RDONLY);
    ssize_t n = fa >= 0 ? read(fa, x, sizeof x) : -1, m = fb >= 0 ? read(fb, y, sizeof y) : -2;
    if (fa >= 0) close(fa);
    if (fb >= 0) close(fb);
    return n >= 0 && n == m && memcmp(x, y, (size_t)n) == 0;
}

/* Files in the executive's directory (the copies it holds). */
static int kfe_dir_count(void)
{
    int n = 0;
    struct dirent *e;
    DIR *d = opendir(VMS_KFE_DIR);
    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.')
            n++;
    closedir(d);
    return n;
}

/* The [100,100] child: no CMKRNL. Exit code 0 when every expectation held. */
static int run_child(void)
{
    int ok = 1, fd = open(SRC, O_RDONLY);
    struct vms_kfe_args a;
    if (fd < 0 || !vms_pcb_init(0xFFFFFFFFFFFFFFFFULL))
        return 2;
    ok &= kfe(VMS_KFE_OP_ADD, fd, VMS_PRV_M_CMKRNL, VMS_KFE_F_PRIV, "CHILDIMG", NULL) == SS$_NOPRIV;
    ok &= kfe(VMS_KFE_OP_FIND_NAME, -1, 0, 0, "CHILDIMG", NULL) == SS$_NOSUCHFILE;
    ok &= (kfe(VMS_KFE_OP_FIND_NAME, -1, 0, 0, "TEST_SYSSVC_KFE.EXE", &a) & 1) &&
          a.privs == VMS_PRV_M_CMKRNL;
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--child") == 0)
        return run_child();

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
    /* What PID 1 makes at boot (src/ovmx_init): the executive's directory. */
    (void)mkdir("/run", 0755);
    (void)mkdir("/run/ovmx-boot", 0755);
    (void)mkdir(VMS_KFE_DIR, 0711);

    int w = open(SRC, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    check(w >= 0 && write(w, "IMAGE-ONE", 9) == 9, "a scratch source image is written");
    close(w);
    int fd = open(SRC, O_RDONLY);
    struct vms_kfe_args a;

    uint32_t ast = kfe(VMS_KFE_OP_ADD, fd, VMS_PRV_M_CMKRNL, VMS_KFE_F_PRIV | VMS_KFE_F_OPEN,
                       "TEST_SYSSVC_KFE.EXE", &a);
    printf("  (INSTALL ADD status %%X%08X, copy %s)\n", (unsigned)ast, a.path);
    check(ast == SS$_NORMAL, "INSTALL ADD /PRIVILEGED=CMKRNL of an open file is SS$_NORMAL");
    char copy[256];
    snprintf(copy, sizeof copy, "%s", a.path);
    check(strncmp(copy, VMS_KFE_DIR "/", sizeof(VMS_KFE_DIR)) == 0,
          "the entry names the executive's own copy, in its directory (not the caller's file)");
    check(same_bytes(SRC, copy), "the executive's copy has the source image's bytes");
    struct stat st;
    check(stat(copy, &st) == 0 && (st.st_mode & 0777) == 0555,
          "the executive's copy is read-only (0555)");

    int cw = open(copy, O_WRONLY);
    /* negctl: kfe-write-not-denied */
    check(cw < 0 && errno == ETXTBSY,
          "opening the executive's copy for write is refused (ETXTBSY) -- even by substrate root");
    if (cw >= 0)
        close(cw);

    w = open(SRC, O_WRONLY | O_TRUNC);
    check(w >= 0 && write(w, "CHANGED!!", 9) == 9, "the caller rewrites its source file");
    close(w);
    check(!same_bytes(SRC, copy), "the installed copy does not follow the change to the source");

    int before = kfe_dir_count();
    check(kfe(VMS_KFE_OP_ADD, fd, 0, 0, "TEST_SYSSVC_KFE.EXE", NULL) == SS$_DUPLNAM,
          "a second ADD of the same name is a duplicate (SS$_DUPLNAM)");
    check(before >= 1 && kfe_dir_count() == before,
          "...and the refused ADD leaves no copy behind in the executive's directory");
    int cfd = open(copy, O_RDONLY);
    check(cfd >= 0 && (kfe(VMS_KFE_OP_FIND, cfd, 0, 0, NULL, &a) & 1) &&
              a.privs == VMS_PRV_M_CMKRNL && (a.flags & VMS_KFE_F_PRIV) &&
              !strcmp(a.name, "TEST_SYSSVC_KFE.EXE"),
          "FIND by the executive's copy reads back the entry: CMKRNL, /PRIVILEGED, its name");
    /* negctl: kfe-keyed-on-name */
    check(kfe(VMS_KFE_OP_FIND, fd, 0, 0, NULL, NULL) == SS$_NOSUCHFILE,
          "the caller's own source file is not an installed image: FIND is SS$_NOSUCHFILE");
    {
        int seen = 0;
        uint32_t idx = 0;
        for (int n = 0; n < VMS_KFE_MAX; n++) {
            struct vms_kfe_args l;
            memset(&l, 0, sizeof l);
            l.op = VMS_KFE_OP_LIST;
            l.fd = -1;
            l.index = idx;
            if (!(vms_kif_kfe(&l) & 1))
                break;
            seen |= !strcmp(l.name, "TEST_SYSSVC_KFE.EXE");
            idx = l.index;
        }
        check(seen, "INSTALL LIST walks the list and finds the entry");
    }

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
              "TEST_SYSSVC_KFE.EXE", &a) == SS$_NORMAL &&
              strcmp(a.path, copy) != 0 && same_bytes(SRC, a.path),
          "INSTALL REPLACE takes a new executive copy of the current image");
    char copy2[256];
    snprintf(copy2, sizeof copy2, "%s", a.path);
    check(access(copy, F_OK) != 0 && errno == ENOENT,
          "...and the executive deletes the copy it replaced");
    check((kfe(VMS_KFE_OP_FIND_NAME, -1, 0, 0, "TEST_SYSSVC_KFE.EXE", &a) & 1) &&
              a.privs == (VMS_PRV_M_CMKRNL | VMS_PRV_M_SYSPRV),
          "...with the new privileges");
    check(kfe(VMS_KFE_OP_REMOVE, -1, 0, 0, "TEST_SYSSVC_KFE.EXE", NULL) == SS$_NORMAL &&
              kfe(VMS_KFE_OP_FIND_NAME, -1, 0, 0, "TEST_SYSSVC_KFE.EXE", NULL) == SS$_NOSUCHFILE,
          "INSTALL REMOVE deletes the entry");
    /* negctl: kfe-remove-copy-kept */
    check(access(copy2, F_OK) != 0 && errno == ENOENT,
          "INSTALL REMOVE also deletes the executive's copy from its directory");

    if (cfd >= 0) close(cfd);
    close(fd);
    unlink(SRC);
    printf("=== test_syssvc_kfe: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
