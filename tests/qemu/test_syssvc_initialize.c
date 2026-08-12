/*
 * test_syssvc_initialize.c - INITIALIZE.EXE formats the REAL backing block
 * device a VMS device name resolves to, and fails honestly on an unresolvable
 * name -- it never formats a bogus local file and reports success (vms-cf62).
 *
 * THE DEFECT THIS EXISTS TO KILL. tools/vms_initialize.c used to treat its
 * first argument as a literal filesystem path. `INITIALIZE DKA100: OVMX 100`
 * therefore did stat("DKA100:") (no such file), took the create-a-file branch,
 * and formatted a brand-new local file literally named "DKA100:" in the
 * current directory -- reporting %INIT success. The real disk DKA100: names
 * (the second virtio unit, vdb) was never touched. The install menu's
 * INITIALIZE step "succeeded" and initialized nothing. That is the INV-6
 * fake-success class exactly (CLAUDE.md Rule 9): a facility that reports
 * success while sharing nothing with the real device.
 *
 * WHAT IS PROVEN HERE, against a real /dev/vms and a real virtio disk:
 *
 *   1. `INITIALIZE DKA100: TESTVOL` resolves DKA100: through the executive
 *      (vms_kif_disk_resolve, the same path MOUNT uses) to its backing block
 *      device and writes the vmsfs home block to THAT device. Proven A/B:
 *      the backing device carries NO home block before, and a valid one --
 *      with the label this command wrote -- after. A resolver that always
 *      "succeeded" without touching the real store fails the "after" read.
 *
 *   2. `INITIALIZE BOGUS999: TESTVOL 10` (a name the executive cannot
 *      resolve, WITH a size argument -- the exact shape that made the old
 *      code fake success) FAILS with a non-zero exit and creates NO local
 *      file named "BOGUS999:". On origin/main this test goes red: the old
 *      code exits 0 and leaves a bogus file behind.
 *
 * The backing device to inspect is discovered from the executive itself
 * (vms_kif_disk_resolve), so the test reads the SAME device INITIALIZE was
 * supposed to write -- not a device name this test picked independently.
 *
 * NEGATIVE-CONTROL RIG (NEGATIVE_CONTROL=1 in tests/qemu/Dockerfile boots
 * without insmod'ing vms.ko): the executive is absent, which is not a product
 * state (vms-0ff). With no executive to resolve the unit, INITIALIZE cannot
 * know a backing device -- and the property that must hold regardless is that
 * it does NOT fake success: `INITIALIZE DKA100: TESTVOL` must exit non-zero
 * and leave no bogus "DKA100:" file. The suite asserts that, then exits
 * EXIT_SKIP (77) -- never a fake pass -- matching the honest-skip contract
 * ci.yml's kernel-executive-negative-control job holds every test_syssvc_* to.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "ssdef.h"
#include "vms_kif.h"
#include "vmsfs_ondisk.h"

#define EXIT_SKIP 77

/* Where tests/qemu/Dockerfile stages the shipped INITIALIZE image. */
#define INIT_PATH_DEFAULT "/tests/INITIALIZE.EXE"

/* The child runs from here so the old code's bogus-file artifact lands at a
 * deterministic path this test can look for (and so must NOT find). */
#define WORKDIR "/tmp"

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static const char *init_path(void)
{
    const char *p = getenv("OVMX_INITIALIZE");
    return (p && p[0]) ? p : INIT_PATH_DEFAULT;
}

/*
 * run_init - execute INITIALIZE.EXE with the given args from WORKDIR and
 * return its exit code, or -1 if it could not be run / did not exit normally.
 * argv is NULL-terminated.
 */
static int run_init(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (chdir(WORKDIR) != 0)
            _exit(127);
        /* stdout/stderr inherit -- the child's %INIT lines land in the
         * transcript, attributable to this suite. */
        execv(argv[0], argv);
        _exit(127);
    }
    int wstatus;
    if (waitpid(pid, &wstatus, 0) != pid)
        return -1;
    if (!WIFEXITED(wstatus))
        return -1;
    return WEXITSTATUS(wstatus);
}

/*
 * home_block_magic - read LBN 1 off a raw block device and return its
 * hb_magic (0 on read failure). Independent of INITIALIZE's own code path:
 * a bare pread of the real device.
 */
