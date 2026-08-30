/*
 * test_syssvc_acp_mount.c - the Files-11 (ODS-2) ACP $MOUNT VALIDATES a genuine
 * ODS-2 volume and records it EXECUTIVE-GLOBAL (vms-127, epic vms-208), proven
 * against a real /dev/vms.
 *
 * This is the third rung of the executive ACP (after the kernel-resident codec
 * vms-dcd and the channel/mount-table front-end vms-149): $MOUNT stops merely
 * recording a unit name and starts VALIDATING that the unit's backing block
 * device is a genuine Files-11 ODS-2 volume -- reading and checking the home
 * block (LBN 1) and the Storage Control Block with the kernel-resident codec --
 * before recording it, executive-global, in shared state EVERY process sees.
 *
 * WHAT THIS SUITE PROVES, through the PUBLIC sys$/kif API against a real /dev/vms:
 *
 *   1. VALIDATION ACCEPTS GENUINE ODS-2. $MOUNT of VDA100: -- a real-VAX ODS-2
 *      volume the harness seeds on vdb (tests/qemu/run_tests.sh, from the staged
 *      /ods2_real.img fixture) -- SUCCEEDS: the executive read the home block +
 *      SCB off the device and they validated ("DECFILE11B  ", structure level
 *      0x0201, checksums).
 *
 *   2. VALIDATION REJECTS NON-ODS-2 MEDIA, FAIL-HONEST. $MOUNT of VDA0: -- a
 *      BLANK 16M virtio disk (vda), all-zero home block -- is REFUSED with
 *      SS$_DEVNOTMOUNT and NOT recorded. This is the INV-6 heart of the rung: a
 *      mount that admitted a non-ODS-2 blob would be the "report success while
 *      sharing nothing" facade CLAUDE.md Rule 9 exists to kill. The CONTRAST
 *      with (1) -- same code, opposite verdict on different media -- is what
 *      proves the executive actually reads and validates, not just records a
 *      name. The negctl acp-mount-nonods2-accepted injects exactly the mutation
 *      (skip validation) this assertion catches.
 *
 *   3. THE MOUNT IS EXECUTIVE-GLOBAL. A SECOND, unrelated process -- a fork that
 *      holds no channel, fd, or pipe to the volume, and never calls $MOUNT
 *      itself -- $ASSIGNs VDA100: and gets an executive file channel, because it
 *      sees the SAME mounted volume the FIRST process mounted. If the mount were
 *      per-process (the userspace OVMX_SYSDISK_DEV passthrough this rung
 *      deletes), the child's $ASSIGN would fail (SS$_DEVNOTMOUNT). Nothing crosses
 *      between the two processes but the unit NAME and the executive that holds
 *      the mount.
 *
 * NO /dev/vms -> honest SKIP (77), never a fake pass: with no executive the
 * mount table does not exist, so there is nothing to assert -- the contract
 * every test_syssvc_* suite is held to (.github/workflows/ci.yml).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77

/*
 * VDA0: (vda) is the genuine ODS-2 SYSTEM disk; VDA100: (vdb) is blank (non-ODS-2).
 * The ODS-2 unit is VDA0: because $ASSIGN routes only the boot unit
 * (VDA0:/SYS$SYSDEVICE) to the ACP today (src/libvms/syssvc/sys_assign.c), so the
 * cross-process $ASSIGN proof must ride the boot unit; the blank/reject media is
 * therefore VDA100:.
 */
#define ODS2_UNIT  "VDA0:"
#define BLANK_UNIT "VDA100:"

/* Internal channel-class reader (declared extern exactly as the channel suite
 * and sys_qio.c's siblings are): proves the child's $ASSIGN handed back the
 * EXECUTIVE's file channel, not a Linux-fd-backed one. */
extern int vms$$chan_is_file(uint16_t chan);

static int pass = 0;
static int fail = 0;

static void check(int cond, const char *name)
{
    if (cond) { printf("  PASS: %s\n", name); pass++; }
    else      { printf("  FAIL: %s\n", name); fail++; }
}

