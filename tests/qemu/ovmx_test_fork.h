/*
 * ovmx_test_fork.h - a suite's service-calling child, made by the suite as a
 * SUBPROCESS the executive created (rd vms-3fa, vms-9f32 design note 3.1/3.4).
 *
 * A plain fork() child is not a VMS process: on a booted OVMX system only
 * $CREPRC (VMS_IOCTL_CREPRC_PCB) makes one, and the child that used to
 * register itself on its first service call goes away with vms-2a5. A suite
 * whose child calls system services makes it here instead: fork, hold the
 * child, VMS_IOCTL_CREPRC_PCB for it (identity from the suite's own row, a
 * fresh VMS PID, the suite its creator of record), release. The child returns
 * 0 from ovmx_fork_subprocess() already a VMS process; the parent gets its pid.
 *
 * If the executive refuses the PCB the child exits held (it never runs as a
 * process the executive did not create) and the parent gets -1 with *status
 * set (when non-NULL).
 *
 * Header-only so the gcc-built test_kmod_* and the cmake-built test_syssvc_*
 * share one copy.
 */
#ifndef OVMX_TEST_FORK_H
#define OVMX_TEST_FORK_H

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "vms_kif.h"

static inline pid_t ovmx_fork_kind_st(uint32_t kind, uint32_t *status)
{
    int hold[2];
    /* No executive (the CI executive-absent control): there is nothing to
     * make a PCB with, so the child is a plain fork whose own service calls
     * then fail honestly -- as ovmx_subprocess.h releases its child. */
    if (vms_kif_open() < 0) {
        if (status) *status = 0x908; /* SS$_NOSUCHDEV */
        return fork();
    }
    if (pipe(hold) != 0) {
        if (status) *status = 0x124; /* SS$_INSFMEM */
        return -1;
    }
    pid_t c = fork();
    if (c == 0) {
        char b = 0;
        close(hold[1]);
        while (read(hold[0], &b, 1) < 0 && errno == EINTR)
            ;
        close(hold[0]);
        if (b != 'g')
            _exit(125);
        return 0;
    }
    close(hold[0]);
    if (c < 0) {
        close(hold[1]);
        if (status) *status = 0x124;
        return -1;
    }
    uint32_t st = vms_kif_creprc_pcb((uint32_t)c, kind, 0, 0, 0, 0);
    ssize_t w;
    do { w = write(hold[1], (st & 1) ? "g" : "x", 1); } while (w < 0 && errno == EINTR);
    close(hold[1]);
    if (status) *status = st;
    if (!(st & 1)) {
        while (waitpid(c, 0, 0) < 0 && errno == EINTR)
            ;
        return -1;
    }
    return c;
}

static inline pid_t ovmx_fork_subprocess_st(uint32_t *status)
{
    return ovmx_fork_kind_st(VMS_CREPRC_PCB_SUBPROCESS, status);
}

static inline pid_t ovmx_fork_subprocess(void)
{
    return ovmx_fork_subprocess_st(0);
}

/*
 * ovmx_fork_continue - DCL's image-task shape (VMS_CREPRC_PCB_CONTINUE, rd
 * vms-e50): the child is the SAME VMS process as the suite -- its VMS PID,
 * job and identity -- with a row of its own the executive made before it ran.
 */
static inline pid_t ovmx_fork_continue(void)
{
    return ovmx_fork_kind_st(VMS_CREPRC_PCB_CONTINUE, 0);
}

/*
 * ovmx_fork_detached_st - a DETACHED process the suite creates (its own job),
 * with the identity given (user name, UIC, privileges; the executive checks the
 * suite may give it). The detached shape of $CREPRC: an intermediate forks the
 * process and stays alive until its PCB exists, so the process is the suite's
 * grandchild through a live intermediate when VMS_IOCTL_CREPRC_PCB is issued.
 * The caller is a child subreaper only while the intermediate exits, so the
 * detached process is reparented to the caller, which can waitpid() it, and
 * nothing else's orphans are.
 * Returns 0 in the new process, its substrate pid in the caller, -1 on refusal.
 */
