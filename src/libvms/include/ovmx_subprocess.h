/*
 * ovmx_subprocess.h - create a VMS SUBPROCESS from a forking program, the way
 * $CREPRC does: the executive creates the child's PCB before the child runs
 * (rd vms-9f32 / vms-1f8, VMS_IOCTL_CREPRC_PCB).
 *
 * On VMS a program that runs another image in a new process gets that process
 * from $CREPRC (or LIB$SPAWN), and the process exists in the executive from
 * the moment it is created. OVMX's substrate creates the task with fork(); the
 * creator then asks the executive to make that task a VMS process -- a
 * subprocess of the creator, with the creator's identity, owned by the
 * creator -- while the child waits, before the child runs a single
 * instruction of its own. That is the same mechanism $CREPRC uses
 * (src/libvms/syssvc/sys_process.c) and DCL uses for its image tasks and PIPE
 * segments (src/vmsdcl/dcl_cmd_process.c, dcl_pcb_hold_*), made available to
 * every in-tree forker so none of them has its child register itself.
 *
 * ovmx_subprocess_fork() returns as fork() does: 0 in the child, the child's
 * task id in the creator, -1 if no task could be created. The child is
 * released whether or not its PCB was made, exactly as an activated image is:
 * with no executive there is none to make, and the child's own service calls
 * then fail honestly (SS$_NOSUCHDEV / SS$_NONEXPR). The executive's answer is
 * returned through *pcb_status and the child's VMS PID through *vms_pid (either
 * may be NULL).
 *
 * Header-only on purpose: it adds no symbol to any shareable's vector.
 */
#ifndef OVMX_SUBPROCESS_H
#define OVMX_SUBPROCESS_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>

#include "ssdef.h"
#include "vms_kif.h"

/* The same, for a process of the given kind (VMS_CREPRC_PCB_SUBPROCESS or
 * VMS_CREPRC_PCB_DETACHED) -- rd vms-798: PID 1 creates OPCOM DETACHED, as
 * VMS's STARTUP does. */
static inline pid_t ovmx_creprc_fork(uint32_t kind, uint32_t *pcb_status, uint32_t *vms_pid);

static inline pid_t ovmx_subprocess_fork(uint32_t *pcb_status, uint32_t *vms_pid)
{
    return ovmx_creprc_fork(VMS_CREPRC_PCB_SUBPROCESS, pcb_status, vms_pid);
}

static inline pid_t ovmx_creprc_fork(uint32_t kind, uint32_t *pcb_status, uint32_t *vms_pid)
{
    int go[2];
    pid_t pid;
    uint32_t st = SS$_ABORT;
    ssize_t n;
    char b = 'g';

    if (pcb_status)
        *pcb_status = SS$_ABORT;
    if (vms_pid)
        *vms_pid = 0;
    if (pipe(go) != 0)
        return -1;
    (void)fcntl(go[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(go[1], F_SETFD, FD_CLOEXEC);

    pid = fork();
    if (pid == 0) {
        /* Child: wait until the creator has made this task its subprocess
         * (or given up: end-of-file). Nothing here touches the executive. */
        close(go[1]);
        do { n = read(go[0], &b, 1); } while (n < 0 && errno == EINTR);
        close(go[0]);
        return 0;
    }
    close(go[0]);
    if (pid > 0)
        st = vms_kif_creprc_pcb((uint32_t)pid, kind,
                                (const char *)0, 0, 0, vms_pid);
    do { n = write(go[1], &b, 1); } while (n < 0 && errno == EINTR);
    close(go[1]);
    if (pcb_status)
        *pcb_status = st;
    return pid;
}

#endif /* OVMX_SUBPROCESS_H */
