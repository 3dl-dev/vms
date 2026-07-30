/*
 * test_kmod_ident.c - Executive-enforced identity and privileges (vms-2b8)
 *
 * A privilege mask a process sets on itself is not an access control
 * system, it is an honor system. Before this test the OVMX executive
 * took the privilege mask FROM THE PROCESS at registration
 * (VMS_IOCTL_REGISTER carried an init_privs quadword), and userspace
 * carried identity between processes in VMS_PRIVILEGES, VMS_UIC_GROUP,
 * VMS_UIC_MEMBER and VMS_USERNAME environment variables -- so any
 * process could choose its own UIC and privileges with setenv().
 *
 * THE ONLY TESTS THAT CAN TELL THE DIFFERENCE ARE ADVERSARIAL ONES.
 * A process that sets its own privileges and then reads them back
 * passes perfectly against the facade. So this test is built out of
 * refusals:
 *
 *   - AN UNPRIVILEGED PROCESS ASKS FOR MORE THAN IT WAS GIVEN AND IS
 *     REFUSED (SS$_NOPRIV), and is still refused with VMS_PRIVILEGES=ALL
 *     sitting in its own environment.
 *   - A PRIVILEGED PROCESS THAT DROPS ITS PRIVILEGES CANNOT CLIMB BACK.
 *   - PROCESS A ESTABLISHES AN IDENTITY AND PROCESS B READS IT BACK
 *     (CLAUDE.md Rule 11: a per-process fake passes every single-process
 *     test, and fails this one).
 *
 * Process B is a real credential change, not a flag: it setgid()s and
 * setuid()s away from root, so capable(CAP_SYS_ADMIN) is genuinely
 * false in the executive and the UIC it derives is genuinely different.
 *
 * The two processes are sequenced entirely by blocking pipe reads.
 * There is no sleep anywhere in this file: a test paced by sleeps
 * against an emulated guest is a flaky test, and a flaky test is a
 * broken test.
 *
 * This drives the real userspace client (src/libvmssys/vms_kif.c)
 * against a real /dev/vms, so the client libvms will call is the client
 * under test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "vms_kif.h"

/*
 * Status values -- ORACLE-PINNED by the implementer of vms-2b8 on the
 * reference lab OpenVMS VAX V7.3 node VAX1, 2026-07-30
 * (docs/oracle/vax73-privileges.md §1):
 *   F$MESSAGE(36)   -> %SYSTEM-F-NOPRIV, insufficient privilege or
 *                      object protection violation
 *   F$MESSAGE(1664) -> %SYSTEM-W-NOTALLPRIV, not all requested
 *                      privileges authorized
 * 1664 replaces the 532 this tree carried; F$MESSAGE(532) on the same
 * node is %SYSTEM-F-RESULTOVF.
 */
#define SS_NORMAL       1
#define SS_NOPRIV       36
#define SS_NOTALLPRIV   1664

/*
 * Process B's credentials. Both differ from root, so the executive
 * derives a different UIC AND sees capable(CAP_SYS_ADMIN) == false.
 * The uid is what makes this test adversarial rather than decorative:
 * setgid() alone (as vms-8019's test uses) keeps root's capabilities.
 */
#define B_GID   300
#define B_UID   1001
#define B_UIC   (((uint32_t)B_GID << 16) | (uint32_t)B_UID)

/* The identity process A stamps on itself, and B then reads back.
 * Its group matches B's so B can also resolve A BY NAME, which is
 * scoped to the caller's UIC group. */
#define A_USERNAME  "SYSTEST"
#define A_UIC       (((uint32_t)300 << 16) | 301u)
#define A_PRCNAM    "VMS$IDENT"

/* OPER is deliberately NOT in the mask registration grants, so
 * stamping it proves SETPRV is what permits exceeding an authorization
 * (docs/oracle/vax73-privileges.md §3). */
#define A_PRIVS  (VMS_PRV_M_TMPMBX | VMS_PRV_M_NETMBX | (1ULL << 18))

/* SYSPRV: never granted to anyone here, so it is a clean probe for
 * "did the executive hand out something it should not have". */
