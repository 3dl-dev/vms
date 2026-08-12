/*
 * test_kmod_vmsfs_sysgroup.c - a SYSTEM session ([1,4], Linux gid=1) can
 * CREATE a file in a SYSTEM-group directory on a real vmsfs.ko mount, the
 * same way it can on the system disk (vms-581).
 *
 * THE DEFECT THIS PINS. vmsfs_blkdev_permission() (src/kernel/vmsfs/
 * vmsfs_blkdev.c) decided the VMS "System" protection category with
 *
 *     process is root  OR  process UIC group == 0
 *
 * but OpenVMS's System category is a GROUP COMPARISON, not equality with 0:
 * every UIC whose group number is <= MAXSYSGROUP (SYSGEN default 8) is in the
 * System category (OpenVMS Guide to System Security, "System" access
 * category). The userspace executive already applies exactly this rule --
 * uic_is_system() in src/libvms/syssvc/sys_security.c, pinned to
 * src/libvms/include/ovmx_secparam.h (OVMX_MAXSYSGROUP == 8). vmsfs.ko never
 * got that memo.
 *
 * WHY IT BLOCKED THE INSTALL. LOGINOUT drops each session to its
 * authenticated UIC (tools/vms_login.c: setgroups(0); setgid(uic_group);
 * setuid(uic_member)), so the SYSTEM account -- UIC [1,4] -- runs as Linux
 * gid=1, uid=4. NOT gid=0, NOT root. The `group == 0` test therefore never
 * put SYSTEM in the System category on any real vmsfs.ko mount: SYSTEM fell to
 * the Group nibble (a SYSTEM-group directory is owned [1,1] by vms_initialize
 * -- group matches, member does not) or the World nibble, and
 * VMSFS_PROT_DEFAULT (0xAA00) denies WRITE to both. So RMS CREATE, which is
 * open(2) O_CREAT under the hood (sys$create(), src/vmsrms/rms_core.c),
 * returned %RMS-E-CRE with STV == EACCES -- and BOTH of OVMX's password-write
 * mechanisms (SET PASSWORD's sysuaf_write_record() and AUTHORIZE writing the
 * target SYSUAF) hit it, so the faithful install (vms-718/vms-dcf) could not
 * write the SYSTEM password to the target. Found by vms-dcf (#374):
 * `CREATE DKA0:[SYSEXE]PROBE.TXT` on the distribution disk itself -- no
 * target, no redirection -- returned %RMS-E-CRE.
 *
 * WHY THIS WAS NEVER CAUGHT. Every prior vmsfs create test either ran as root
 * (test_kmod_vmsfs_blkdev.c: root is System, so create always succeeded) or
 * ran against /vms, which in the tests/qemu rig is a plain rootfs directory,
 * NOT a vmsfs mount, so Linux DAC -- not vmsfs's SOGW check -- gated it
 * (test_syssvc_scratch_writable.c's own header says so). This suite is the
 * missing case: a NON-root, SYSTEM-group process creating on a REAL vmsfs.ko
 * mount.
 *
 * THREE DIRECTIONS, ONE SUITE (each a separate fork -- no run can be explained
 * by state another left in this process):
 *   A. SYSTEM [1,4] creating a file IN a [1,1]-owned directory must SUCCEED
 *      (the vms-581 fix: group 1 <= MAXSYSGROUP -> System category). This is
 *      the assertion that FAILS on origin/main (EACCES) and PASSES after.
 *   B. SYSTEM [1,4] reopening that file for READ must SUCCEED (the file it
 *      just created is stamped [1,4]; it also lands in System). Proves the
 *      create actually persisted a readable file, not a phantom fd.
 *   C. An ordinary account [200,202] (group 200 > MAXSYSGROUP) creating in the
 *      SAME directory must be DENIED (EACCES) -- the fix widens System to the
 *      system groups only, it does NOT make the directory world-writable.
 *      Holds identically before and after the fix (negative control).
 *
 * Requires a real vmsfs.ko and /dev/loop0 attached to the test image (init.sh
 * sets both up). SKIPs (77) if loop0 or the mount is unavailable -- never a
 * fake pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <grp.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define EXIT_SKIP 77

#define LOOP_DEV    "/dev/loop0"
#define MOUNT_POINT "/mnt/vmsfs_sysgroup"

/* UICs as [group,member]. Login maps UIC group -> Linux gid, member -> uid. */
#define DIROWNER_GID 1    /* [1,1] -- a SYSTEM-group dir, as vms_initialize stamps */
#define DIROWNER_UID 1
#define SYSTEM_GID   1    /* [1,4] -- the SYSTEM account after LOGINOUT's drop  */
#define SYSTEM_UID   4
#define USER_GID     202  /* [200,202] -- an ordinary shipped account           */
#define USER_UID     200

