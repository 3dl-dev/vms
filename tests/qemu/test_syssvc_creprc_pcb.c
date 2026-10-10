/*
 * test_syssvc_creprc_pcb.c - the creator creates its child's PCB before the
 * child runs its image (VMS_IOCTL_CREPRC_PCB, rd vms-c43 / vms-9f32, design
 * note docs/design-executive-process-lifecycle.md 3.1).
 *
 * The parent forks a child and HOLDS it (blocked on a pipe, before exec and
 * before any executive call). The parent issues vms_kif_creprc_pcb() for it,
 * then reads the child's row with $GETJPI BY THE VMS PID THE EXECUTIVE GAVE --
 * the row exists before the child has run anything, carries the parent's
 * identity, and is the row the child then adopts when it does reach the
 * executive. The executive refuses to make a PCB for a task that is not the
 * caller's child, refuses a second PCB for the same task, and refuses a
 * detached identity its caller may not give.
 *
 * negctl: creprc-pcb-not-a-child, creprc-pcb-identity-unchecked
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdint.h>
#include "ssdef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

/* Fork a child that blocks until released, then reports its own executive
 * VMS PID (its first executive call) on `rep`, then exits. */
static pid_t held_child(int *go_w, int *rep_r)
{
    int go[2], rp[2];
    if (pipe(go) < 0 || pipe(rp) < 0)
        return -1;
    pid_t c = fork();
    if (c == 0) {
        char b;
        close(go[1]); close(rp[0]);
        if (read(go[0], &b, 1) != 1)
            _exit(0);
        struct vms_procinfo me;
        uint32_t v = (vms_kif_getjpi_self(&me) & 1) ? me.vms_pid : 0;
        (void)!write(rp[1], &v, sizeof(v));
        _exit(0);
    }
    close(go[0]); close(rp[1]);
    *go_w = go[1];
    *rep_r = rp[0];
    return c;
}