static inline pid_t ovmx_fork_detached_st(const char *uname, uint32_t uic, uint64_t privs,
                                          uint32_t *status)
{
    int gp[2], hold[2];
    int was_reaper = 0;
    if (vms_kif_open() < 0) {        /* no executive: see ovmx_fork_kind_st */
        if (status) *status = 0x908; /* SS$_NOSUCHDEV */
        return fork();
    }
    (void)prctl(PR_GET_CHILD_SUBREAPER, &was_reaper, 0, 0, 0);
    (void)prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
    if (pipe(gp) != 0 || pipe(hold) != 0) {
        if (status) *status = 0x124;
        return -1;
    }
    pid_t inter = fork();
    if (inter == 0) {
        char b = 0;
        close(gp[0]); close(hold[1]);
        pid_t g = fork();
        if (g == 0) {
            close(gp[1]);
            while (read(hold[0], &b, 1) < 0 && errno == EINTR)
                ;
            close(hold[0]);
            if (b != 'g')
                _exit(125);
            return 0;                       /* the detached process */
        }
        int32_t gpid = (int32_t)g;
        ssize_t w;
        do { w = write(gp[1], &gpid, sizeof gpid); } while (w < 0 && errno == EINTR);
        close(gp[1]);
        while (read(hold[0], &b, 1) < 0 && errno == EINTR)   /* alive until released */
            ;
        _exit(0);
    }
    close(gp[1]); close(hold[0]);
    int32_t gpid = -1;
    ssize_t r;
    do { r = read(gp[0], &gpid, sizeof gpid); } while (r < 0 && errno == EINTR);
    close(gp[0]);
    uint32_t st = 0x124;
    if (r == (ssize_t)sizeof gpid && gpid > 0)
        st = vms_kif_creprc_pcb((uint32_t)gpid, VMS_CREPRC_PCB_DETACHED, uname, uic, privs, 0);
    ssize_t w;
    do { w = write(hold[1], (st & 1) ? "gg" : "xx", 2); } while (w < 0 && errno == EINTR);
    close(hold[1]);
    while (inter > 0 && waitpid(inter, 0, 0) < 0 && errno == EINTR)
        ;
    /* The detached process is this caller's child now; orphans of anything
     * else go on to init, as they would without this helper. */
    if (!was_reaper)
        (void)prctl(PR_SET_CHILD_SUBREAPER, 0, 0, 0, 0);
    if (status) *status = st;
    return (st & 1) ? (pid_t)gpid : -1;
}

/*
 * ovmx_fork_unprivileged - an UNPRIVILEGED VMS process the suite creates
 * detached in UIC [gid,uid] with only the default privileges (TMPMBX, NETMBX),
 * under the suite's own user name -- what $CREPRC with a UIC argument gives on
 * VMS. It replaces a forked child that setgid()/setuid()'d and then registered
 * itself with a UIC derived from those credentials (rd vms-2a5): the executive
 * makes its row, from what its creator may give. The child still drops its own
 * substrate credentials, so neither identity lends it privilege.
 */
static inline pid_t ovmx_fork_unprivileged(uint32_t gid, uint32_t uid)
{
    struct vms_procinfo me;
    memset(&me, 0, sizeof(me));
    (void)vms_kif_getjpi_self(&me);
    return ovmx_fork_detached_st(me.username, ((gid & 0xFFFFu) << 16) | (uid & 0xFFFFu),
                                 VMS_PRV_M_TMPMBX | VMS_PRV_M_NETMBX, 0);
}

/*
 * ovmx_test_self - this process's own VMS PID, from the row its creator made
 * (the harness boot task, or the suite that forked it through
 * ovmx_fork_subprocess). Replaces the explicit VMS_IOCTL_REGISTER a suite used
 * to issue for itself (rd vms-2a5): a process does not enter the executive's
 * table on its own; it is already there, or it is not a VMS process.
 */
static inline uint32_t ovmx_test_self(uint32_t *vms_pid)
{
    struct vms_procinfo me;
    uint32_t st = vms_kif_getjpi_self(&me);
    if (vms_pid)
        *vms_pid = (st & 1) ? me.vms_pid : 0;
    return st;
}

#endif /* OVMX_TEST_FORK_H */
