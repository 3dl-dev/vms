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
 *   - AN UNPRIVILEGED PROCESS CANNOT NAME ITSELF A USER.
 *   - A PROCESS CANNOT READ THE IDENTITY OF A PROCESS OUTSIDE ITS UIC
 *     GROUP WITHOUT THE PRIVILEGE THE ORACLE SAYS IS REQUIRED.
 *   - PROCESS A ESTABLISHES AN IDENTITY AND PROCESS B READS IT BACK
 *     (CLAUDE.md Rule 11: a per-process fake passes every single-process
 *     test, and fails this one).
 *   - A SECOND THREAD OF A SEES A'S IDENTITY, because on VMS a process has
 *     one PCB shared by every thread. A per-THREAD row is the same facade
 *     one layer in, and passes every single-threaded test here.
 *
 * EACH REFUSAL RULE IS PROVED THROUGH A PROBE THAT ONLY THAT RULE CAN
 * REFUSE. The executive's grant rule has three clauses -- the requested
 * privilege mask must be a subset of the caller's, the requested UIC must
 * be the caller's own, AND the requested user name must be the caller's
 * own -- and a probe that violates all three cannot fail on any one of
 * them: delete a clause and such a test stays green. So every clause gets
 * a probe that varies EXACTLY ONE field against a call that is otherwise
 * known to be allowed.
 *
 * THE THREE PROCESSES, and why there are three:
 *   A  the parent. Starts root, so the executive derives a privileged
 *      mask for it; later stamps an authenticated identity in UIC group
 *      300 and thereby drops SETPRV and WORLD.
 *   B  a real credential change, not a flag: setgid(300)+setuid(1001), so
 *      capable(CAP_SYS_ADMIN) is genuinely false and the derived UIC is
 *      genuinely different. Same UIC GROUP as A's stamped identity, which
 *      is the case the oracle says needs no privilege.
 *   D  a process in a DIFFERENT UIC group (400) carrying a real user
 *      name. Without it there is nothing in the table that a
 *      cross-group rule could refuse, and the rule cannot be tested at
 *      all -- which is exactly how the missing check survived round 2.
 *
 * The three processes are sequenced entirely by blocking pipe reads.
 * There is no sleep anywhere in this file: a test paced by sleeps
 * against an emulated guest is a flaky test, and a flaky test is a
 * broken test.
 *
 * This drives the real userspace client (src/libvmssys/vms_kif.c)
 * against a real /dev/vms, so the client libvms will call is the client
 * under test.
 */

#include <pthread.h>
#include <signal.h>
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
/* SS$_NONEXPR -- the wildcard-scan terminator, same value the process
 * table tests already use (tests/qemu/test_kmod_procnam.c). */
#define SS_NONEXPR      2280

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

/* SYSTEM's UIC [1,4]. Used ONLY as the target of the UIC-alone escalation
 * probe below -- it is the UIC an attacker actually wants. */
#define SYS_UIC     (((uint32_t)1 << 16) | 4u)

/*
 * PROCESS D -- the out-of-group process, and the only reason the
 * cross-group rule is testable at all.
 *
 * D registers while still root (so it holds SETPRV and may establish an
 * identity that is not a weakening of its own), stamps a REAL user name
 * in UIC group 400, and only then does a real setgid()+setuid(). So by
 * the time anything reads it, D is a genuinely unprivileged Linux task
 * whose executive row holds a user name and a UIC in a group nobody else
 * here belongs to.
 */
