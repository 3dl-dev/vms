/*
 * test_syssvc_scratch_writable.c - a SYSTEM (UIC [1,4]) session can
 * sys$create under SYS$SCRATCH: and SYS$LOGIN:, and an ordinary account
 * still cannot (vms-e5c).
 *
 * THE DEFECT THIS CLOSES. [SYSTMP] (SYS$SCRATCH:'s target) and [USERS]
 * (SYS$LOGIN:'s SYSTEM-table default) are MFD-level siblings of [SYS0],
 * created at every boot by src/ovmx_init/ovmx_init.c's provision_dirs().
 * PROVISION.EXE's identity-derived ownership walk
 * (src/ovmx_provision/ovmx_provision.c, provision_ownership()) only reaches
 * [SYS0] and everything beneath it, so these two directories used to stay
 * root:root 0755 forever: a SYSTEM DCL session got EACCES on sys$create
 * under either one, and OVMX's own PARTS demo (RMS indexed-file app) silently
 * fell all the way through its candidate list to /tmp/PARTS.DAT -- a Unix
 * leak in a "zero Unix leaks" narrative, and the reason the 0.2 killer-app
 * demo's data file has never actually lived under a VMS filespec.
 *
 * WHY A RAW open(2), NOT DCL's CREATE OR PARTS's sys$create() (MEASURED,
 * vms-e5c, both against a real QEMU boot with this item's fix applied):
 *
 *   - `$ CREATE/DIRECTORY SYS$SCRATCH:[anything]` currently crashes DCL
 *     (segfault, session drops back to Username:) -- a PRE-EXISTING defect
 *     in src/vmsdcl/src/vmsfs's logical-name-plus-directory resolution,
 *     reproduced identically with and without this item's fix, so it is
 *     unrelated to what this item changes. src/vmsdcl is also outside this
 *     item's lane (src/ovmx_init and distro/rootfs only).
 *   - PARTS's sys$create() (src/vmsrms/rms_core.c, an RMS INDEXED file via
 *     FAB$C_IDX + XABKEY) STILL returns EACCES even once these two
 *     directories are genuinely SYSTEM-writable at the Linux/vmsfs layer --
 *     MEASURED directly: a plain open(2) on the exact same
 *     vmsfs_to_linux_path()-resolved path, under the exact same dropped
 *     SYSTEM credentials, in the exact same QEMU boot, succeeds every time.
 *     So the gap is downstream of this item, in RMS's indexed-file create
 *     path and/or the kernel module's own version/protection handling --
 *     src/vmsrms and src/kernel/vmsfs are both outside this item's lane.
 *     Filed as a separate finding; not fixed here, and not silently routed
 *     around by weakening what this test checks.
 *
 * So this suite proves the thing THIS item actually owns -- the directory
 * permission decision vmsfs makes for a real sys$create-equivalent open(2)
 * -- directly, the same way test_syssvc_ident.c and test_syssvc_setuai.c
 * prove an executive/RMS decision without going through DCL or a shipped
 * application: a real credential drop to a real UIC, against a real
 * vmsfs-mounted /dev/vms.
 *
 * TWO DIRECTIONS, ONE SUITE:
 *   A. SYSTEM (UIC [1,4]) sys$create-equivalent open() under SYS$SCRATCH:
 *      and SYS$LOGIN: must SUCCEED.
 *   B. USER1 (UIC [200,202], an ordinary shipped account) attempting the
 *      SAME open() under SYS$SCRATCH: must FAIL -- proving the fix made
 *      SYSTEM the owner, not the world writable (CLAUDE.md Rule 8 spirit:
 *      match the VMS UIC/protection model, not a Unix "everyone" bit).
 *
 * Requires a real, insmod'd vms.ko at /dev/vms (this is a vmsfs-mounted
 * system disk, and the CI negative-control rig boots with no executive at
 * all, vms-0ff): exits EXIT_SKIP (77) there, never a fake pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <grp.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "vms_kif.h"
#include "ovmx_layout.h"
#include "vmsfs/device.h"
#include "vmsfs/filespec.h"
#include "vms/logical.h"

#define EXIT_SKIP 77

static int pass = 0;
static int fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* SYSTEM's UIC, as shipped in distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/
 * SYSUAF.DAT ("SYSTEM|...|1|4|..."); USER1's, as shipped in the same file
 * ("USER1|...|200|202|..."). Pinned to the DATA FILE, not re-derived by
 * parsing it here -- this suite is a consumer of the shipped UIC values,
 * the same way ovmx_init.c's provision_writable_dir() is (vms-e5c, vms-9b7:
 * no second SYSUAF parser). */
