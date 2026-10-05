/*
 * test_syssvc_creprc_inherit.c - a $CREPRC/SPAWN SUBPROCESS inherits its
 * creator's executive identity BY CONTINUATION, with a FRESH VMS PID, even when
 * the child is NON-ROOT (vms-19e9), proven against a real /dev/vms.
 *
 * THE BUG THIS PROVES FIXED. The booted runtime's interactive DCL runs NON-ROOT
 * (LOGINOUT setgid/setuid's to the user's UIC after establishing the identity).
 * When that non-root DCL runs SPAWN, sys$creprc forks a child that touches the
 * executive BEFORE image activation. That child used to register FRESH (an empty
 * user name, a capable()-derived unprivileged mask), then try to STAMP the
 * creator's identity onto itself with vms_kif_setident() -- a self-declaration
 * the executive correctly refuses for a non-root caller (SS$_NOPRIV), surfaced
 * as %DCL-F-CREPRC. SPAWN was dead in the runtime.
 *
 * The existing spawn suites (test_syssvc_spawn_users, test_syssvc_libspawn_reg)
 * never caught it because their creator is ROOT and UNNAMED, so the failing
 * branch -- a NAMED, NON-ROOT creator -- is never exercised. This suite drives
 * exactly that condition.
 *
 * The fix: the subprocess registers via VMS_IOCTL_REGISTER_SUBPROCESS
 * (vms_kif_register_subprocess). The executive derives the child's identity from
 * its UNFORGEABLE real_parent -- the creator -- and mints a FRESH, distinct VMS
 * PID (a subprocess is a new VMS process, unlike an image activation, which
 * shares the PID via _CONTINUE). No privileged name is ever self-declared.
 *
 *   A) A NAMED creator (SYSTEM, SYSPRV/SETPRV) forks a child that DROPS to a
 *      non-root Linux credential and registers as a SUBPROCESS. The child holds
 *      the creator's user name, UIC and privileges -- inherited, never asked for
 *      -- AND a VMS PID DISTINCT from the creator's. This is the fix.
 *
 *   B) THE SECURITY HALF, UNCHANGED. A non-root child that registers FRESH and
 *      then tries to self-declare the SYSTEM identity through vms_kif_setident()
 *      is REFUSED (SS$_NOPRIV). The fix changed how a subprocess GETS its
 *      identity (parent-derived continuation); it did NOT weaken the guard that
 *      refuses a non-root self-declared privileged name.
 *
 * A implies a non-root child gained its creator's identity without declaring it;
 * B implies it still cannot declare one for itself. Both must hold at once.
 *
 * Requires a real, insmod'd vms.ko at /dev/vms. With no executive it exits
 * EXIT_SKIP (77), never a fake pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/wait.h>

#include "ssdef.h"
#include "prvdef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77

static int pass = 0;
static int fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* SYSTEM: the identity LOGINOUT stamps on the interactive SYSTEM session. */
#define SYS_NAME   "SYSTEM"
#define SYS_UIC    ((1u << 16) | 4u)
#define SYS_PRIVS  (PRV$M_TMPMBX | PRV$M_NETMBX | PRV$M_SYSPRV | PRV$M_SETPRV)

/* A deliberately UNPRIVILEGED Linux credential the children drop to, so the
 * subprocess and the negative control both run non-root -- exactly the runtime
 * condition (a DCL that setuid'd away from root) the shipped bug needed. */
#define DROP_GID  10
#define DROP_UID  200

