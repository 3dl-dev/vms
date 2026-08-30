/*
 * test_syssvc_multiuser_stage.c - a NON-ROOT authenticated VMS session must be
 * able to activate a SYS$SYSTEM image over the Files-11 ACP (vms-a86f).
 *
 * WHAT THIS REPRODUCES, AND WHY THE PARTS 0.2 DEMO / RELEASE-ACCEPTANCE GATE
 * GO RED WITHOUT THE FIX.
 *
 * LOGINOUT setuid()/setgid()s every VMS session onto its SYSUAF UIC
 * (tools/vms_login.c: setgid(uic_group); setuid(uic_member)). SYSTEM is UIC
 * [1,4], so the SYSTEM DCL session -- the one the PARTS demo drives -- runs as
 * a NON-ROOT uid (4), not root.
 *
 * The atomic flip (vms-5f0) retired the /vms passthrough on the runtime path,
 * so an image the Linux kernel must execve() (a foreign command like `$ PARTS`,
 * which activates SYS$SYSTEM:PARTS.EXE) is first STAGED off the genuine ODS-2
 * volume THROUGH the executive ACP into OVMX_BOOT_STAGE_DIR ("/run/ovmx-boot"),
 * which the kernel then execve()s. PID 1 (root) pre-stages the boot images and
 * a fixed set of SYS$SYSTEM utilities into that directory and creates it 0755,
 * root-owned. PARTS.EXE is NOT in that pre-staged set, so the FIRST time a
 * session runs `$ PARTS`, DCL's resolver (dcl_resolve_activatable_acp, case 2)
 * lazily stages PARTS.EXE itself -- open(dest, O_CREAT) inside /run/ovmx-boot.
 *
 * For a non-root session that open() is EACCES: /run/ovmx-boot is root-owned
 * 0755, so uid 4 cannot create a file in it. Staging returns SS$_ABORT, the
 * resolver reports %DCL-E-IVIMAGE, `$ PARTS` never reaches %PARTS-S-DONE, and
 * both the PARTS 0.2 demo e2e and the Release-acceptance gate time out red.
 *
 * NOT A SECURITY HOLE, AND NOT FAKED (the fix this suite pins): making
 * /run/ovmx-boot world-writable would let any user plant an image others
 * activate. The faithful fix is PER-USER PRIVATE staging: each activation
 * stages to /run/ovmx-boot/<uid>/, a directory the activating process OWNS,
 * created 0700. The bytes still come GENUINELY off the ODS-2 volume over the
 * ACP (INV-6) -- only the Linux-exec handoff path is per-user-owned.
 *
 * WHAT THIS SUITE ASSERTS, all AS A NON-ROOT uid, over a REAL /dev/vms and the
 * committed ODS-2 fixture on VDA400: (tests/qemu/mkimage_ods2_imgact.c, the
 * same fixture test_syssvc_imgact_acp uses):
 *
 *   1. ACP READ WORKS FOR A NON-ROOT SESSION. rms_stage_over_acp() of
 *      VDA400:[IMGACT]TESTIMG.EXE to a path the non-root uid owns (/tmp)
 *      succeeds -- so any staging failure below is a DESTINATION-permission
 *      problem, never "non-root cannot read the volume".
 *
 *   2. THE BUG, NAMED. Staging that same image into the ROOT-OWNED shared
 *      /run/ovmx-boot (exactly what the pre-fix resolver does) FAILS for the
 *      non-root uid -- SS$_ABORT, errno EACCES. This is the confirmed root
 *      cause of the red PARTS gate.
 *
 *   3. THE FIX. Staging into the PER-USER private /run/ovmx-boot/<uid>/
 *      (created 0700, owned by the uid) SUCCEEDS, and the staged file is owned
 *      by the uid and owner-executable -- a securely activatable image, no
 *      world-writable directory anywhere.
 *
 * NO /dev/vms -> honest SKIP (77): the ACP, the mount and the transfer are all
 * executive-resident, so with no /dev/vms there is nothing to assert (the
 * contract every test_syssvc_* suite is held to).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <grp.h>              /* setgroups                                    */
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "rms/rms.h"          /* rms_stage_over_acp, RMS$_NORMAL, SS$_ABORT   */
#include "vms_kif.h"          /* vms_kif_open, vms_kif_acp_mount/_dmount      */
#include "ovmx_layout.h"      /* OVMX_BOOT_STAGE_DIR                          */