#define SYSTEM_UIC_GROUP   1u
#define SYSTEM_UIC_MEMBER  4u
#define USER1_UIC_GROUP    200u
#define USER1_UIC_MEMBER   202u

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0)
        return 0;
    vms_kif_close();
    return 1;
}

/*
 * Fork a child, drop it to the given UIC (the same setgroups/setgid/setuid
 * sequence tools/vms_login.c uses for a real credential drop -- see its
 * "BECOME THE AUTHENTICATED USER" section), and try to sys$create-equivalent
 * open(2) a fresh file under the given VMS filespec DEVICE (no directory
 * bracket -- SYS$SCRATCH: and SYS$LOGIN: are the directory).
 *
 * Returns 1 if the create succeeded (and cleans it up), 0 if it failed, -1
 * on an infrastructure error (fork/wait/credential-drop failure -- never
 * silently folded into either verdict).
 */
static int try_create_as(const char *vms_device, const char *name,
                          uint32_t uic_group, uint32_t uic_member,
                          char *detail, size_t detail_sz)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        snprintf(detail, detail_sz, "pipe() failed: %s", strerror(errno));
        return -1;
    }

    fflush(NULL);  /* vms-cdb: flush before fork, see test_syssvc_authorize.c */
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        snprintf(detail, detail_sz, "fork() failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        char msg[900];

        if (setgroups(0, NULL) != 0 ||
            setgid((gid_t)uic_group) != 0 ||
            setuid((uid_t)uic_member) != 0) {
            snprintf(msg, sizeof(msg), "ERR credential-drop failed: %s",
                     strerror(errno));
            (void)!write(pipefd[1], msg, strlen(msg));
            _exit(2);
        }

        char vms_spec[256], linux_path[600];
        snprintf(vms_spec, sizeof(vms_spec), "%s%s", vms_device, name);
        if (vmsfs_to_linux_path(vms_spec, linux_path, sizeof(linux_path)) != 1) {
            snprintf(msg, sizeof(msg), "ERR vmsfs_to_linux_path(%s) failed to resolve",
                      vms_spec);
            (void)!write(pipefd[1], msg, strlen(msg));
            _exit(2);
        }

        int fd = open(linux_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            snprintf(msg, sizeof(msg), "DENY %s (uid=%d gid=%d): %s",
                     linux_path, (int)geteuid(), (int)getegid(),
                     strerror(errno));
            (void)!write(pipefd[1], msg, strlen(msg));
            _exit(1);
        }
        close(fd);
        unlink(linux_path);
        snprintf(msg, sizeof(msg), "OK %s (uid=%d gid=%d)",
                 linux_path, (int)geteuid(), (int)getegid());
        (void)!write(pipefd[1], msg, strlen(msg));
        _exit(0);
    }

    close(pipefd[1]);
    ssize_t n = read(pipefd[0], detail, detail_sz - 1);
    detail[n > 0 ? n : 0] = '\0';
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(status)) {
        snprintf(detail, detail_sz, "child did not exit normally");
        return -1;
    }
    if (WEXITSTATUS(status) == 2)
        return -1;   /* infrastructure error, detail already filled */
    return WEXITSTATUS(status) == 0;
}