static struct dsc$descriptor_s mkdsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0)
        return 0;
    vms_kif_close();
    return 1;
}

/* Read exactly `len` bytes or fail (bounded by the parent's waitpid). */
static int read_exact(int fd, void *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, (char *)buf + got, len - got);
        if (n <= 0)
            return -1;
        got += (size_t)n;
    }
    return 0;
}
static int send_token(int fd, char tok) { return write(fd, &tok, 1) == 1 ? 0 : -1; }

/* The child's report back to the parent: what its $ASSIGN of the parent-mounted
 * volume saw. Fixed-width so the layout is identical across the fork. */
struct child_rep {
    uint32_t assign_status;   /* sys$assign status the child observed */
    uint32_t chan_nonzero;    /* 1 iff a channel number was returned */
    uint32_t chan_is_file;    /* 1 iff it was an executive file channel */
};

/*
 * The SECOND process, entered via a fork()+execl() RE-EXEC (argv "--child" mode)
 * so it is a genuinely separate image with its OWN executive registration --
 * sharing nothing with the parent but the unit NAME and the coordination pipes.
 * A plain fork() would inherit the parent's /dev/vms connection, which would
 * make "a second process sees it" indistinguishable from the parent seeing its
 * own mount; the re-exec is what makes the executive-global claim real (the same
 * discipline test_syssvc_mbx_crossproc.c uses). It never mounts anything: it
 * waits for the parent to signal "VDA100: is mounted", then $ASSIGNs VDA100: and
 * reports what it saw. Success here can ONLY come from seeing the parent's
 * executive-global mount.
 */
static int run_child(int a2b_read, int b2a_write)
{
    struct dsc$descriptor_s dev = mkdsc(ODS2_UNIT);
    struct child_rep rep;
    uint16_t chan = 0;
    char go = 0;

    memset(&rep, 0, sizeof(rep));

    /* A re-exec'd gcc image has no per-process PCB until it makes one. */
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        (void)!write(b2a_write, &rep, sizeof(rep));
        return 1;
    }

    /* Wait for the parent to finish mounting VDA100:. */
    if (read_exact(a2b_read, &go, 1) != 0 || go != 'M') {
        (void)!write(b2a_write, &rep, sizeof(rep));
        return 1;
    }

    rep.assign_status = sys$assign(&dev, &chan, 0, NULL);
    rep.chan_nonzero  = (chan != 0) ? 1u : 0u;
    rep.chan_is_file  = vms$$chan_is_file(chan) ? 1u : 0u;
    if (rep.assign_status == SS$_NORMAL && chan != 0)
        (void)sys$dassgn(chan);

    if (write(b2a_write, &rep, sizeof(rep)) != (ssize_t)sizeof(rep))
        return 1;
    return 0;
}