#define D_GID       400
#define D_UID       1003
#define D_UIC       (((uint32_t)D_GID << 16) | (uint32_t)D_UID)
#define D_USERNAME  "OUTSIDER"
#define D_PRCNAM    "VMS$OUTGRP"
#define D_PRIVS     (VMS_PRV_M_TMPMBX | VMS_PRV_M_NETMBX)

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

    /*
     * THE TWO CLAUSES OF THE GRANT RULE, ISOLATED.
     *
     * vms_ioctl_setident refuses a caller without SETPRV on EITHER of two
     * grounds: the requested authorized mask is not a subset of the
     * caller's, OR the requested UIC is not the caller's. setident_escalate
     * above trips BOTH at once, so it cannot fail on one of them -- delete
     * either clause from the executive and that assertion still passes.
     * (Measured: deleting `|| args.uic != cur_uic` left the whole suite at
     * 36 passed / 0 failed.) A guard whose removal is invisible to the test
     * that exists to prove it is an untested guard.
     *
     * So each clause gets a probe that can ONLY be refused by that clause:
     *   uic_only  - asks for EXACTLY the mask it already holds, with
     *               SYSTEM's UIC. The subset test passes by construction,
     *               so only the UIC clause can refuse it.
     *   priv_only - asks for its OWN UIC, with one extra privilege. The
     *               UIC test passes by construction, so only the subset
     *               clause can refuse it.
     * Each records the resulting identity as well as the status: a refusal
     * that still applied half the change would be worse than an allow.
     */
    uint32_t setident_uic_only;
    uint32_t uic_after_uic_only;
    uint64_t perm_after_uic_only;
    char     username_after_uic_only[VMS_USERNAME_SIZE];

    uint32_t setident_priv_only;
    uint32_t uic_after_priv_only;
    uint64_t perm_after_priv_only;
    char     username_after_priv_only[VMS_USERNAME_SIZE];

    /*
     * THE USER NAME CLAUSE, ISOLATED -- and this is the round-2 exploit
     * verbatim. The two probes above vary the UIC and the mask; the name
     * was varied by NOTHING, and the executive memcpy'd it
     * unconditionally. So a process that had done a real setuid() off
     * root could pass its OWN uic and its OWN mask -- both existing
     * clauses satisfied by construction -- with the name "SYSTEM", and
     * the executive reported it as SYSTEM to every reader afterwards.
     * One string literal was the whole attack.
     */
    uint32_t setident_name_only;
    uint32_t uic_after_name_only;
    uint64_t perm_after_name_only;
    char     username_after_name_only[VMS_USERNAME_SIZE];

    uint32_t setprv_unauth;     /* enable SYSPRV, temporary */
    uint32_t chkpriv_sysprv;
    uint32_t setprv_unauth_perm;/* enable SYSPRV, permanent */
    uint64_t perm_after_setprv;
    uint32_t setprv_authorized; /* enable TMPMBX (already authorized) */
    /*
     * An UNNAMED process weakening its own mask through SETIDENT. This
     * used to be the allow case; it is now a refusal, and the change is
     * the point rather than a regression. Stamping ANY name is a change
     * of name for a process whose name is "", and a process may not
     * change its own name. The allow case for a caller without SETPRV
     * moved to process A, which HAS a name to hold constant (section 4c).
     */
    uint32_t setident_weaken;
};

/* What process B reports after A has stamped its identity: the
 * A-writes / B-reads observation, plus the cross-group refusals. */
struct b_report2 {
    uint32_t bypid_status;
    char     bypid_username[VMS_USERNAME_SIZE];
    uint32_t bypid_uic;
    uint64_t bypid_perm_privs;

    uint32_t byname_status;
    char     byname_username[VMS_USERNAME_SIZE];

    /* Reading D, which is in UIC group 400 while B is in 300. */
    uint32_t outgrp_bypid_status;
    char     outgrp_bypid_username[VMS_USERNAME_SIZE];
    uint32_t outgrp_bypid_uic;

    /*
     * What the whole-table enumeration hands an unprivileged caller.
     * In round 2 this handed back every field of every row: the
     * adversary's probe read uic=0x00000000 user='SYSTEM' out of a
     * process it had no relationship with.
     */
    uint32_t scan_rows;             /* rows returned before SS$_NONEXPR */
    uint32_t scan_terminator;
    uint32_t scan_saw_a;            /* A's row appeared */
    char     scan_a_username[VMS_USERNAME_SIZE];
    uint32_t scan_a_uic;
    uint32_t scan_saw_d;            /* D's row appeared (by process name) */
    char     scan_d_username[VMS_USERNAME_SIZE];
    uint32_t scan_d_uic;
    uint64_t scan_d_perm_privs;
    uint32_t scan_d_vms_pid;
    /*
     * D's terminal, as B's unprivileged scan sees it (vms-d0b). D binds
     * a real one (see process_d()) so this cell has something to leak;
     * the redaction check below only means something because of that.
     */
    char     scan_d_terminal[VMS_DEVNAM_SIZE];
};

/* What process D reports once it is registered, named, identified and
 * genuinely unprivileged. */