static int pass = 0;
static int fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s (errno=%d)\n", msg, errno); fail++; } \
} while (0)

/* Child-exit encoding: 0 == op succeeded; else 100 + min(errno,99). */
#define ENC_OK   0
static int enc_err(int e) { return 100 + (e > 99 ? 99 : e); }
static int dec_err(int status) { return status >= 100 ? status - 100 : 0; }

/* Drop, permanently, to a VMS UIC exactly like tools/vms_login.c. Returns 0
 * on success; on failure the child exits enc_err(errno) itself. */
static int drop_to_uic(gid_t gid, uid_t uid)
{
    if (setgroups(0, NULL) != 0) return -1;
    if (setgid(gid) != 0)        return -1;
    if (setuid(uid) != 0)        return -1;
    return 0;
}

/* fork a child, drop to [gid,uid], mkdir(path). Parent returns child status. */
static int child_mkdir(gid_t gid, uid_t uid, const char *path)
{
    pid_t pid = fork();
    if (pid < 0) return enc_err(errno);
    if (pid == 0) {
        if (drop_to_uic(gid, uid) != 0) _exit(enc_err(errno));
        if (mkdir(path, 0755) != 0)     _exit(enc_err(errno));
        _exit(ENC_OK);
    }
    int st; waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : enc_err(EINTR);
}

/* fork a child, drop to [gid,uid], open(path, O_CREAT|O_RDWR) and write. */
static int child_create(gid_t gid, uid_t uid, const char *path, const char *data)
{
    pid_t pid = fork();
    if (pid < 0) return enc_err(errno);
    if (pid == 0) {
        if (drop_to_uic(gid, uid) != 0) _exit(enc_err(errno));
        int fd = open(path, O_CREAT | O_RDWR, 0644);
        if (fd < 0) _exit(enc_err(errno));
        if (data && write(fd, data, strlen(data)) != (ssize_t)strlen(data)) {
            int e = errno; close(fd); _exit(enc_err(e));
        }
        close(fd);
        _exit(ENC_OK);
    }
    int st; waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : enc_err(EINTR);
}

/* fork a child, drop to [gid,uid], open(path, O_RDONLY) and compare content. */
static int child_read_expect(gid_t gid, uid_t uid, const char *path,
                             const char *want)
{
    pid_t pid = fork();
    if (pid < 0) return enc_err(errno);
    if (pid == 0) {
        if (drop_to_uic(gid, uid) != 0) _exit(enc_err(errno));
        int fd = open(path, O_RDONLY);
        if (fd < 0) _exit(enc_err(errno));
        char buf[256]; memset(buf, 0, sizeof(buf));
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n < 0) _exit(enc_err(errno));
        _exit(strcmp(buf, want) == 0 ? ENC_OK : enc_err(EIO));
    }
    int st; waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : enc_err(EINTR);
}