#define PRV_M_SYSPRV    (1ULL << 28)

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* What process B reports back before A stamps its identity. */
struct b_report1 {
    uint32_t registered;        /* status of B's REGISTER */
    uint32_t getjpi_status;
    uint32_t uic;               /* UIC the executive derived for B */
    uint32_t vms_pid;
    uint64_t cur_privs;
    uint64_t perm_privs;
    char     username[VMS_USERNAME_SIZE];

    uint32_t setident_escalate; /* B tries to become privileged */
    uint64_t perm_after_escalate;
    char     username_after_escalate[VMS_USERNAME_SIZE];

    uint32_t setprv_unauth;     /* enable SYSPRV, temporary */
    uint32_t chkpriv_sysprv;
    uint32_t setprv_unauth_perm;/* enable SYSPRV, permanent */
    uint64_t perm_after_setprv;
    uint32_t setprv_authorized; /* enable TMPMBX (already authorized) */
    uint32_t setident_weaken;   /* stamp a WEAKER identity: allowed */
};

/* What process B reports after A has stamped its identity: the
 * A-writes / B-reads observation. */
struct b_report2 {
    uint32_t bypid_status;
    char     bypid_username[VMS_USERNAME_SIZE];
    uint32_t bypid_uic;
    uint64_t bypid_perm_privs;

    uint32_t byname_status;
    char     byname_username[VMS_USERNAME_SIZE];
};

/* What A tells B once it has stamped itself. */
struct a_signal {
    uint32_t a_vms_pid;
};

/*
 * process_b - the unprivileged half.
 *
 * Drops root for real, then tries every route to more privilege than
 * the executive gave it.
 */
static int process_b(int rfd, int wfd)
{
    struct b_report1 r1;
    struct b_report2 r2;
    struct a_signal sig;
    struct vms_procinfo info;
    uint64_t prev;

    memset(&r1, 0, sizeof(r1));
    memset(&r2, 0, sizeof(r2));

    /*
     * THE ENVIRONMENT FORGERY, planted BEFORE the credential drop so it
     * is unambiguously present for every call below. These are the four
     * variables that were, until this item, real inputs to OVMX's idea
     * of who a process is (src/vmsdcl/dcl_main.c, dcl_cmd_show.c,
     * src/vmsrms/rms_core.c). If any of them still reached the
     * executive, this process would come out of registration as SYSTEM
     * with every privilege.
     */
    setenv("VMS_PRIVILEGES", "ALL", 1);
    setenv("VMS_USERNAME",   "SYSTEM", 1);
    setenv("VMS_UIC_GROUP",  "1", 1);
    setenv("VMS_UIC_MEMBER", "4", 1);

    /* Order matters: setgid() first, because setuid() away from root
     * would drop the capability needed to change groups. */
    if (setgid(B_GID) != 0)
        _exit(70);
    if (setuid(B_UID) != 0)
        _exit(71);
    /* Belt and braces: if the drop silently failed, the whole test is
     * meaningless, so refuse to continue rather than report a pass. */
    if (getuid() == 0 || geteuid() == 0)
        _exit(72);

    if (vms_kif_open() < 0)
        _exit(73);

    r1.registered = vms_kif_register((uint32_t)getpid());

    memset(&info, 0, sizeof(info));
    r1.getjpi_status = vms_kif_getjpi_self(&info);
    r1.uic        = info.uic;
    r1.vms_pid    = info.vms_pid;
    r1.cur_privs  = info.cur_privs;
    r1.perm_privs = info.perm_privs;
    memcpy(r1.username, info.username, VMS_USERNAME_SIZE);

    /* Escalation attempt 1: claim to be SYSTEM with every privilege. */
    r1.setident_escalate = vms_kif_setident("SYSTEM", A_UIC, ~0ULL);
    memset(&info, 0, sizeof(info));
    (void)vms_kif_getjpi_self(&info);
    r1.perm_after_escalate = info.perm_privs;
    memcpy(r1.username_after_escalate, info.username, VMS_USERNAME_SIZE);

    /* Escalation attempt 2: enable a privilege outside our authorization. */
    r1.setprv_unauth = vms_kif_setprv(PRV_M_SYSPRV, 1, 0, &prev);
    r1.chkpriv_sysprv = vms_kif_chkpriv(PRV_M_SYSPRV);

    /* Escalation attempt 3: widen the AUTHORIZED mask itself. */
    r1.setprv_unauth_perm = vms_kif_setprv(PRV_M_SYSPRV, 1, 1, &prev);
    memset(&info, 0, sizeof(info));
    (void)vms_kif_getjpi_self(&info);
    r1.perm_after_setprv = info.perm_privs;

    /* The legitimate case, oracle-pinned: enabling a privilege that IS
     * authorized needs no SETPRV and must succeed. If this failed the
     * executive would merely be broken rather than secure. */
    r1.setprv_authorized = vms_kif_setprv(VMS_PRV_M_TMPMBX, 1, 0, &prev);

    /* Weakening our own identity is allowed without SETPRV. */
    r1.setident_weaken = vms_kif_setident("GUEST", B_UIC, VMS_PRV_M_TMPMBX);

    if (write(wfd, &r1, sizeof(r1)) != (ssize_t)sizeof(r1))
        _exit(74);

    /* Block until A has stamped its identity. No sleep: the read is
     * the synchronisation. */
    if (read(rfd, &sig, sizeof(sig)) != (ssize_t)sizeof(sig))
        _exit(75);

    /* ---- A-writes / B-reads ---- */
    memset(&info, 0, sizeof(info));
    r2.bypid_status = vms_kif_getjpi_pid(sig.a_vms_pid, &info);
    memcpy(r2.bypid_username, info.username, VMS_USERNAME_SIZE);
    r2.bypid_uic = info.uic;
    r2.bypid_perm_privs = info.perm_privs;

    memset(&info, 0, sizeof(info));
    r2.byname_status = vms_kif_getjpi_prcnam(A_PRCNAM, &info);
    memcpy(r2.byname_username, info.username, VMS_USERNAME_SIZE);

    if (write(wfd, &r2, sizeof(r2)) != (ssize_t)sizeof(r2))
        _exit(76);

    vms_kif_close();
    return 0;
}

