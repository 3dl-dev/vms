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
 * The fix, as it stands since rd vms-c43 / vms-2a5: the CREATOR has the
 * executive make the subprocess's PCB (VMS_IOCTL_CREPRC_PCB) before the child
 * runs. The executive copies the creator's identity onto it and mints a FRESH,
 * distinct VMS PID (a subprocess is a new VMS process). The child declares
 * nothing and registers nothing.
 *
 *   A) A NAMED creator (SYSTEM, SYSPRV/SETPRV) creates a child that DROPS to a
 *      non-root Linux credential. The child holds the creator's user name, UIC
 *      and privileges -- inherited, never asked for -- AND a VMS PID DISTINCT
 *      from the creator's. This is the fix.
 *
 *   B) THE SECURITY HALF. An UNPRIVILEGED process (created detached in another
 *      UIC with only TMPMBX/NETMBX) that tries to self-declare the SYSTEM
 *      identity through vms_kif_setident() is REFUSED (SS$_NOPRIV).
 *
 *   C) A DETACHED identity is what the creator may give: a SETPRV creator
 *      gives UIC [128,129] + TMPMBX and the new process's row shows it.
 *
 *   F) A creator WITHOUT SETPRV is refused a foreign UIC (SS$_NOPRIV) and may
 *      give a subset of its own identity.
 *
 * A implies a non-root child gained its creator's identity without declaring
 * it; B implies it still cannot declare one for itself. Both must hold at once.
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
#include "ovmx_test_fork.h"

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
     * It is SYSTEM already (the harness boot task made it), and holds CMKRNL,
     * so it may stamp the exact identity the checks below compare against.
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
            pid_t p = ovmx_fork_subprocess();   /* THE PATH UNDER TEST: the creator makes the PCB */
            if (p < 0) {
                CHECK(0, "A: fork()");
                close(relay[0]); close(relay[1]);
            } else if (p == 0) {
                close(relay[0]);
                /* Drop to a non-root credential -- the runtime SPAWN condition. */
                int dropped = (drop_to_unprivileged() == 0);
                struct vms_procinfo self;
                memset(&self, 0, sizeof(self));
                uint32_t gj = vms_kif_getjpi_self(&self);
                dprintf(relay[1],
                        "DROPPED=%d\nEUID=%u\nGJ=%u\nUSER=%s\nUIC=%u\nPRIVS=%016llx\nVMSPID=%u\n",
                        dropped, (unsigned)geteuid(), (unsigned)gj,
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

                printf("  ---- A: subprocess readback ----\n%s  ---- end A ----\n", out);

                unsigned euid = 1, gj = 0, uic = 0, vmspid = 0;
                char user[64] = {0};
                unsigned long long privs = 0;
                sscanf(strstr(out, "EUID=") ? strstr(out, "EUID=") : out, "EUID=%u", &euid);
                if (strstr(out, "GJ=")) sscanf(strstr(out, "GJ="), "GJ=%u", &gj);
                if (strstr(out, "UIC=")) sscanf(strstr(out, "UIC="), "UIC=%u", &uic);
                if (strstr(out, "PRIVS=")) sscanf(strstr(out, "PRIVS="), "PRIVS=%016llx", &privs);
                if (strstr(out, "VMSPID=")) sscanf(strstr(out, "VMSPID="), "VMSPID=%u", &vmspid);
                { char *u = strstr(out, "USER=");
                  if (u) sscanf(u, "USER=%63[^\n]", user); }

                CHECK(euid != 0, "A: the subprocess is genuinely NON-ROOT (the runtime SPAWN condition)");
                CHECK(gj == 1, "A: the subprocess has the row its creator made -- it registered nothing");
                /* negctl: register-subprocess-identity-self-declared */
                CHECK(strcmp(user, SYS_NAME) == 0,
                      "A: the subprocess INHERITED the creator's user name (SYSTEM) -- a readback, not a self-declaration");
                CHECK(uic == SYS_UIC,
                      "A: the subprocess inherited the creator's UIC [1,4]");
                /* negctl: register-continue-identity-dropped */
                CHECK(privs == (unsigned long long)(uint64_t)SYS_PRIVS,
                      "A: the subprocess inherited the creator's privilege mask, SETPRV/SYSPRV included");
                CHECK(vmspid != 0 && vmspid != creator_vms_pid,
                      "A: the subprocess got a FRESH, DISTINCT VMS PID (a new VMS process, not the creator's PID)");
            }
        }
    }

    /* ---- B: an unprivileged process STILL cannot self-declare a privileged name */
    {
        int relay[2];
        if (pipe(relay) < 0) {
            CHECK(0, "B: pipe()");
        } else {
            fflush(NULL);
            pid_t p = ovmx_fork_unprivileged(DROP_GID, DROP_UID);
            if (p < 0) {
                CHECK(0, "B: fork()");
                close(relay[0]); close(relay[1]);
            } else if (p == 0) {
                close(relay[0]);
                int dropped = (drop_to_unprivileged() == 0);
                /* Attempt to STAMP the SYSTEM identity onto ourselves -- the
                 * self-declaration the guard must refuse. */
                uint32_t ist = vms_kif_setident(SYS_NAME, SYS_UIC, SYS_PRIVS);
                dprintf(relay[1], "DROPPED=%d\nEUID=%u\nSETIDENT=%u\n",
                        dropped, (unsigned)geteuid(), (unsigned)ist);
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

                printf("  ---- B: unprivileged self-declaration attempt ----\n%s  ---- end B ----\n", out);

                unsigned euid = 1, setident = 1;
                if (strstr(out, "EUID=")) sscanf(strstr(out, "EUID="), "EUID=%u", &euid);
                if (strstr(out, "SETIDENT=")) sscanf(strstr(out, "SETIDENT="), "SETIDENT=%u", &setident);

                CHECK(euid != 0, "B: the child is genuinely NON-ROOT");
                CHECK(setident == SS$_NOPRIV,
                      "B: vms_kif_setident REFUSED the unprivileged self-declared SYSTEM identity (SS$_NOPRIV)");
            }
        }
    }

    /* ---- C / F: a DETACHED identity is what the creator may give ---------- */
    {
        /* VMS UIC [128,129] -- the DCL display is octal, the packed word is
         * (group<<16)|member; this pins the numeric value the caller asked for. */
        const uint32_t FUIC_D = (128u << 16) | 129u;
        const uint64_t FPRIV = PRV$M_TMPMBX;
        int relay[2];

        if (pipe(relay) < 0) {
            CHECK(0, "C: pipe()");
        } else {
            uint32_t cst = 0;
            fflush(NULL);
            pid_t p = ovmx_fork_detached_st(SYS_NAME, FUIC_D, FPRIV, &cst);
            if (p == 0) {
                close(relay[0]);
                (void)drop_to_unprivileged();
                struct vms_procinfo self;
                memset(&self, 0, sizeof(self));
                uint32_t gj = vms_kif_getjpi_self(&self);
                dprintf(relay[1], "EUID=%u\nGJ=%u\nUSER=%s\nUIC=%u\nPRIVS=%016llx\nVMSPID=%u\n",
                        (unsigned)geteuid(), (unsigned)gj, self.username,
                        (unsigned)self.uic, (unsigned long long)self.cur_privs,
                        (unsigned)self.vms_pid);
                close(relay[1]);
                _exit(0);
            }
            close(relay[1]);
            char out[2048]; size_t used = 0; ssize_t n;
            while (p > 0 && (n = read(relay[0], out + used, sizeof(out) - 1 - used)) > 0) {
                used += (size_t)n;
                if (used >= sizeof(out) - 1) break;
            }
            out[used] = '\0';
            close(relay[0]);
            if (p > 0) waitpid(p, NULL, 0);
            printf("  ---- C: detached process ----\n%s  ---- end C ----\n", out);
            unsigned euid = 0, gj = 0, uic = 0, vmspid = 0;
            unsigned long long privs = 0;
            char user[64] = {0};
            if (strstr(out, "EUID=")) sscanf(strstr(out, "EUID="), "EUID=%u", &euid);
            if (strstr(out, "GJ=")) sscanf(strstr(out, "GJ="), "GJ=%u", &gj);
            if (strstr(out, "UIC=")) sscanf(strstr(out, "UIC="), "UIC=%u", &uic);
            if (strstr(out, "PRIVS=")) sscanf(strstr(out, "PRIVS="), "PRIVS=%016llx", &privs);
            if (strstr(out, "VMSPID=")) sscanf(strstr(out, "VMSPID="), "VMSPID=%u", &vmspid);
            { char *u = strstr(out, "USER="); if (u) sscanf(u, "USER=%63[^\n]", user); }
            CHECK(cst == 1 && p > 0, "C: a SETPRV creator may give UIC [128,129] + TMPMBX");
            CHECK(euid != 0, "C: the detached process is genuinely NON-ROOT");
            CHECK(gj == 1, "C: the detached process has the row its creator made");
            CHECK(uic == FUIC_D, "C: the detached process's $GETJPI row shows UIC [128,129]");
            CHECK(privs == (unsigned long long)FPRIV,
                  "C: the detached process's $GETJPI row shows exactly TMPMBX");
            CHECK(strcmp(user, SYS_NAME) == 0, "C: it carries the creator-authorized user name");
            CHECK(vmspid != 0 && vmspid != creator_vms_pid,
                  "C: it is a NEW VMS process (fresh, distinct PID)");
        }

        /* F: a creator without SETPRV */
        if (pipe(relay) == 0) {
            fflush(NULL);
            pid_t p = ovmx_fork_subprocess();          /* SYSTEM, SETPRV */
            if (p == 0) {
                close(relay[0]);
                (void)drop_to_unprivileged();
                uint64_t prev = 0;
                uint32_t a = 0, b = 0;
                uint32_t sp = vms_kif_setprv(PRV$M_SETPRV, 0, 0, &prev);
                pid_t g1 = ovmx_fork_detached_st(SYS_NAME, FUIC_D, FPRIV, &a);
                if (g1 == 0) _exit(0);
                if (g1 > 0) waitpid(g1, NULL, 0);
                pid_t g2 = ovmx_fork_detached_st(SYS_NAME, SYS_UIC, FPRIV, &b);
                if (g2 == 0) _exit(0);
                if (g2 > 0) waitpid(g2, NULL, 0);
                dprintf(relay[1], "SP=%u\nFOREIGN=%u\nSUBSET=%u\n",
                        (unsigned)sp, (unsigned)a, (unsigned)b);
                close(relay[1]);
                _exit(0);
            }
            close(relay[1]);
            char out[512]; size_t used = 0; ssize_t n;
            while (p > 0 && (n = read(relay[0], out + used, sizeof(out) - 1 - used)) > 0) {
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
                  "F: a creator WITHOUT SETPRV is REFUSED a foreign UIC [128,129] (SS$_NOPRIV)");
            CHECK(su == 1,
                  "F: the same creator may give a SUBSET of its own identity (own UIC, TMPMBX)");
        } else {
            CHECK(0, "F: pipe()");
        }
    }

    printf("=== test_syssvc_creprc_inherit: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