struct d_report {
    uint32_t registered;
    uint32_t setprn_status;
    uint32_t setident_status;
    uint32_t vms_pid;
    uint32_t uic;
    char     username[VMS_USERNAME_SIZE];
    /* D's own binding, so the parent can tell "D bound OPA0:" apart
     * from "D bound nothing" before trusting an empty scan cell as a
     * redaction rather than a vacuous check (vms-d0b). */
    uint32_t assign_status;
    uint32_t setterm_status;
};

/* What A tells B once it has stamped itself. */
struct a_signal {
    uint32_t a_vms_pid;
    uint32_t d_vms_pid;
};

/* What a SECOND THREAD of process A sees when it asks who it is. */
struct thread_obs {
    uint32_t status;
    uint32_t vms_pid;
    uint32_t uic;
    uint64_t perm_privs;
    char     username[VMS_USERNAME_SIZE];
};

/*
 * identity_thread - a second thread of process A asks the executive who
 * it is.
 *
 * ONE PCB PER PROCESS (CLAUDE.md Rule 11, one layer in). If the executive
 * keys its process table on the LINUX THREAD id rather than the process,
 * this thread is a different VMS process from the one that created it: it
 * gets its own row, its own privilege mask and its own event flags, and
 * neither thread can see the other's. That fake passes every
 * single-threaded test in this file perfectly, which is exactly why it
 * needs its own probe.
 *
 * The channel is opened HERE and not inherited: vms_dev_fd in
 * src/libvmssys/vms_kif.c is __thread, so a second thread necessarily
 * enters the executive through a second open file. Two channels into the
 * executive from one process must resolve to ONE row.
 */