/*
 * Provision one directory EXACTLY the way src/ovmx_init/ovmx_init.c's
 * provision_writable_dir() does at boot (mkdir 0775, chown to SYSTEM's UIC
 * -- both fields, chmod 0775) -- duplicated here, not called, because this
 * suite runs in the kernel-executive QEMU rig (tests/qemu/Dockerfile +
 * init.sh), which insmods vms.ko and drives the public sys$ API directly
 * WITHOUT ever running PID 1 / STARTUP.EXE's boot sequence. So [SYSTMP] and
 * [USERS] are never created by the product's own boot path in this rig, and
 * this suite has to stand them up itself before it can probe them -- against
 * the SAME real, insmod'd /dev/vms every other test_syssvc_* suite uses.
 * (The full-boot integration -- does STARTUP.EXE actually reach this code
 * and does PARTS's own sys$create() land here -- was verified by hand
 * against a real `distro/Dockerfile.bootable` QEMU boot for this item; see
 * the PR description. That path has no ctest/CI hook of its own.)
 *
 * Kept as a literal duplicate, not a shared helper, because
 * provision_writable_dir() is `static` in a PID-1-only translation unit
 * with no public header -- factoring it out is a src/ovmx_init change,
 * outside what a tests/qemu file may do.
 */
static void provision_writable_dir_for_test(const char *path)
{
    mkdir(path, 0775);
    if (chown(path, (uid_t)SYSTEM_UIC_MEMBER, (gid_t)SYSTEM_UIC_GROUP) != 0)
        fprintf(stderr, "  SETUP-WARN: chown(%s) failed: %s\n", path, strerror(errno));
    if (chmod(path, 0775) != 0)
        fprintf(stderr, "  SETUP-WARN: chmod(%s) failed: %s\n", path, strerror(errno));
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_scratch_writable (SYSTEM sys$create under "
           "SYS$SCRATCH:/SYS$LOGIN:, vms-e5c) ===\n");

    /* Bootstrap the VMS namespace the same way every shipped image does
     * (ovmx_init.c, ovmx_provision.c, DCL.EXE): a bare device name has
     * nothing to resolve against until SYS$SYSDEVICE is in this process's
     * device table and the SYS$SCRATCH/SYS$LOGIN logicals exist. */
    vmsfs_device_add(SYSDISK_DEVICE, SYSDISK_MOUNT);
    lnm_setup_defaults(lnm_get_manager(), SYSDISK_MOUNT);

    if (!executive_present()) {
        printf("  INFO: cannot open /dev/vms -- CI negative-control rig, not the product\n");
        printf("=== test_syssvc_scratch_writable: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    /* Stand up [SYSTMP] and [USERS] as this rig's boot path never does --
     * see provision_writable_dir_for_test()'s header comment. */
    {
        char p[512];
        if (vmsfs_to_linux_path(VMS_SYSTMP, p, sizeof(p)) == 1)
            provision_writable_dir_for_test(p);
        if (vmsfs_to_linux_path(VMS_USERS, p, sizeof(p)) == 1)
            provision_writable_dir_for_test(p);
    }

    char detail[900];

    /* --- A: SYSTEM must succeed under both directories ------------------ */
    int rc = try_create_as("SYS$SCRATCH:", "E5C_SYSTEM_SCRATCH.DAT",
                            SYSTEM_UIC_GROUP, SYSTEM_UIC_MEMBER,
                            detail, sizeof(detail));
    printf("  A1: %s\n", detail);
    CHECK(rc == 1, "SYSTEM's sys$create-equivalent open() succeeds under SYS$SCRATCH:");

    rc = try_create_as("SYS$LOGIN:", "E5C_SYSTEM_LOGIN.DAT",
                        SYSTEM_UIC_GROUP, SYSTEM_UIC_MEMBER,
                        detail, sizeof(detail));
    printf("  A2: %s\n", detail);
    CHECK(rc == 1, "SYSTEM's sys$create-equivalent open() succeeds under SYS$LOGIN:");

    /* --- B: an ordinary account must NOT succeed under SYS$SCRATCH: ----- */
    rc = try_create_as("SYS$SCRATCH:", "E5C_USER1_SCRATCH.DAT",
                        USER1_UIC_GROUP, USER1_UIC_MEMBER,
                        detail, sizeof(detail));
    printf("  B1: %s\n", detail);
    CHECK(rc == 0, "USER1 (an ordinary account) is REFUSED under SYS$SCRATCH: "
                   "-- the fix made SYSTEM the owner, not the world writable");

    printf("=== test_syssvc_scratch_writable: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