static int drop_to_unprivileged(void)
{
    /* setgid BEFORE setuid: once the uid is dropped the gid can no longer be
     * changed. Best-effort -- if the harness is already non-root the property
     * under test (a non-root child) still holds. */
    if (setgid(DROP_GID) != 0 && geteuid() == 0)
        return -1;
    if (setuid(DROP_UID) != 0 && geteuid() == 0)
        return -1;
    return 0;
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0)
        return 0;
    vms_kif_close();
    return 1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_creprc_inherit ($CREPRC subprocess inherits identity by continuation, vms-19e9) ===\n");

    if (!executive_present()) {
        printf("  INFO: cannot open /dev/vms -- CI executive-absent rig, not the product\n");
        printf("=== test_syssvc_creprc_inherit: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    /*
     * THIS process becomes the NAMED CREATOR: SYSTEM, holding SYSPRV/SETPRV.
     * It must hold SETPRV to establish the identity at all, which a ROOT
     * registration's enforced mask carries -- the same precondition
     * test_syssvc_identcont relies on.
     */
    {
        uint32_t ist = vms_kif_setident(SYS_NAME, SYS_UIC, SYS_PRIVS);
        CHECK(ist == 1, "creator established the SYSTEM/SYSPRV identity to be inherited");
    }

    struct vms_procinfo creator;
    memset(&creator, 0, sizeof(creator));
    CHECK((vms_kif_getjpi_self(&creator) & 1) != 0,
          "creator read back its own executive row");
    uint32_t creator_vms_pid = creator.vms_pid;

    /* ---- A: a NON-ROOT subprocess inherits the creator's identity ---------- */
    {
        int relay[2];
        if (pipe(relay) < 0) {
            CHECK(0, "A: pipe()");
        } else {
            fflush(NULL);
            pid_t p = fork();
            if (p < 0) {
                CHECK(0, "A: fork()");
                close(relay[0]); close(relay[1]);
            } else if (p == 0) {
                close(relay[0]);
                /* Drop to a non-root credential -- the runtime SPAWN condition. */
                int dropped = (drop_to_unprivileged() == 0);
                /* THE PATH UNDER TEST: register as a SUBPROCESS. No setident. */
                uint32_t st = vms_kif_register_subprocess();
                struct vms_procinfo self;
                memset(&self, 0, sizeof(self));
                uint32_t gj = vms_kif_getjpi_self(&self);
                dprintf(relay[1],
                        "DROPPED=%d\nEUID=%u\nREG=%u\nGJ=%u\nUSER=%s\nUIC=%u\nPRIVS=%016llx\nVMSPID=%u\n",
                        dropped, (unsigned)geteuid(), (unsigned)st, (unsigned)gj,
                        self.username, (unsigned)self.uic,
                        (unsigned long long)self.cur_privs, (unsigned)self.vms_pid);
                close(relay[1]);
                _exit(0);
            } else {
                close(relay[1]);
                char out[4096]; size_t used = 0; ssize_t n;
                while ((n = read(relay[0], out + used, sizeof(out) - 1 - used)) > 0) {
                    used += (size_t)n;
                    if (used >= sizeof(out) - 1) break;
                }
                out[used] = '\0';
                close(relay[0]);
                waitpid(p, NULL, 0);

                printf("  ---- A: subprocess registration readback ----\n%s  ---- end A ----\n", out);

                unsigned euid = 1, reg = 0, uic = 0, vmspid = 0;
                char user[64] = {0};
                unsigned long long privs = 0;
                sscanf(strstr(out, "EUID=") ? strstr(out, "EUID=") : out, "EUID=%u", &euid);
                if (strstr(out, "REG=")) sscanf(strstr(out, "REG="), "REG=%u", &reg);
                if (strstr(out, "UIC=")) sscanf(strstr(out, "UIC="), "UIC=%u", &uic);
                if (strstr(out, "PRIVS=")) sscanf(strstr(out, "PRIVS="), "PRIVS=%016llx", &privs);
                if (strstr(out, "VMSPID=")) sscanf(strstr(out, "VMSPID="), "VMSPID=%u", &vmspid);
                { char *u = strstr(out, "USER=");
                  if (u) sscanf(u, "USER=%63[^\n]", user); }

                CHECK(euid != 0, "A: the subprocess is genuinely NON-ROOT (the runtime SPAWN condition)");
                CHECK(reg == 1, "A: VMS_IOCTL_REGISTER_SUBPROCESS was accepted");
                /* negctl: register-subprocess-identity-self-declared */
                CHECK(strcmp(user, SYS_NAME) == 0,
                      "A: the subprocess INHERITED the creator's user name (SYSTEM) -- a readback, not a self-declaration");
                CHECK(uic == SYS_UIC,
                      "A: the subprocess inherited the creator's UIC [1,4]");
                CHECK(privs == (unsigned long long)(uint64_t)SYS_PRIVS,
                      "A: the subprocess inherited the creator's privilege mask, SETPRV/SYSPRV included");
                CHECK(vmspid != 0 && vmspid != creator_vms_pid,
                      "A: the subprocess got a FRESH, DISTINCT VMS PID (a new VMS process, not the creator's PID)");
            }
        }
    }

    /* ---- B: a non-root child STILL cannot self-declare a privileged name --- */
    {
        int relay[2];
        if (pipe(relay) < 0) {
            CHECK(0, "B: pipe()");
        } else {
            fflush(NULL);
            pid_t p = fork();
            if (p < 0) {
                CHECK(0, "B: fork()");
                close(relay[0]); close(relay[1]);
            } else if (p == 0) {
                close(relay[0]);
                int dropped = (drop_to_unprivileged() == 0);
                /* Register FRESH (the pre-fix path), then attempt to STAMP the
                 * SYSTEM identity onto ourselves -- the self-declaration the
                 * guard must refuse. */
                uint32_t rst = vms_kif_register(NULL);
                uint32_t ist = vms_kif_setident(SYS_NAME, SYS_UIC, SYS_PRIVS);
                dprintf(relay[1], "DROPPED=%d\nEUID=%u\nREG=%u\nSETIDENT=%u\n",
                        dropped, (unsigned)geteuid(), (unsigned)rst, (unsigned)ist);
                close(relay[1]);
                _exit(0);
            } else {
                close(relay[1]);
                char out[1024]; size_t used = 0; ssize_t n;
                while ((n = read(relay[0], out + used, sizeof(out) - 1 - used)) > 0) {
                    used += (size_t)n;
                    if (used >= sizeof(out) - 1) break;
                }
                out[used] = '\0';
                close(relay[0]);
                waitpid(p, NULL, 0);

                printf("  ---- B: non-root self-declaration attempt ----\n%s  ---- end B ----\n", out);

                unsigned euid = 1, setident = 1;
                if (strstr(out, "EUID=")) sscanf(strstr(out, "EUID="), "EUID=%u", &euid);
                if (strstr(out, "SETIDENT=")) sscanf(strstr(out, "SETIDENT="), "SETIDENT=%u", &setident);

                CHECK(euid != 0, "B: the child is genuinely NON-ROOT");
                CHECK(setident == SS$_NOPRIV,
                      "B: vms_kif_setident REFUSED the non-root self-declared SYSTEM identity (SS$_NOPRIV) -- guard UNCHANGED");
            }
        }
    }

    /* ==== rd vms-ff75: DETACHED identity, authorized by the CREATOR ==========
     * C) creator (SYSTEM, SETPRV) gets a ticket for a FOREIGN identity --
     *    UIC [128,129], TMPMBX only; its detached GRANDCHILD (non-root, real
     *    parent an unregistered intermediate, exactly sys$creprc's shape)
     *    claims it and its $GETJPI row shows that identity + a fresh PID.
     * D) the same ticket presented again is REFUSED (single use).
     * E) an ORPHANED grandchild (intermediate already gone, so the creator is
     *    no longer its ancestor) is REFUSED its ticket.
     * F) a creator WITHOUT SETPRV is refused a foreign UIC (SS$_NOPRIV) but may
     *    ticket a subset of its own identity.  */
    {
        /* VMS UIC [128,129] -- the DCL display is octal, the packed word is
         * (group<<16)|member; this pins the numeric value the caller asked for. */
        const uint32_t FUIC_D = (128u << 16) | 129u;
        const uint64_t FPRIV = PRV$M_TMPMBX;
        uint64_t tk = 0, tk_orphan = 0;
        uint32_t tst = vms_kif_creprc_ticket(SYS_NAME, FUIC_D, FPRIV, &tk);
        CHECK(tst == 1 && tk != 0,
              "C: a SETPRV creator is issued a ticket for UIC [128,129] + TMPMBX");
        uint32_t tst2 = vms_kif_creprc_ticket(SYS_NAME, FUIC_D, FPRIV, &tk_orphan);
        CHECK(tst2 == 1 && tk_orphan != 0 && tk_orphan != tk,
              "C: a second, distinct ticket for the orphan check");

        /* run one grandchild claim; orphan=1 makes the intermediate exit first */
        for (int round = 0; round < 3; round++) {
            uint64_t use = (round == 2) ? tk_orphan : tk;  /* round 1 = replay */
            int orphan = (round == 2);
            int relay[2];
            if (pipe(relay) < 0) { CHECK(0, "C: pipe()"); break; }
            fflush(NULL);
            pid_t p = fork();
            if (p == 0) {                                    /* intermediate */
                close(relay[0]);
                (void)setsid();
                pid_t g = fork();
                if (g > 0) {
                    if (!orphan)
                        waitpid(g, NULL, 0);                 /* stay the link */
                    _exit(0);
                }
                if (g < 0) _exit(1);
                if (orphan) {                                /* wait to be orphaned */
                    for (int w = 0; w < 200 && getppid() != 1; w++)
                        usleep(10000);
                }
                (void)drop_to_unprivileged();
                uint32_t vp = 0;
                uint32_t rs = vms_kif_register_detached(use, &vp);
                struct vms_procinfo self;
                memset(&self, 0, sizeof(self));
                uint32_t gj = (rs & 1) ? vms_kif_getjpi_self(&self) : 0;
                dprintf(relay[1], "EUID=%u\nRS=%u\nGJ=%u\nUSER=%s\nUIC=%u\nPRIVS=%016llx\nVMSPID=%u\n",
                        (unsigned)geteuid(), (unsigned)rs, (unsigned)gj, self.username,
                        (unsigned)self.uic, (unsigned long long)self.cur_privs,
                        (unsigned)self.vms_pid);
                close(relay[1]);
                _exit(0);
            }
            close(relay[1]);
            char out[2048]; size_t used = 0; ssize_t n;
            while ((n = read(relay[0], out + used, sizeof(out) - 1 - used)) > 0) {
                used += (size_t)n;
                if (used >= sizeof(out) - 1) break;
            }
            out[used] = '\0';
            close(relay[0]);
            if (p > 0) waitpid(p, NULL, 0);
            printf("  ---- %s ----\n%s  ---- end ----\n",
                   round == 0 ? "C: detached claim" : round == 1 ? "D: replay" : "E: orphan", out);
            unsigned euid = 0, rs = 0, uic = 0, vmspid = 0;
            unsigned long long privs = 0;
            char user[64] = {0};
            if (strstr(out, "EUID=")) sscanf(strstr(out, "EUID="), "EUID=%u", &euid);
            if (strstr(out, "RS=")) sscanf(strstr(out, "RS="), "RS=%u", &rs);
            if (strstr(out, "UIC=")) sscanf(strstr(out, "UIC="), "UIC=%u", &uic);
            if (strstr(out, "PRIVS=")) sscanf(strstr(out, "PRIVS="), "PRIVS=%016llx", &privs);
            if (strstr(out, "VMSPID=")) sscanf(strstr(out, "VMSPID="), "VMSPID=%u", &vmspid);
            { char *u = strstr(out, "USER="); if (u) sscanf(u, "USER=%63[^\n]", user); }
            if (round == 0) {
                CHECK(euid != 0, "C: the detached grandchild is genuinely NON-ROOT");
                CHECK(rs == 1, "C: VMS_IOCTL_REGISTER_DETACHED honoured the creator's ticket");
                CHECK(uic == FUIC_D, "C: the detached process's $GETJPI row shows UIC [128,129]");
                CHECK(privs == (unsigned long long)FPRIV,
                      "C: the detached process's $GETJPI row shows exactly TMPMBX");
                CHECK(strcmp(user, SYS_NAME) == 0, "C: it carries the creator-authorized user name");
                CHECK(vmspid != 0 && vmspid != creator_vms_pid,
                      "C: it is a NEW VMS process (fresh, distinct PID)");
            } else if (round == 1) {
                CHECK(rs == SS$_NOPRIV, "D: the same ticket presented again is REFUSED (single use)");
            } else {
                CHECK(rs == SS$_NOPRIV,
                      "E: an orphaned claimant (creator no longer its ancestor) is REFUSED its ticket");
            }
        }

        /* F: a creator without SETPRV */
        int relay[2];
        if (pipe(relay) == 0) {
            fflush(NULL);
            pid_t p = fork();
            if (p == 0) {
                close(relay[0]);
                (void)drop_to_unprivileged();
                uint32_t r = vms_kif_register_subprocess();   /* SYSTEM, SETPRV */
                uint64_t prev = 0, t1 = 0, t2 = 0;
                uint32_t sp = vms_kif_setprv(PRV$M_SETPRV, 0, 0, &prev);
                uint32_t a = vms_kif_creprc_ticket(SYS_NAME, FUIC_D, FPRIV, &t1);
                uint32_t b = vms_kif_creprc_ticket(SYS_NAME, SYS_UIC, FPRIV, &t2);
                dprintf(relay[1], "REG=%u\nSP=%u\nFOREIGN=%u\nSUBSET=%u\n",
                        (unsigned)r, (unsigned)sp, (unsigned)a, (unsigned)b);
                close(relay[1]);
                _exit(0);
            }
            close(relay[1]);
            char out[512]; size_t used = 0; ssize_t n;
            while ((n = read(relay[0], out + used, sizeof(out) - 1 - used)) > 0) {
                used += (size_t)n;
                if (used >= sizeof(out) - 1) break;
            }
            out[used] = '\0';
            close(relay[0]);
            if (p > 0) waitpid(p, NULL, 0);
            printf("  ---- F: creator without SETPRV ----\n%s  ---- end F ----\n", out);
            unsigned sp = 0, fo = 1, su = 0;
            if (strstr(out, "SP=")) sscanf(strstr(out, "SP="), "SP=%u", &sp);
            if (strstr(out, "FOREIGN=")) sscanf(strstr(out, "FOREIGN="), "FOREIGN=%u", &fo);
            if (strstr(out, "SUBSET=")) sscanf(strstr(out, "SUBSET="), "SUBSET=%u", &su);
            CHECK(sp == 1, "F: the creator disabled SETPRV");
            CHECK(fo == SS$_NOPRIV,
                  "F: a creator WITHOUT SETPRV is REFUSED a ticket for a foreign UIC [128,129] (SS$_NOPRIV)");
            CHECK(su == 1,
                  "F: the same creator may ticket a SUBSET of its own identity (own UIC, TMPMBX)");
        } else {
            CHECK(0, "F: pipe()");
        }
    }

    printf("=== test_syssvc_creprc_inherit: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
