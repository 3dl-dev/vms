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
 * segments (src/vmsdcl/dcl_cmd_process.c, dcl_image_fork), made available to
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
 * VMS's STARTUP does. A DETACHED process is created with the identity the
 * creator names (username, uic, privs), which the executive checks against the
 * creator's own; a subprocess takes its creator's (NULL, 0, 0). */
static inline pid_t ovmx_creprc_fork_as(uint32_t kind, const char *username,
                                        uint32_t uic, uint64_t privs,
                                        uint32_t *pcb_status, uint32_t *vms_pid);

/* The same, for a process (of the creator's identity) whose deletion the
 * executive reports into the creator's mailbox MBA<mbxunt>: -- $CREPRC's
 * termination mailbox (rd vms-6d9, vms_kif_creprc_pcb_mbx). mbxunt 0 = none. */
static inline pid_t ovmx_creprc_fork_mbx(uint32_t kind, uint32_t mbxunt,
                                         uint32_t *pcb_status, uint32_t *vms_pid);

static inline pid_t ovmx_creprc_fork(uint32_t kind, uint32_t *pcb_status, uint32_t *vms_pid)
{
    return ovmx_creprc_fork_as(kind, (const char *)0, 0, 0, pcb_status, vms_pid);
}

static inline pid_t ovmx_subprocess_fork(uint32_t *pcb_status, uint32_t *vms_pid)
{
    return ovmx_creprc_fork(VMS_CREPRC_PCB_SUBPROCESS, pcb_status, vms_pid);
}

/*
 * The creator and the task synchronise through the executive's creation record
 * (VMS_IOCTL_CREPRC_SYNC, rd vms-416d), as $CREPRC does -- never a host pipe
 * (rd vms-ff3). The creator OPENs the record and forks; the task ARRIVEs and
 * waits; the creator ACCEPTs it, has the executive make its PCB and ANSWERs;
 * the task REPORTs it has its answer and the creator reads that RESULT before
 * it CLOSEs the record, so closing never cuts the answer off. A party that dies
 * ends the other's wait in the executive. With no executive the OPEN fails:
 * there is no PCB to make, so the task is forked and released as it always
 * was, and *pcb_status carries the OPEN's honest status.
 */
static inline pid_t ovmx_creprc_fork_rec(uint32_t kind, const char *username,
                                         uint32_t uic, uint64_t privs, uint32_t mbxunt,
                                         uint32_t *pcb_status, uint32_t *vms_pid)
{
    uint32_t rec = 0, st, task, go_pid = 0, report = 0;
    pid_t pid;

    if (pcb_status)
        *pcb_status = SS$_ABORT;
    if (vms_pid)
        *vms_pid = 0;
    /* NEGCTL-ANCHORED (subprocess-fork-host-pipe) */
    st = vms_kif_creprc_sync(VMS_CREPRC_SYNC_OPEN, &rec, NULL, NULL, NULL);
    if (!(st & 1)) {
        if (pcb_status)
            *pcb_status = st;
        return fork();
    }

    pid = fork();
    if (pid == 0) {
        /* Child: wait until the creator has made this task its subprocess (or
         * given up), then say so. Released either way, as before. */
        uint32_t got = SS$_ABORT, got_pid = 0;
        if (vms_kif_creprc_sync(VMS_CREPRC_SYNC_ARRIVE, &rec, NULL, &got, &got_pid) & 1)
            (void)vms_kif_creprc_sync(VMS_CREPRC_SYNC_REPORT, &rec, NULL, &got, NULL);
        return 0;
    }
    st = SS$_ABORT;
    task = (uint32_t)pid;
    if (pid > 0 &&
        (vms_kif_creprc_sync(VMS_CREPRC_SYNC_ACCEPT, &rec, &task, NULL, NULL) & 1)) {
        st = mbxunt ? vms_kif_creprc_pcb_mbx(task, kind, mbxunt, &go_pid)
                    : vms_kif_creprc_pcb(task, kind, username, uic, privs, &go_pid);
        if (vms_kif_creprc_sync(VMS_CREPRC_SYNC_ANSWER, &rec, NULL, &st, &go_pid) & 1)
            (void)vms_kif_creprc_sync(VMS_CREPRC_SYNC_RESULT, &rec, NULL, &report, NULL);
    }
    (void)vms_kif_creprc_sync(VMS_CREPRC_SYNC_CLOSE, &rec, NULL, NULL, NULL);
    if (pcb_status)
        *pcb_status = st;
    if (vms_pid && (st & 1))
        *vms_pid = go_pid;
    return pid;
}

static inline pid_t ovmx_creprc_fork_as(uint32_t kind, const char *username,
                                        uint32_t uic, uint64_t privs,
                                        uint32_t *pcb_status, uint32_t *vms_pid)
{
    return ovmx_creprc_fork_rec(kind, username, uic, privs, 0, pcb_status, vms_pid);
}

static inline pid_t ovmx_creprc_fork_mbx(uint32_t kind, uint32_t mbxunt,
                                         uint32_t *pcb_status, uint32_t *vms_pid)
{
    return ovmx_creprc_fork_rec(kind, (const char *)0, 0, 0, mbxunt, pcb_status, vms_pid);
}

#endif /* OVMX_SUBPROCESS_H */