static void release_and_reap(pid_t c, int go_w, int rep_r, uint32_t *child_view)
{
    uint32_t v = 0;
    (void)!write(go_w, "g", 1);
    if (read(rep_r, &v, sizeof(v)) != (ssize_t)sizeof(v))
        v = 0;
    if (child_view)
        *child_view = v;
    close(go_w); close(rep_r);
    while (waitpid(c, NULL, 0) < 0 && errno == EINTR)
        ;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_creprc_pcb: the creator creates its child's PCB before the child runs (rd vms-c43) ===\n");
    if (!executive_present()) {
        printf("=== test_syssvc_creprc_pcb: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    struct vms_procinfo me;
    CHECK(vms_kif_getjpi_self(&me) & 1, "the creator has a row");

    /* 1. A held child gets its PCB from the creator, before it runs. */
    {
        int go_w, rep_r;
        pid_t c = held_child(&go_w, &rep_r);
        uint32_t vpid = 0, child_view = 0;
        uint32_t st = vms_kif_creprc_pcb((uint32_t)c, VMS_CREPRC_PCB_SUBPROCESS,
                                         NULL, 0, 0, &vpid);
        CHECK(st == SS$_NORMAL && vpid != 0,
              "CREPRC_PCB creates the held child's PCB and returns its VMS PID");
        CHECK(vpid != me.vms_pid, "the child is a new VMS process (its own VMS PID)");
        struct vms_procinfo ci;
        memset(&ci, 0, sizeof(ci));
        uint32_t gst = vms_kif_getjpi_pid(vpid, &ci);
        CHECK(gst == SS$_NORMAL && ci.linux_pid == (uint32_t)c,
              "the creator reads the child's row by that VMS PID BEFORE the child has run anything");
        CHECK(gst == SS$_NORMAL && ci.uic == me.uic && ci.cur_privs == me.cur_privs &&
              strncmp(ci.username, me.username, sizeof(ci.username)) == 0,
              "the row carries the creator's identity (UIC, user name, privileges)");
        CHECK(vms_kif_creprc_pcb((uint32_t)c, VMS_CREPRC_PCB_SUBPROCESS, NULL, 0, 0, NULL) ==
                  SS$_BADPARAM,
              "a second CREPRC_PCB for the same task is refused -- it already is a VMS process");
        release_and_reap(c, go_w, rep_r, &child_view);
        CHECK(child_view == vpid,
              "the child, reaching the executive, is the row its creator made (same VMS PID)");
    }

    /* 2. A task that is not the caller's child gets no PCB from it. */
    {
        int go_w, rep_r;
        pid_t c = held_child(&go_w, &rep_r);
        int p2[2];
        uint32_t st = 0;
        if (pipe(p2) == 0) {
            pid_t sib = fork();
            if (sib == 0) {
                /* A sibling of `c`, not its parent. */
                close(p2[0]);
                uint32_t s2 = vms_kif_creprc_pcb((uint32_t)c, VMS_CREPRC_PCB_SUBPROCESS,
                                                 NULL, 0, 0, NULL);
                (void)!write(p2[1], &s2, sizeof(s2));
                _exit(0);
            }
            close(p2[1]);
            if (read(p2[0], &st, sizeof(st)) != (ssize_t)sizeof(st))
                st = 0;
            close(p2[0]);
            while (waitpid(sib, NULL, 0) < 0 && errno == EINTR)
                ;
        }
        /* negctl: creprc-pcb-not-a-child */
        CHECK(st == SS$_NOPRIV,
              "CREPRC_PCB for a task that is not the caller's child is refused (SS$_NOPRIV)");
        release_and_reap(c, go_w, rep_r, NULL);
    }

    /* 3. A detached identity the creator may not give is refused. A helper
     *    without SETPRV (stamped down) asks for a privilege beyond its own. */
    {
        int p3[2];
        uint32_t res[2] = { 0, 0 };
        if (pipe(p3) == 0) {
            pid_t h = fork();
            if (h == 0) {
                close(p3[0]);
                struct vms_procinfo hi;
                uint32_t out[2] = { 0, 0 };
                if (vms_kif_getjpi_self(&hi) & 1) {
                    uint64_t low = VMS_PRV_M_TMPMBX;
                    out[0] = vms_kif_setident("OVMXC43HELP", hi.uic, low);
                    /* The detached shape: helper -> intermediate -> held grandchild,
                     * the intermediate alive while the helper asks. */
                    int gp[2], hold[2];
                    if (pipe(gp) == 0 && pipe(hold) == 0) {
                        pid_t inter = fork();
                        if (inter == 0) {
                            char b;
                            close(gp[0]); close(hold[1]);
                            pid_t g = fork();
                            if (g == 0) {
                                close(gp[1]);
                                (void)!read(hold[0], &b, 1);   /* held, then exits */
                                _exit(0);
                            }
                            uint32_t gpid = (uint32_t)g;
                            (void)!write(gp[1], &gpid, sizeof(gpid));
                            (void)!read(hold[0], &b, 1);       /* alive until released */
                            while (waitpid(g, NULL, 0) < 0 && errno == EINTR)
                                ;
                            _exit(0);
                        }
                        close(gp[1]); close(hold[0]);
                        uint32_t gpid = 0;
                        if (read(gp[0], &gpid, sizeof(gpid)) == (ssize_t)sizeof(gpid))
                            out[1] = vms_kif_creprc_pcb(gpid, VMS_CREPRC_PCB_DETACHED,
                                                        "OVMXC43HELP", hi.uic,
                                                        low | VMS_PRV_M_SETPRV, NULL);
                        close(gp[0]);
                        close(hold[1]);                        /* releases both */
                        while (waitpid(inter, NULL, 0) < 0 && errno == EINTR)
                            ;
                    }
                }
                (void)!write(p3[1], out, sizeof(out));
                _exit(0);
            }
            close(p3[1]);
            if (read(p3[0], res, sizeof(res)) != (ssize_t)sizeof(res))
                res[0] = res[1] = 0;
            close(p3[0]);
            while (waitpid(h, NULL, 0) < 0 && errno == EINTR)
                ;
        }
        CHECK(res[0] & 1, "the helper stamped itself down to TMPMBX (no SETPRV)");
        /* negctl: creprc-pcb-identity-unchecked */
        CHECK(res[1] == SS$_NOPRIV,
              "a detached identity with a privilege its creator is not authorized for is refused (SS$_NOPRIV)");
    }

    printf("=== test_syssvc_creprc_pcb: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