int main(void)
{
    int a2b[2], b2a[2];
    pid_t child;
    struct b_report1 r1;
    struct b_report2 r2;
    struct a_signal sig;
    struct vms_procinfo info;
    uint32_t status, a_vms_pid;
    uint64_t prev, expect_reg_privs;

    printf("=== test_kmod_ident: executive-enforced identity ===\n");

    if (vms_kif_open() < 0) {
        printf("  FAIL: cannot open /dev/vms (executive absent)\n");
        printf("=== test_kmod_ident: 0 passed, 1 failed ===\n");
        return 1;
    }
    if (vms_kif_register((uint32_t)getpid()) != SS_NORMAL) {
        printf("  FAIL: VMS_IOCTL_REGISTER rejected\n");
        printf("=== test_kmod_ident: 0 passed, 1 failed ===\n");
        return 1;
    }

    /*
     * TEST FIXTURE, stated plainly: /dev/vms is a miscdevice with no
     * .mode, so it is created 0600 root:root and NO unprivileged
     * process can reach the executive at all. Process B must be able to
     * open it for this test to say anything about enforcement, so the
     * still-root parent opens the door. The executive's own decisions
     * are what is under test, not the device's permission bits -- and
     * the device permission is itself a real gap, reported separately.
     */
    if (chmod("/dev/vms", 0666) != 0) {
        printf("  FAIL: cannot chmod /dev/vms for the unprivileged half\n");
        printf("=== test_kmod_ident: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    /* ----------------------------------------------------------------
     * 1. Registration DERIVES the authorized mask; it does not take one.
     *
     * There is no longer any argument to pass, so the strong statement
     * this makes is about the VALUE: even a CAP_SYS_ADMIN process gets
     * exactly the privileges the executive decided to grant, and not
     * the 0xFFFFFFFFFFFFFFFF that src/ovmx_init/ovmx_init.c used to
     * hand itself.
     * ---------------------------------------------------------------- */
    expect_reg_privs = VMS_PRV_M_ENFORCED | VMS_PRV_M_TMPMBX | VMS_PRV_M_NETMBX;

    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_self(&info);
    CHECK(status == SS_NORMAL, "privileged process reads its own executive row");
    CHECK(info.perm_privs == expect_reg_privs,
          "registration grants the executive's derived mask, not all privileges");
    CHECK(info.cur_privs == info.perm_privs,
          "current privileges start equal to authorized privileges");
    CHECK(info.username[0] == '\0',
          "a registered process has NO user name until one is authenticated");

    CHECK(vms_kif_chkpriv(VMS_PRV_M_SETPRV) == SS_NORMAL,
          "privileged process holds SETPRV");
    CHECK(vms_kif_chkpriv(PRV_M_SYSPRV) == SS_NOPRIV,
          "even a privileged process is NOT given SYSPRV by registration");

    a_vms_pid = info.vms_pid;

    if (pipe(a2b) < 0 || pipe(b2a) < 0) {
        printf("  FAIL: pipe()\n");
        return 1;
    }

    child = fork();
    if (child < 0) {
        printf("  FAIL: fork()\n");
        return 1;
    }
    if (child == 0) {
        close(a2b[1]);
        close(b2a[0]);
        /* Take our own channel rather than the inherited descriptor. */
        vms_kif_close();
        _exit(process_b(a2b[0], b2a[1]));
    }
    close(a2b[0]);
    close(b2a[1]);

    memset(&r1, 0, sizeof(r1));
    if (read(b2a[0], &r1, sizeof(r1)) != (ssize_t)sizeof(r1)) {
        printf("  FAIL: unprivileged half never reported "
               "(credential drop or /dev/vms open failed)\n");
        waitpid(child, NULL, 0);
        printf("=== test_kmod_ident: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    /* ----------------------------------------------------------------
     * 2. The unprivileged process gets the default set -- and the
     *    environment it forged had no effect on any of it.
     * ---------------------------------------------------------------- */
    CHECK(r1.registered == SS_NORMAL,
          "unprivileged process may register with the executive");
    CHECK(r1.getjpi_status == SS_NORMAL,
          "unprivileged process reads its own executive row");
    CHECK(r1.perm_privs == (VMS_PRV_M_TMPMBX | VMS_PRV_M_NETMBX),
          "unprivileged registration grants only TMPMBX|NETMBX");
    CHECK((r1.perm_privs & VMS_PRV_M_ENFORCED) == 0,
          "unprivileged process gets NO enforced privilege (no CMKRNL/CMEXEC/SETPRV)");
    CHECK(r1.uic == B_UIC,
          "UIC comes from the task's real credentials, not VMS_UIC_GROUP/MEMBER");
    CHECK(r1.username[0] == '\0',
          "VMS_USERNAME=SYSTEM in the environment does NOT name the process");

    /* ----------------------------------------------------------------
     * 3. THE CORE REFUSAL: a process cannot grant itself a privilege it
     *    was not given.
     * ---------------------------------------------------------------- */
    CHECK(r1.setident_escalate == SS_NOPRIV,
          "process WITHOUT SETPRV is REFUSED an identity it was not given (SS$_NOPRIV)");
    CHECK(r1.perm_after_escalate == (VMS_PRV_M_TMPMBX | VMS_PRV_M_NETMBX),
          "refused identity leaves the authorized mask untouched");
    CHECK(r1.username_after_escalate[0] == '\0',
          "refused identity leaves the process still unnamed (no partial effect)");

    CHECK(r1.setprv_unauth == SS_NOTALLPRIV,
          "$SETPRV outside the authorized mask reports SS$_NOTALLPRIV");
    CHECK(r1.chkpriv_sysprv == SS_NOPRIV,
          "... and the unauthorized privilege is NOT enabled");
    CHECK(r1.setprv_unauth_perm == SS_NOPRIV,
          "widening the AUTHORIZED mask without SETPRV is refused");
    CHECK(r1.perm_after_setprv == (VMS_PRV_M_TMPMBX | VMS_PRV_M_NETMBX),
          "... and the authorized mask is genuinely unchanged");

    /* The oracle-pinned allow case: without it, the executive would be
     * refusing everything, which proves nothing about enforcement. */
    CHECK(r1.setprv_authorized == SS_NORMAL,
          "$SETPRV WITHIN the authorized mask needs no SETPRV (oracle-pinned)");
    CHECK(r1.setident_weaken == SS_NORMAL,
          "a process may always WEAKEN its own identity");

    /* ----------------------------------------------------------------
     * 4. A stamps an authenticated identity on itself, exceeding its
     *    own authorization -- which only SETPRV permits.
     * ---------------------------------------------------------------- */
    CHECK(vms_kif_setprn(A_PRCNAM) == SS_NORMAL,
          "privileged process names itself for the by-name lookup below");

    status = vms_kif_setident(A_USERNAME, A_UIC, A_PRIVS);
    CHECK(status == SS_NORMAL,
          "process WITH SETPRV may establish an authenticated identity");

    memset(&info, 0, sizeof(info));
    (void)vms_kif_getjpi_self(&info);
    CHECK(strcmp(info.username, A_USERNAME) == 0,
          "the executive holds the stamped user name");
    CHECK(info.uic == A_UIC,
          "the executive holds the stamped UIC");
    CHECK(info.perm_privs == A_PRIVS,
          "the executive holds the stamped authorized mask");
    CHECK((info.perm_privs & (1ULL << 18)) != 0,
          "SETPRV permitted granting OPER, which registration had not granted");

    /* ----------------------------------------------------------------
     * 5. THE SECOND CORE REFUSAL: the drop is one-way.
     * ---------------------------------------------------------------- */
    CHECK(vms_kif_chkpriv(VMS_PRV_M_SETPRV) == SS_NOPRIV,
          "SETPRV is gone after stamping an identity that does not include it");

    status = vms_kif_setident("SYSTEM", 0, ~0ULL);
    CHECK(status == SS_NOPRIV,
          "a process that dropped SETPRV cannot climb back (SS$_NOPRIV)");

    memset(&info, 0, sizeof(info));
    (void)vms_kif_getjpi_self(&info);
    CHECK(strcmp(info.username, A_USERNAME) == 0 && info.perm_privs == A_PRIVS,
          "the refused climb-back changed nothing");

    CHECK(vms_kif_setprv(PRV_M_SYSPRV, 1, 0, &prev) == SS_NOTALLPRIV,
          "the de-privileged process cannot enable SYSPRV either");

    /* ----------------------------------------------------------------
     * 6. A-WRITES / B-READS. The check a per-process fake cannot pass.
     * ---------------------------------------------------------------- */
    sig.a_vms_pid = a_vms_pid;
    if (write(a2b[1], &sig, sizeof(sig)) != (ssize_t)sizeof(sig)) {
        printf("  FAIL: could not signal the unprivileged half\n");
        fail++;
    }

    memset(&r2, 0, sizeof(r2));
    if (read(b2a[0], &r2, sizeof(r2)) != (ssize_t)sizeof(r2)) {
        printf("  FAIL: unprivileged half never reported the cross-process read\n");
        fail++;
    } else {
        CHECK(r2.bypid_status == SS_NORMAL,
              "another process resolves A's row in the executive");
        CHECK(strcmp(r2.bypid_username, A_USERNAME) == 0,
              "A WRITES the user name, B READS it (identity is executive-resident)");
        CHECK(r2.bypid_uic == A_UIC,
              "B sees the UIC A established, not the one A registered with");
        CHECK(r2.bypid_perm_privs == A_PRIVS,
              "B sees A's authorized privilege mask");
        CHECK(r2.byname_status == SS_NORMAL &&
              strcmp(r2.byname_username, A_USERNAME) == 0,
              "B resolves A BY NAME within the UIC group and sees the same identity");
    }

    waitpid(child, NULL, 0);
    vms_kif_close();

    printf("=== test_kmod_ident: %d passed, %d failed ===\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