static void *identity_thread(void *arg)
{
    struct thread_obs *obs = arg;
    struct vms_procinfo info;

    if (vms_kif_open() < 0) {
        obs->status = 0;    /* not a VMS status: "never got in" */
        return NULL;
    }

    memset(&info, 0, sizeof(info));
    obs->status     = vms_kif_getjpi_self(&info);
    obs->vms_pid    = info.vms_pid;
    obs->uic        = info.uic;
    obs->perm_privs = info.perm_privs;
    memcpy(obs->username, info.username, VMS_USERNAME_SIZE);

    vms_kif_close();
    return NULL;
}

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

    r1.registered = vms_kif_register(NULL);

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

    /*
     * Escalation attempt 1a: THE UIC CLAUSE ALONE. Ask for exactly the
     * authorized mask the executive just reported for us -- the subset
     * test cannot possibly reject that -- and take SYSTEM's UIC with it.
     * The UIC is the group-scoping key that find_by_name() and every
     * group-scoped facility read, so a process that can pick its own UIC
     * picks whose processes it can see.
     */
    r1.setident_uic_only = vms_kif_setident("GUEST", SYS_UIC, r1.perm_privs);
    memset(&info, 0, sizeof(info));
    (void)vms_kif_getjpi_self(&info);
    r1.uic_after_uic_only  = info.uic;
    r1.perm_after_uic_only = info.perm_privs;
    memcpy(r1.username_after_uic_only, info.username, VMS_USERNAME_SIZE);

    /*
     * Escalation attempt 1b: THE SUBSET CLAUSE ALONE. Keep our own UIC --
     * the UIC test cannot possibly reject that -- and ask for one
     * privilege we do not hold.
     */
    r1.setident_priv_only =
        vms_kif_setident("GUEST", B_UIC, r1.perm_privs | PRV_M_SYSPRV);
    memset(&info, 0, sizeof(info));
    (void)vms_kif_getjpi_self(&info);
    r1.uic_after_priv_only  = info.uic;
    r1.perm_after_priv_only = info.perm_privs;
    memcpy(r1.username_after_priv_only, info.username, VMS_USERNAME_SIZE);

    /*
     * Escalation attempt 1c: THE USER NAME CLAUSE ALONE, which is the
     * round-2 exploit exactly as the adversary ran it. Our OWN uic --
     * the UIC test cannot reject that. Our OWN authorized mask -- the
     * subset test cannot reject that. Only the name is different, and it
     * is the name every reader displays.
     */
    r1.setident_name_only = vms_kif_setident("SYSTEM", B_UIC, r1.perm_privs);
    memset(&info, 0, sizeof(info));
    (void)vms_kif_getjpi_self(&info);
    r1.uic_after_name_only  = info.uic;
    r1.perm_after_name_only = info.perm_privs;
    memcpy(r1.username_after_name_only, info.username, VMS_USERNAME_SIZE);

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

    /* Weakening our own mask through SETIDENT requires stamping a name,
     * and an unnamed process has no name to stamp: refused. */
    r1.setident_weaken = vms_kif_setident("GUEST", B_UIC, VMS_PRV_M_TMPMBX);

    if (write(wfd, &r1, sizeof(r1)) != (ssize_t)sizeof(r1))
        _exit(74);

    /* Block until A has stamped its identity. No sleep: the read is
     * the synchronisation. */
    if (read(rfd, &sig, sizeof(sig)) != (ssize_t)sizeof(sig))
        _exit(75);

    /* ---- A-writes / B-reads, IN THE SAME UIC GROUP ----
     * Oracle-pinned as needing no privilege: on VAX 7.3 a process with
     * SET PROCESS/PRIVILEGE=(NOALL) read a same-group process's USERNAME
     * successfully (docs/oracle/vax73-privileges.md §5). B holds only
     * TMPMBX|NETMBX, and must still see A. */
    memset(&info, 0, sizeof(info));
    r2.bypid_status = vms_kif_getjpi_pid(sig.a_vms_pid, &info);
    memcpy(r2.bypid_username, info.username, VMS_USERNAME_SIZE);
    r2.bypid_uic = info.uic;
    r2.bypid_perm_privs = info.perm_privs;

    memset(&info, 0, sizeof(info));
    r2.byname_status = vms_kif_getjpi_prcnam(A_PRCNAM, &info);
    memcpy(r2.byname_username, info.username, VMS_USERNAME_SIZE);

    /* ---- ... AND THE SAME READ ACROSS UIC GROUPS, WHICH MUST NOT WORK.
     * D is in group 400, B in group 300, and B holds no WORLD. Oracle:
     * cross-group is SS$_NOPRIV, and GROUP does not help -- only WORLD. */
    memset(&info, 0, sizeof(info));
    r2.outgrp_bypid_status = vms_kif_getjpi_pid(sig.d_vms_pid, &info);
    memcpy(r2.outgrp_bypid_username, info.username, VMS_USERNAME_SIZE);
    r2.outgrp_bypid_uic = info.uic;

    /* ---- ... and the enumeration cannot be used to walk around it. */
    {
        uint32_t index = 0;
        uint32_t st;

        for (;;) {
            memset(&info, 0, sizeof(info));
            st = vms_kif_procscan(&index, &info);
            if (st != SS_NORMAL) {
                r2.scan_terminator = st;
                break;
            }
            r2.scan_rows++;
            if (info.vms_pid == sig.a_vms_pid) {
                r2.scan_saw_a = 1;
                memcpy(r2.scan_a_username, info.username, VMS_USERNAME_SIZE);
                r2.scan_a_uic = info.uic;
            }
            /* D is identified BY ITS PROCESS NAME, because the oracle
             * shows the name of an out-of-group process in SHOW SYSTEM
             * even to a privilege-less caller -- so the row must be
             * findable here, and it is the IDENTITY fields that must be
             * empty. Matching on the name rather than on the pid also
             * means a row that came back completely blank cannot be
             * mistaken for a correctly redacted one. */
            if (strcmp(info.prcnam, D_PRCNAM) == 0) {
                r2.scan_saw_d = 1;
                memcpy(r2.scan_d_username, info.username, VMS_USERNAME_SIZE);
                r2.scan_d_uic = info.uic;
                r2.scan_d_perm_privs = info.perm_privs;
                r2.scan_d_vms_pid = info.vms_pid;
                memcpy(r2.scan_d_terminal, info.terminal, VMS_DEVNAM_SIZE);
            }
            if (r2.scan_rows > 64)
                break;
        }
    }

    if (write(wfd, &r2, sizeof(r2)) != (ssize_t)sizeof(r2))
        _exit(76);

    vms_kif_close();
    return 0;
}

/*
 * process_d - a process in a DIFFERENT UIC group, carrying a real
 * identity.
 *
 * Registers as root so that it holds SETPRV and may legitimately stamp
 * an identity (this is the LOGINOUT shape, done a second time by a
 * second process), then drops its Linux credentials for real so that
 * nothing about it is privileged by the time it is read.
 *
 * It then blocks forever on rfd. Its row has to still be in the table
 * when B scans it, and a process that exits has its row reaped -- so the
 * lifetime is held open by the pipe, not by a sleep.
 */