int main(int argc, char **argv)
{
    struct dsc$descriptor_s ods2 = mkdsc(ODS2_UNIT);
    uint32_t st;
    int a2b[2], b2a[2];
    pid_t pid;

    setvbuf(stdout, NULL, _IOLBF, 0);
    /* A peer exit must not take us down with SIGPIPE on the pipe. */
    signal(SIGPIPE, SIG_IGN);

    /* Re-exec'd second-process mode (the executive-global proof's other half). */
    if (argc >= 4 && strcmp(argv[1], "--child") == 0)
        return run_child(atoi(argv[2]), atoi(argv[3]));

    printf("=== test_syssvc_acp_mount: executive ACP validates + mounts a "
           "genuine ODS-2 volume, executive-global (vms-127, epic vms-208) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    if (!executive_present()) {
        printf("=== test_syssvc_acp_mount: 0 passed, 0 failed "
               "(SKIPPED: no /dev/vms -- ODS-2 validation and the mounted-volume "
               "table are executive-resident) ===\n");
        return EXIT_SKIP;
    }

    /* --- (1) validation ACCEPTS a genuine ODS-2 volume -------------------- */
    st = vms_kif_acp_mount(ODS2_UNIT);
    check($VMS_STATUS_SUCCESS(st),
          "$MOUNT of the genuine ODS-2 " ODS2_UNIT " validates (home block + SCB) and records it");

    /* Idempotent: a second mount of an already-mounted (validated) unit is
     * success and does not re-read the disk. */
    st = vms_kif_acp_mount(ODS2_UNIT);
    check($VMS_STATUS_SUCCESS(st),
          "$MOUNT of an already-mounted ODS-2 unit is idempotent (SS$_NORMAL)");

    /* --- (2) validation REJECTS non-ODS-2 media, fail-honest -------------- */
    /* The assertion text below is a contiguous literal (not a macro concat) so
     * facility_defects.sh coverage can match it against the defect's require_fail. */
    st = vms_kif_acp_mount(BLANK_UNIT);
    /* negctl: acp-mount-nonods2-accepted */
    check(st == SS$_DEVNOTMOUNT,
          "$MOUNT of the BLANK VDA100: is REJECTED SS$_DEVNOTMOUNT -- non-ODS-2 media, not recorded");

    /* And the reject really did not record it: $DISMOUNT of the blank unit is
     * SS$_NOSUCHDEV, exactly as for a unit that was never mounted. (Via the KIF
     * $DISMOUNT rather than $ASSIGN, because sys$assign routes only the boot unit
     * to the ACP today -- see the ODS2_UNIT note above.) */
    st = vms_kif_acp_dmount(BLANK_UNIT);
    check(st == SS$_NOSUCHDEV,
          "$DISMOUNT of the rejected VDA100: is SS$_NOSUCHDEV (the reject recorded nothing)");

    /* --- (3) the mount is EXECUTIVE-GLOBAL: a second process sees it ------ */
    if (pipe(a2b) < 0 || pipe(b2a) < 0) {
        printf("  FAIL: pipe() failed\n");
        return 1;
    }
    pid = fork();
    if (pid < 0) {
        printf("  FAIL: fork() failed\n");
        return 1;
    }
    if (pid == 0) {
        /* Re-exec so the second process is a genuinely separate image with its
         * OWN executive registration (sharing nothing with the parent but the
         * NAME + the pipes). Pipe fds survive execl (not close-on-exec). */
        char wfd[16], rfd[16];
        close(a2b[1]);
        close(b2a[0]);
        snprintf(rfd, sizeof(rfd), "%d", a2b[0]);
        snprintf(wfd, sizeof(wfd), "%d", b2a[1]);
        execl(argv[0], argv[0], "--child", rfd, wfd, (char *)NULL);
        _exit(1);
    }

    /* Parent. VDA100: is already mounted (step 1). Tell the child to go. */
    close(a2b[0]);
    close(b2a[1]);
    (void)send_token(a2b[1], 'M');

    {
        struct child_rep rep;
        int wstatus = 0;
        int got = read_exact(b2a[0], &rep, sizeof(rep));
        waitpid(pid, &wstatus, 0);

        check(got == 0, "the second process reported back");
        check(got == 0 && rep.assign_status == SS$_NORMAL,
              "a SECOND process $ASSIGNs " ODS2_UNIT " and sees the SAME mounted volume (executive-global, not per-process)");
        check(got == 0 && rep.chan_nonzero == 1,
              "the second process's $ASSIGN returned a real channel number");
        check(got == 0 && rep.chan_is_file == 1,
              "the second process's channel is an EXECUTIVE file channel, not a Linux fd");
    }

    close(a2b[1]);
    close(b2a[0]);

    /* --- cleanup: $DISMOUNT the ODS-2 volume ----------------------------- */
    st = vms_kif_acp_dmount(ODS2_UNIT);
    check($VMS_STATUS_SUCCESS(st), "$DISMOUNT removes the ODS-2 volume from the executive table");

    /* After dismount, $ASSIGN of the ODS-2 unit is fail-honest again. */
    {
        uint16_t chan = 0;
        st = sys$assign(&ods2, &chan, 0, NULL);
        check(st == SS$_DEVNOTMOUNT,
              "after $DISMOUNT, $ASSIGN of " ODS2_UNIT " is SS$_DEVNOTMOUNT again (volume gone)");
    }

    printf("=== test_syssvc_acp_mount: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