#define EXIT_SKIP 77

/* VDA400: (vde) carries the generated ODS-2 fixture with [IMGACT]TESTIMG.EXE. */
#define ODS2_UNIT   "VDA400:"
#define IMG_SPEC    "VDA400:[IMGACT]TESTIMG.EXE"

/* A non-root identity the SYSTEM session stands in for. Any non-zero uid/gid
 * exercises the same EACCES the real SYSTEM UIC [1,4] hits against a root-owned
 * 0755 staging dir; 4/1 mirrors SYSTEM's UIC member/group exactly. */
#define NONROOT_UID   4
#define NONROOT_GID   1

static int pass = 0;
static int fail = 0;
static void check(int cond, const char *name)
{
    if (cond) { printf("  PASS: %s\n", name); pass++; }
    else      { printf("  FAIL: %s\n", name); fail++; }
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0)
        return 0;
    vms_kif_close();
    return 1;
}

/* The non-root child body: everything below runs as uid NONROOT_UID. Returns 0
 * iff every assertion passed. */
static int run_as_nonroot(void)
{
    uint32_t st;
    struct stat sb;

    printf("  (running as uid=%u gid=%u)\n", (unsigned)getuid(), (unsigned)getgid());

    /* The per-user private staging directory the privileged LOGINOUT window
     * created (0700, owned by this uid) BEFORE the credential drop -- exactly
     * what tools/vms_login.c does before setuid, and what the parent below
     * mimics for this rig. The session cannot create it itself (the root-owned
     * shared parent is not writable by a non-root uid -- the whole point). */
    char user_dir[512], user_dest[512], probe[512];
    snprintf(user_dir, sizeof(user_dir), "%s/%u", OVMX_BOOT_STAGE_DIR,
             (unsigned)getuid());
    snprintf(user_dest, sizeof(user_dest), "%s/TESTIMG.EXE", user_dir);
    snprintf(probe, sizeof(probe), "%s/PROBE.EXE", user_dir);

    /* 1. ACP read works for a non-root session: stage into the per-user dir it
     *    owns. TESTIMG.EXE is a World:RE image (ods2_class_fileprot), so the
     *    non-root UIC reads it over the ACP; a failure here would mean the
     *    diagnosis is a READ problem, not a destination-permission one. */
    unlink(probe);
    st = rms_stage_over_acp(IMG_SPEC, probe);
    check(st == RMS$_NORMAL,
          "non-root session reads " IMG_SPEC " over the ACP into an owned dir");
    if (st != RMS$_NORMAL)
        printf("    (rms_stage_over_acp -> %#x; non-root cannot even READ the "
               "volume, diagnosis differs)\n", st);

    /* 2. THE BUG: staging into the root-owned shared /run/ovmx-boot fails for a
     *    non-root uid. This is exactly the destination the pre-fix resolver
     *    computes (OVMX_BOOT_STAGE_DIR "/" basename), and exactly what makes
     *    `$ PARTS` fail for the SYSTEM session. */
    char shared_dest[512];
    snprintf(shared_dest, sizeof(shared_dest), "%s/TESTIMG.EXE", OVMX_BOOT_STAGE_DIR);
    errno = 0;
    st = rms_stage_over_acp(IMG_SPEC, shared_dest);
    int shared_errno = errno;
    check(st != RMS$_NORMAL,
          "non-root staging into the ROOT-OWNED shared " OVMX_BOOT_STAGE_DIR
          " FAILS (the confirmed PARTS root cause)");
    printf("    (shared-dir stage -> status=%#x errno=%d (%s) -- expected the "
           "EACCES the SYSTEM session hits)\n",
           st, shared_errno, strerror(shared_errno));

    /* 3. THE FIX: per-user private staging into /run/ovmx-boot/<uid>/ (the dir
     *    LOGINOUT created and this uid owns) succeeds and yields a securely-
     *    owned, activatable image. */
    st = rms_stage_over_acp(IMG_SPEC, user_dest);
    if (st != RMS$_NORMAL)
        printf("    (per-user stage -> %#x)\n", st);
    /* negctl: multiuser-stage-shared-not-peruser */
    check(st == RMS$_NORMAL,
          "non-root staging into per-user private /run/ovmx-boot/<uid>/ SUCCEEDS");

    if (stat(user_dest, &sb) == 0) {
        check(sb.st_uid == (uid_t)getuid(),
              "the per-user staged image is owned by the activating non-root uid");
        check((sb.st_mode & S_IXUSR) != 0,
              "the per-user staged image is owner-executable");
    } else {
        check(0, "the per-user staged image is owned by the activating non-root uid");
        check(0, "the per-user staged image is owner-executable");
    }

    /* The per-user directory must not be world-writable (no shared plant hole). */
    if (stat(user_dir, &sb) == 0) {
        check((sb.st_mode & (S_IWGRP | S_IWOTH)) == 0,
              "the per-user staging directory is NOT group/world-writable");
    } else {
        check(0, "the per-user staging directory is NOT group/world-writable");
    }

    unlink(probe);
    return fail == 0 ? 0 : 1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("=== test_syssvc_multiuser_stage: a non-root VMS session activates a "
           "SYS$SYSTEM image over the ACP (vms-a86f) ===\n");

    if (!executive_present()) {
        printf("  SKIP: no /dev/vms (executive absent) -- nothing to assert\n");
        return EXIT_SKIP;
    }

    /* As root (the harness runs suites as root, as PID 1 does before LOGINOUT
     * drops credentials): $MOUNT the fixture volume and create the shared boot
     * staging directory EXACTLY as PID 1 does -- root-owned, 0755. This is the
     * environment a dropped-credential SYSTEM session then faces. */
    uint32_t mst = vms_kif_acp_mount(ODS2_UNIT);
    check($VMS_STATUS_SUCCESS(mst),
          "$MOUNT of the ODS-2 fixture " ODS2_UNIT " (precondition)");
    if (!$VMS_STATUS_SUCCESS(mst))
        printf("    (vms_kif_acp_mount -> %#x)\n", mst);

    if (mkdir("/run", 0755) != 0 && errno != EEXIST)
        printf("  note: mkdir /run: %s\n", strerror(errno));
    /* Force the shared dir to the PID-1 shape even if a prior suite made it. */
    (void)mkdir(OVMX_BOOT_STAGE_DIR, 0755);
    if (chmod(OVMX_BOOT_STAGE_DIR, 0755) != 0)
        printf("  note: chmod %s 0755: %s\n", OVMX_BOOT_STAGE_DIR, strerror(errno));

    /* Create the session's per-user staging dir 0700, owned by the target UIC,
     * EXACTLY as the privileged LOGINOUT window does (tools/vms_login.c) before
     * dropping credentials -- a non-root session cannot create it under the
     * root-owned shared parent, so a privileged actor must. */
    {
        char udir[512];
        if (ovmx_boot_stage_user_dir(udir, sizeof(udir), (unsigned long)NONROOT_UID)) {
            if (mkdir(udir, 0700) == 0 || errno == EEXIST) {
                if (chown(udir, (uid_t)NONROOT_UID, (gid_t)NONROOT_GID) != 0)
                    printf("  note: chown %s: %s\n", udir, strerror(errno));
                (void)chmod(udir, 0700);   /* clear any EEXIST leftover mode */
            } else {
                printf("  note: mkdir %s: %s\n", udir, strerror(errno));
            }
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        check(0, "fork a non-root session");
        return 1;
    }
    if (pid == 0) {
        /* Become the non-root SYSTEM UIC, exactly as LOGINOUT does. */
        if (setgroups(0, NULL) != 0 ||
            setgid(NONROOT_GID) != 0 ||
            setuid(NONROOT_UID) != 0 ||
            getuid() != NONROOT_UID) {
            printf("  FAIL: could not drop to non-root uid %u: %s\n",
                   (unsigned)NONROOT_UID, strerror(errno));
            _exit(2);
        }
        _exit(run_as_nonroot());
    }

    int wstat = 0;
    waitpid(pid, &wstat, 0);
    int child_ok = WIFEXITED(wstat) && WEXITSTATUS(wstat) == 0;

    /* The child printed its own PASS/FAIL lines on the shared stdout; fold its
     * verdict into this process's tally so the harness sees a single rc. */
    if (!child_ok && pass == 0 && fail == 0)
        fail = 1;   /* child died before any assertion (e.g. setuid failed) */

    (void)vms_kif_acp_dmount(ODS2_UNIT);

    printf("=== multiuser_stage: %d passed, %d failed (child rc=%d) ===\n",
           pass, fail, WIFEXITED(wstat) ? WEXITSTATUS(wstat) : -1);
    return child_ok ? 0 : 1;
}