static int process_d(int rfd, int wfd)
{
    struct d_report rd;
    struct vms_procinfo info;
    char byte;

    memset(&rd, 0, sizeof(rd));

    if (vms_kif_open() < 0)
        _exit(80);

    rd.registered     = vms_kif_register(NULL);
    rd.setprn_status  = vms_kif_setprn(D_PRCNAM);
    rd.setident_status = vms_kif_setident(D_USERNAME, D_UIC, D_PRIVS);

    /*
     * D binds a REAL terminal through the executive (vms-d0b), so its
     * row genuinely has something in the terminal field for the
     * redaction check in main() to catch leaking. Without this, D's
     * terminal cell would be empty regardless of whether redaction
     * works, and the check below would be satisfiable by either the
     * correct behaviour or the defect it exists to catch.
     */
    {
        uint32_t chan = 0;

        rd.assign_status = vms_kif_assign("OPA0:", &chan);
        rd.setterm_status = vms_kif_setterm(chan);
    }

    /* Now become genuinely unprivileged at the Linux level too, so no
     * reader is being refused by a capability check somewhere else. */
    if (setgid(D_GID) != 0)
        _exit(81);
    if (setuid(D_UID) != 0)
        _exit(82);
    if (getuid() == 0 || geteuid() == 0)
        _exit(83);

    memset(&info, 0, sizeof(info));
    (void)vms_kif_getjpi_self(&info);
    rd.vms_pid = info.vms_pid;
    rd.uic     = info.uic;
    memcpy(rd.username, info.username, VMS_USERNAME_SIZE);

    if (write(wfd, &rd, sizeof(rd)) != (ssize_t)sizeof(rd))
        _exit(84);

    /* Stay registered until told to go. */
    if (read(rfd, &byte, 1) != 1)
        _exit(85);

    vms_kif_close();
    return 0;
}