static uint32_t home_block_magic(const char *devpath, char *volname_out,
                                 size_t volname_sz)
{
    int fd = open(devpath, O_RDONLY);
    if (fd < 0)
        return 0;
    uint8_t blk[VMSFS_BLOCK_SIZE];
    ssize_t n = pread(fd, blk, sizeof(blk),
                      (off_t)VMSFS_HOME_LBN * VMSFS_BLOCK_SIZE);
    close(fd);
    if (n != (ssize_t)sizeof(blk))
        return 0;
    const struct vmsfs_home_block *hb = (const struct vmsfs_home_block *)blk;
    if (volname_out && volname_sz) {
        size_t k = sizeof(hb->hb_volname);
        if (k >= volname_sz) k = volname_sz - 1;
        memcpy(volname_out, hb->hb_volname, k);
        volname_out[k] = '\0';
    }
    return hb->hb_magic;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct stat st;

    printf("=== test_syssvc_initialize (INITIALIZE resolves DKA100: to the real backing device) ===\n");

    if (stat(init_path(), &st) != 0) {
        printf("  FAIL: %s is not staged in the initramfs -- this suite cannot\n",
               init_path());
        printf("        run the command it exists to test\n");
        printf("=== test_syssvc_initialize: 0 passed, 1 failed ===\n");
        return 1;
    }

    char bogus_file[64];
    snprintf(bogus_file, sizeof(bogus_file), "%s/BOGUS999:", WORKDIR);
    unlink(bogus_file);   /* start clean */

    /* --------------------------------------------------------------
     * NEGATIVE-CONTROL RIG: no executive. Assert only the property that
     * holds regardless -- INITIALIZE of a device name must not fake success.
     * -------------------------------------------------------------- */
    if (vms_kif_open() < 0) {
        char *av[] = { (char *)init_path(), (char *)"DKA100:",
                       (char *)"TESTVOL", NULL };
        int rc = run_init(av);
        CHECK(rc != 0,
              "with no executive, INITIALIZE DKA100: fails honestly -- it does NOT fake success");
        CHECK(stat(bogus_file, &st) != 0,
              "with no executive, INITIALIZE created no bogus local file");

        printf("=== test_syssvc_initialize: %d passed, %d failed (SKIPPED: no /dev/vms -- the real-device format was not exercised, the no-fake-success checks WERE) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* --------------------------------------------------------------
     * Discover, from the executive itself, the backing device DKA100:
     * resolves to -- the device INITIALIZE is supposed to write.
     * -------------------------------------------------------------- */
    char backing[VMS_BACKING_SIZE];
    memset(backing, 0, sizeof(backing));
    uint32_t rst = vms_kif_disk_resolve("DKA100:", backing, sizeof(backing),
                                        NULL, NULL);
    CHECK(rst == SS$_NORMAL,
          "DKA100: resolves to a backing block device through the executive");
    if (rst != SS$_NORMAL) {
        printf("=== test_syssvc_initialize: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }
    char devpath[VMS_BACKING_SIZE + 8];
    snprintf(devpath, sizeof(devpath), "/dev/%s", backing);
    printf("  INFO: DKA100: -> %s\n", devpath);

    /* --------------------------------------------------------------
     * A: the backing device carries NO vmsfs home block before INITIALIZE.
     * (run_tests.sh attaches a blank scratch disk for vdb.)
     * -------------------------------------------------------------- */
    uint32_t magic_before = home_block_magic(devpath, NULL, 0);
    CHECK(magic_before != VMSFS_HOME_MAGIC,
          "before INITIALIZE, the backing device has no vmsfs home block");

    /* --------------------------------------------------------------
     * Run INITIALIZE DKA100: TESTVOL (no size arg -- the device geometry is
     * authoritative). Must succeed.
     * -------------------------------------------------------------- */
    {
        char *av[] = { (char *)init_path(), (char *)"DKA100:",
                       (char *)"TESTVOL", NULL };
        int rc = run_init(av);
        CHECK(rc == 0, "INITIALIZE DKA100: TESTVOL succeeds against the real unit");
    }

    /* --------------------------------------------------------------
     * B: the SAME backing device now carries a valid home block with the
     * label this command wrote. This is what proves the real store was
     * formatted -- not a local file named "DKA100:".
     * -------------------------------------------------------------- */
    char volname[16];
    uint32_t magic_after = home_block_magic(devpath, volname, sizeof(volname));
    CHECK(magic_after == VMSFS_HOME_MAGIC,
          "after INITIALIZE, the REAL backing device carries a vmsfs home block");
    CHECK(strncmp(volname, "TESTVOL", 7) == 0,
          "the home block on the backing device carries the label INITIALIZE was given");

    /* A local file named "DKA100:" must NOT have been created by the old
     * create-a-file path. */
    {
        char df[64];
        snprintf(df, sizeof(df), "%s/DKA100:", WORKDIR);
        CHECK(stat(df, &st) != 0,
              "INITIALIZE DKA100: created NO bogus local file named \"DKA100:\"");
    }

    /* --------------------------------------------------------------
     * Unresolvable name WITH a size argument -- the exact shape that made the
     * old code fabricate a file and report success. Must fail honestly and
     * leave no file behind.
     * -------------------------------------------------------------- */
    {
        char *av[] = { (char *)init_path(), (char *)"BOGUS999:",
                       (char *)"TESTVOL", (char *)"10", NULL };
        int rc = run_init(av);
        CHECK(rc != 0,
              "INITIALIZE BOGUS999: (unresolvable) FAILS -- it does not fake success");
        CHECK(stat(bogus_file, &st) != 0,
              "INITIALIZE BOGUS999: created NO bogus local file -- no fabricated format");
    }

    vms_kif_close();
    printf("=== test_syssvc_initialize: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