int main(void)
{
    int rc;
    char dirpath[256], probe[256], probe2[256];

    printf("=== test_kmod_vmsfs_sysgroup ===\n");

    if (access(LOOP_DEV, F_OK) != 0) {
        printf("  SKIP: %s not present (no loop device)\n", LOOP_DEV);
        printf("=== test_kmod_vmsfs_sysgroup: %d passed, %d failed (SKIPPED) ===\n",
               pass, fail);
        return EXIT_SKIP;
    }

    mkdir(MOUNT_POINT, 0755);
    /* Best-effort: clear any stale mount another suite left on our point. */
    umount2(MOUNT_POINT, MNT_DETACH);

    rc = mount(LOOP_DEV, MOUNT_POINT, "vmsfs", 0, NULL);
    if (rc != 0) {
        printf("  SKIP: rw mount of vmsfs failed (errno=%d)\n", errno);
        printf("=== test_kmod_vmsfs_sysgroup: %d passed, %d failed (SKIPPED) ===\n",
               pass, fail);
        return EXIT_SKIP;
    }

    snprintf(dirpath, sizeof(dirpath), "%s/SYSTEST", MOUNT_POINT);
    snprintf(probe,   sizeof(probe),   "%s/SYSTEST/PROBE.TXT", MOUNT_POINT);
    snprintf(probe2,  sizeof(probe2),  "%s/SYSTEST/PROBE2.TXT", MOUNT_POINT);

    /* Setup: create a SYSTEM-group directory owned [1,1], VMSFS_PROT_DEFAULT.
     * (Runs as [1,1], which is Owner of the MFD root in the test image, so it
     * succeeds on both origin/main and the fixed tree -- it is the SCENERY,
     * not the thing under test.) */
    rc = child_mkdir(DIROWNER_GID, DIROWNER_UID, dirpath);
    CHECK(rc == ENC_OK, "setup: [1,1] created a SYSTEM-group directory");
    if (rc != ENC_OK) {
        printf("    (mkdir errno=%d -- cannot run the create scenarios)\n",
               dec_err(rc));
        umount(MOUNT_POINT);
        printf("=== test_kmod_vmsfs_sysgroup: %d passed, %d failed ===\n",
               pass, fail);
        return fail > 0 ? 1 : 0;
    }

    /* A. THE vms-581 ASSERTION. SYSTEM [1,4] creating in the [1,1] directory
     *    must SUCCEED. On origin/main this returns EACCES (SYSTEM fell to the
     *    Group nibble, which VMSFS_PROT_DEFAULT denies WRITE) -> %RMS-E-CRE. */
    rc = child_create(SYSTEM_GID, SYSTEM_UID, probe, "SYSTEM WAS HERE\n");
    CHECK(rc == ENC_OK,
          "SYSTEM [1,4] CREATE in a [1,1] SYSTEM-group dir succeeds "
          "(vms-581: was EACCES/%RMS-E-CRE on main)");
    if (rc != ENC_OK)
        printf("    (create errno=%d; EACCES==%d)\n", dec_err(rc), EACCES);

    /* B. The file is real and readable back by SYSTEM. */
    rc = child_read_expect(SYSTEM_GID, SYSTEM_UID, probe, "SYSTEM WAS HERE\n");
    CHECK(rc == ENC_OK, "SYSTEM reads its just-created file back, content intact");

    /* C. NEGATIVE CONTROL. An ordinary [200,202] account (group 200 >
     *    MAXSYSGROUP) creating in the same dir must still be DENIED -- the fix
     *    widens System to system groups only, not the world. */
    rc = child_create(USER_GID, USER_UID, probe2, "user\n");
    CHECK(rc != ENC_OK && dec_err(rc) == EACCES,
          "ordinary [200,202] CREATE in the SYSTEM-group dir is DENIED (EACCES)");
    if (rc == ENC_OK)
        printf("    (UNEXPECTED: world got write -- the dir is not "
               "world-writable)\n");

    /* Cleanup (root == System: full access to unlink/rmdir). */
    unlink(probe);
    unlink(probe2);
    rmdir(dirpath);
    umount(MOUNT_POINT);

    printf("=== test_kmod_vmsfs_sysgroup: %d passed, %d failed ===\n",
           pass, fail);
    return fail > 0 ? 1 : 0;
}