int main(void)
{
    int a2b[2], b2a[2], a2d[2], d2a[2];
    pid_t child, dchild;
    struct b_report1 r1;
    struct b_report2 r2;
    struct d_report rd;
    struct a_signal sig;
    struct vms_procinfo info;
    uint32_t status, a_vms_pid, reg_vms_pid = 0;
    uint64_t prev, expect_reg_privs;

    printf("=== test_kmod_ident: executive-enforced identity ===\n");

    if (vms_kif_open() < 0) {
        printf("  FAIL: cannot open /dev/vms (executive absent)\n");
        printf("=== test_kmod_ident: 0 passed, 1 failed ===\n");
        return 1;
    }
    if (vms_kif_register(&reg_vms_pid) != SS_NORMAL) {
        printf("  FAIL: VMS_IOCTL_REGISTER rejected\n");
        printf("=== test_kmod_ident: 0 passed, 1 failed ===\n");
        return 1;
    }

    /*
     * NO FIXTURE. This used to chmod /dev/vms 0666 so the unprivileged
     * half could reach the executive at all, because the miscdevice
     * carried no .mode and therefore shipped 0600 root:root. A test that
     * has to widen the product's own permissions before it can run is
     * proving enforcement under a posture the product does not have: with
     * a 0600 door every reachable caller is root, and the entire
     * unprivileged half of this proof is unreachable outside the fixture.
     *
     * The module now sets .mode = 0666 deliberately, because on OpenVMS
     * the executive entry sequence is unprivileged for every process and
     * access control lives inside each service (see the comment on
     * vms_misc in src/kernel/vms_module.c). So the posture is asserted
     * here instead of being manufactured: if a later change quietly puts
     * the 0600 door back, this goes red rather than silently reverting the
     * product to single-user while the test keeps passing.
     */
    {
        struct stat st;

        if (stat("/dev/vms", &st) != 0) {
            printf("  FAIL: cannot stat /dev/vms\n");
            printf("=== test_kmod_ident: %d passed, %d failed ===\n", pass, fail + 1);
            return 1;
        }
        CHECK((st.st_mode & 0666) == 0666,
              "the executive entry point is reachable by every process, "
              "as the VMS system-service entry is (no test fixture)");
        if ((st.st_mode & 0666) != 0666) {
            printf("  (mode is %04o -- the unprivileged half below cannot run)\n",
                   (unsigned)(st.st_mode & 07777));
            printf("=== test_kmod_ident: %d passed, %d failed ===\n", pass, fail);
            return 1;
        }
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

    /* ----------------------------------------------------------------
     * 1b. THE VMS PROCESS ID IS THE EXECUTIVE'S, NOT THE PROCESS'S.
     *
     * It used to be an INPUT to registration with no uniqueness check,
     * so an unprivileged process could register claiming a privileged
     * process's VMS PID and $GETJPI by that PID would return whichever
     * row the hash walk reached first. The argument is gone; the
     * executive assigns and reports.
     * ---------------------------------------------------------------- */
    CHECK(reg_vms_pid != 0,
          "registration REPORTS a VMS process ID the executive assigned");
    CHECK(reg_vms_pid == a_vms_pid,
          "... and it is the ID the executive files the process under");
    CHECK(a_vms_pid != (uint32_t)getpid(),
          "the VMS process ID is not a copy of the Linux pid the caller knows");

    if (pipe(a2d) < 0 || pipe(d2a) < 0) {
        printf("  FAIL: pipe()\n");
        return 1;
    }

    /* ----------------------------------------------------------------
     * 1c. Bring up the OUT-OF-GROUP process. Everything about the
     *     cross-group rule below is unprovable without a row that is
     *     genuinely somewhere else.
     * ---------------------------------------------------------------- */
    dchild = fork();
    if (dchild < 0) {
        printf("  FAIL: fork()\n");
        return 1;
    }
    if (dchild == 0) {
        close(a2d[1]);
        close(d2a[0]);
        vms_kif_close();
        _exit(process_d(a2d[0], d2a[1]));
    }
    close(a2d[0]);
    close(d2a[1]);

    memset(&rd, 0, sizeof(rd));
    if (read(d2a[0], &rd, sizeof(rd)) != (ssize_t)sizeof(rd)) {
        printf("  FAIL: out-of-group process never reported\n");
        kill(dchild, SIGKILL);
        waitpid(dchild, NULL, 0);
        printf("=== test_kmod_ident: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    CHECK(rd.registered == SS_NORMAL && rd.setprn_status == SS_NORMAL &&
          rd.setident_status == SS_NORMAL,
          "a second process establishes an identity in another UIC group");
    CHECK(rd.uic == D_UIC && strcmp(rd.username, D_USERNAME) == 0,
          "... and the executive holds it (name and UIC) for that process");
    CHECK(rd.vms_pid != 0 && rd.vms_pid != a_vms_pid,
          "two processes get DIFFERENT VMS process IDs from the executive");

    /* ----------------------------------------------------------------
     * 1d. WORLD ALLOWS THE CROSS-GROUP READ.
     *
     * This process is still in UIC group 0 and still holds the mask
     * registration derived for a CAP_SYS_ADMIN task, which includes
     * WORLD. The identical call is repeated in section 5b after this
     * process has dropped WORLD, and must then be refused -- same
     * caller, same target, same call, only the privilege mask different.
     * That pair is what isolates the WORLD clause: nothing else in the
     * executive differs between the two.
     * ---------------------------------------------------------------- */
    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_pid(rd.vms_pid, &info);
    CHECK(status == SS_NORMAL,
          "WORLD permits reading a process outside the caller's UIC group");
    CHECK(strcmp(info.username, D_USERNAME) == 0 && info.uic == D_UIC,
          "... and returns the whole row (this is what section 5b must lose)");

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

    /*
     * ... and each half of the grant rule proved on its own. Deleting
     * either clause from vms_ioctl_setident must turn exactly one of these
     * two pairs red; if a clause can be deleted and the suite stays green,
     * that clause is not being tested by anything.
     */
    CHECK(r1.setident_uic_only == SS_NOPRIV,
          "UIC CLAUSE ISOLATED: same authorized mask, SYSTEM's UIC -> SS$_NOPRIV");
    CHECK(r1.uic_after_uic_only == B_UIC &&
          r1.perm_after_uic_only == (VMS_PRV_M_TMPMBX | VMS_PRV_M_NETMBX) &&
          r1.username_after_uic_only[0] == '\0',
          "... and the refused UIC change applied nothing at all");

    CHECK(r1.setident_priv_only == SS_NOPRIV,
          "SUBSET CLAUSE ISOLATED: own UIC, one extra privilege -> SS$_NOPRIV");
    CHECK(r1.uic_after_priv_only == B_UIC &&
          r1.perm_after_priv_only == (VMS_PRV_M_TMPMBX | VMS_PRV_M_NETMBX) &&
          r1.username_after_priv_only[0] == '\0',
          "... and the refused privilege grant applied nothing at all");

    CHECK(r1.setident_name_only == SS_NOPRIV,
          "USER NAME CLAUSE ISOLATED: own UIC, own mask, name \"SYSTEM\" -> SS$_NOPRIV");
    CHECK(r1.username_after_name_only[0] == '\0' &&
          r1.uic_after_name_only == B_UIC &&
          r1.perm_after_name_only == (VMS_PRV_M_TMPMBX | VMS_PRV_M_NETMBX),
          "... and the process is STILL UNNAMED (a process cannot name itself a user)");

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
    CHECK(r1.setident_weaken == SS_NOPRIV,
          "an UNNAMED process cannot stamp an identity at all -- there is no "
          "name it is allowed to hold constant");

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
     * 4b. IDENTITY IS PER-PROCESS, NOT PER-THREAD.
     *
     * A second thread of A must see the identity A just stamped -- the
     * same row, the same VMS pid, the same UIC, the same mask. If the
     * executive minted a row per Linux thread, this thread would either
     * be unknown to the executive or be a second, differently-privileged
     * VMS process wearing the same image.
     * ---------------------------------------------------------------- */
    {
        pthread_t th;
        struct thread_obs obs;

        memset(&obs, 0, sizeof(obs));
        if (pthread_create(&th, NULL, identity_thread, &obs) != 0) {
            printf("  FAIL: pthread_create\n");
            fail++;
        } else {
            pthread_join(th, NULL);

            CHECK(obs.status == SS_NORMAL,
                  "a second thread of the process is known to the executive");
            CHECK(obs.vms_pid == a_vms_pid,
                  "... as the SAME process, not a second one (one PCB per process)");
            CHECK(strcmp(obs.username, A_USERNAME) == 0 &&
                  obs.uic == A_UIC && obs.perm_privs == A_PRIVS,
                  "... and sees the identity the other thread stamped");
        }
    }

    /* ----------------------------------------------------------------
     * 4c. THE USER NAME CLAUSE, ISOLATED AGAINST AN ALLOWED CALL.
     *
     * This process now HAS a user name and no longer holds SETPRV, which
     * is the only state in which the name clause can be isolated: the
     * allowed call and the refused call differ in exactly one field.
     *
     *   allowed:  SETIDENT(SYSTEST, A_UIC, A_PRIVS)   -- identity unchanged
     *   refused:  SETIDENT(SYSTEM,  A_UIC, A_PRIVS)   -- ONLY the name moved
     *
     * Delete the name clause from vms_ioctl_setident and the second of
     * these turns green while everything else stays green, which is
     * exactly the state round 2 shipped in.
     * ---------------------------------------------------------------- */
    CHECK(vms_kif_chkpriv(VMS_PRV_M_SETPRV) == SS_NOPRIV,
          "the identity just stamped left this process without SETPRV");

    status = vms_kif_setident(A_USERNAME, A_UIC, A_PRIVS);
    CHECK(status == SS_NORMAL,
          "a process WITHOUT SETPRV may re-stamp the identity it already has");

    status = vms_kif_setident("SYSTEM", A_UIC, A_PRIVS);
    CHECK(status == SS_NOPRIV,
          "USER NAME CLAUSE ISOLATED: same UIC, same mask, different name "
          "-> SS$_NOPRIV");

    memset(&info, 0, sizeof(info));
    (void)vms_kif_getjpi_self(&info);
    CHECK(strcmp(info.username, A_USERNAME) == 0 &&
          info.uic == A_UIC && info.perm_privs == A_PRIVS,
          "... and the executive still holds the name it authenticated");

    /* ----------------------------------------------------------------
     * 5b. WORLD REFUSES THE CROSS-GROUP READ.
     *
     * The identical call made in section 1d, by the identical process,
     * against the identical target. The ONLY thing that changed is that
     * the identity stamped in section 4 does not include WORLD. If the
     * cross-group check is deleted this pair collapses and this goes red
     * while section 1d stays green.
     *
     * The status is oracle-pinned: on VAX 7.3, F$GETJPI against a
     * process in another UIC group with no WORLD returns
     * %SYSTEM-F-NOPRIV, and does so for EVERY item -- so the refusal
     * carries no row at all.
     * ---------------------------------------------------------------- */
    CHECK(vms_kif_chkpriv(VMS_PRV_M_WORLD) == SS_NOPRIV,
          "the stamped identity does not include WORLD");

    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_pid(rd.vms_pid, &info);
    CHECK(status == SS_NOPRIV,
          "WORLD CLAUSE ISOLATED: the same cross-group read, now without "
          "WORLD -> SS$_NOPRIV");
    CHECK(info.username[0] == '\0' && info.uic == 0 && info.perm_privs == 0,
          "... and the refusal returns no part of the row");

    /* The same-group read is NOT refused: oracle-pinned as needing no
     * privilege at all, so a rule that refused everything would be as
     * wrong as one that refused nothing. */
    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_prcnam(A_PRCNAM, &info);
    CHECK(status == SS_NORMAL,
          "a read within the caller's own UIC group still needs no privilege");

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
    sig.d_vms_pid = rd.vms_pid;
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

        /* ------------------------------------------------------------
         * 6b. AND AN UNPRIVILEGED PROCESS CANNOT READ ACROSS UIC GROUPS.
         *
         * B holds TMPMBX|NETMBX and nothing else. D is in group 400.
         * This is the adversary's round-2 probe: it read a row outside
         * its own group and got uic and user name out of it.
         * ------------------------------------------------------------ */
        CHECK(r2.outgrp_bypid_status == SS_NOPRIV,
              "an unprivileged process is REFUSED a process in another UIC group");
        CHECK(r2.outgrp_bypid_username[0] == '\0' && r2.outgrp_bypid_uic == 0,
              "... and gets no part of that process's identity");

        /* ------------------------------------------------------------
         * 6c. THE ENUMERATION IS NOT A WAY AROUND IT.
         *
         * $PROCESS_SCAN used to hand every field of every row to any
         * caller, which made the check in 6b decorative -- refuse the
         * front door, leave the whole table on the windowsill.
         *
         * The split asserted here is oracle-measured, not chosen: a
         * privilege-less caller on VAX 7.3 saw EVERY process in SHOW
         * SYSTEM including a cross-group one WITH ITS PROCESS NAME,
         * while $GETJPI on that same process was refused every item. So
         * D's row must still appear, must still carry its process name,
         * and must carry nothing else -- even though the executive
         * genuinely holds "OUTSIDER" for it, as section 1d proved by
         * reading it with WORLD.
         * ------------------------------------------------------------ */
        CHECK(r2.scan_terminator == SS_NONEXPR,
              "the unprivileged scan terminates with SS$_NONEXPR");
        CHECK(r2.scan_saw_a && strcmp(r2.scan_a_username, A_USERNAME) == 0 &&
              r2.scan_a_uic == A_UIC,
              "the scan returns a SAME-GROUP row in full (no privilege needed)");
        CHECK(r2.scan_saw_d,
              "the scan still LISTS the out-of-group process (VMS shows it)");
        CHECK(r2.scan_d_vms_pid == rd.vms_pid,
              "... under its real VMS process ID");
        CHECK(r2.scan_d_username[0] == '\0' && r2.scan_d_uic == 0 &&
              r2.scan_d_perm_privs == 0,
              "... but WITHOUT its user name, UIC or privilege mask");

        /*
         * TERMINAL REDACTION (vms-d0b). D's terminal field joined the
         * same redacted row as username/uic/perm_privs above
         * (src/kernel/vms_proctab.c, proc_fill_info()) -- until this
         * check, nothing asserted that it actually stayed off a row B
         * may not $GETJPI. The precondition first: D must have
         * genuinely bound OPA0:, or an empty scan_d_terminal proves
         * nothing about redaction at all.
         */
        CHECK(rd.assign_status == SS_NORMAL && rd.setterm_status == SS_NORMAL,
              "D genuinely bound OPA0: as its terminal (precondition -- "
              "an unbound D would make the next check vacuous)");
        CHECK(r2.scan_d_terminal[0] == '\0',
              "TERMINAL REDACTION: the scan withholds D's terminal too, "
              "even though D genuinely bound one -- a caller that may not "
              "read D's identity may not read which terminal D is on either");
    }

    waitpid(child, NULL, 0);

    /* Release the out-of-group process: its row had to outlive every
     * read above, so it was held open by a blocking read, not a sleep. */
    (void)!write(a2d[1], "x", 1);
    waitpid(dchild, NULL, 0);

    vms_kif_close();

    printf("=== test_kmod_ident: %d passed, %d failed ===\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
