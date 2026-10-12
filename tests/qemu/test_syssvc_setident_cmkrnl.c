/*
 * test_syssvc_setident_cmkrnl.c - stamping a process identity needs CMKRNL.
 *
 * VMS_IOCTL_SETIDENT (vms_kif_setident) sets a process's user name, UIC and
 * privilege masks. On VMS that is LOGINOUT writing the PCB and JIB in kernel
 * mode -- LOGINOUT is installed with CMKRNL -- and no system service lets a
 * program change its own identity. So the executive requires CMKRNL among the
 * caller's ENABLED privileges (src/kernel-core/vms_proctab.c,
 * vms_ioctl_setident; vms_prot_require_priv).
 *
 * A child disables CMKRNL in its current privileges with $SETPRV -- keeping
 * SETPRV, which on its own let a caller stamp anything before -- and asks to
 * re-stamp its OWN identity: refused SS$_NOPRIV, and the row is unchanged.
 * It re-enables CMKRNL ($SETPRV, authorized) and the same call succeeds.
 *
 * negctl: setident-cmkrnl-unchecked
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <stdint.h>
#include "ssdef.h"
#include "starlet.h"
#include "vms_kif.h"

#define EXIT_SKIP 77

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

struct result {
    uint32_t got_self, had_cmkrnl, had_setprv;
    uint32_t drop_st, refused_st, unchanged;
    uint32_t restore_st, allowed_st, stamped;
};

static void child(int fd)
{
    struct result r;
    struct vms_procinfo me, after;
    memset(&r, 0, sizeof r);
    r.got_self = vms_kif_getjpi_self(&me);
    if (r.got_self & 1) {
        uint64_t cmk = VMS_PRV_M_CMKRNL;
        r.had_cmkrnl = (me.cur_privs & VMS_PRV_M_CMKRNL) != 0;
        r.had_setprv = (me.cur_privs & VMS_PRV_M_SETPRV) != 0;
        r.drop_st = sys$setprv(0, &cmk, 0, NULL);          /* current only */
        r.refused_st = vms_kif_setident("OVMXSIDN", me.uic, me.perm_privs);
        r.unchanged = (vms_kif_getjpi_self(&after) & 1) &&
                      strncmp(after.username, "OVMXSIDN", sizeof after.username) != 0;
        r.restore_st = sys$setprv(1, &cmk, 0, NULL);
        r.allowed_st = vms_kif_setident("OVMXSIDN", me.uic, me.perm_privs);
        r.stamped = (vms_kif_getjpi_self(&after) & 1) &&
                    strncmp(after.username, "OVMXSIDN", sizeof after.username) == 0;
    }
    (void)!write(fd, &r, sizeof r);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_setident_cmkrnl: stamping a process identity needs CMKRNL ===\n");
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_setident_cmkrnl: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }
    int p[2];
    if (pipe(p) != 0) { printf("  FAIL: pipe\n"); return 1; }
    pid_t c = fork();
    if (c == 0) {
        close(p[0]);
        child(p[1]);
        _exit(0);
    }
    close(p[1]);
    struct result r;
    memset(&r, 0, sizeof r);
    ssize_t n = read(p[0], &r, sizeof r);
    close(p[0]);
    while (waitpid(c, NULL, 0) < 0 && errno == EINTR)
        ;
    CHECK(n == (ssize_t)sizeof r && (r.got_self & 1), "the child is a VMS process");
    CHECK(r.had_cmkrnl && r.had_setprv, "it holds CMKRNL and SETPRV to start with");
    CHECK(r.drop_st & 1, "$SETPRV disables CMKRNL in its current privileges");
    /* negctl: setident-cmkrnl-unchecked */
    CHECK(r.refused_st == SS$_NOPRIV,
          "without CMKRNL enabled, stamping even its own identity is refused SS$_NOPRIV (SETPRV is not enough)");
    CHECK(r.unchanged, "the refused stamp left the row unchanged");
    CHECK(r.restore_st & 1, "$SETPRV re-enables CMKRNL (it is authorized)");
    CHECK(r.allowed_st & 1, "with CMKRNL enabled the same stamp is accepted");
    CHECK(r.stamped, "and the row carries the stamped user name");
    printf("=== test_syssvc_setident_cmkrnl: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
