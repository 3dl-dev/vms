/*
 * vms_process.c - Process identity queries and UIC conversion
 *
 * Implements the four functions declared in vms/process.h that had no
 * source file (vms-115): vms_get_current_process, vms_pid_from_linux,
 * vms_format_uic, vms_parse_uic.
 *
 * VMS PID mapping: OVMX runs one VMS process per Linux process — see
 * src/libvms/syssvc/sys_process.c ("VMS processes are mapped to Linux
 * processes using fork()/exec(), with VMS PIDs being Linux PIDs").
 * vms_pid_from_linux is the single place that encodes that identity
 * mapping, matching the "%08X" 8-hex-digit VMS PID display convention
 * already used by dcl_lexical.c (F$PID) and sys_process.c.
 *
 * UIC packing/format: a VMS UIC (User Identification Code) is a 32-bit
 * longword with the group number in bits <31:16> and the member number
 * in bits <15:0> (OpenVMS System Services Reference Manual, $GETUAI /
 * UAI$_UIC item code; OpenVMS DCL Dictionary, SET UIC). Both fields are
 * conventionally displayed in octal as "[group,member]". This matches
 * the packing already used elsewhere in this codebase — SET UIC
 * (src/vmsdcl/dcl_cmd_set.c cmd_set_uic), rms_get_session_uic
 * (src/vmsrms/rms_core.c), and vmssshd.c's SYSUAF UIC assembly — all of
 * which use (group << 16) | member and "[%03o,%03o]" formatting. This
 * implementation reuses that established convention rather than
 * inventing a new one.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "vms/process.h"
#include "vms/pcb.h"

/*
 * Default VMS base priority for an interactive process (OpenVMS default
 * UAF PRIORITY value for non-batch, non-realtime accounts). No other
 * base-priority constant exists yet in this codebase; this is the
 * documented OpenVMS default and is used only as a fallback when the
 * PCB does not carry a priority of its own.
 */
#define VMS_DEFAULT_BASE_PRIORITY 4

uint32_t vms_pid_from_linux(pid_t pid)
{
    return (uint32_t)pid;
}

void vms_format_uic(uint32_t uic, char *buf, size_t bufsize)
{
    unsigned int group, member;

    if (!buf || bufsize == 0)
        return;

    group  = (uic >> 16) & 0xFFFFu;
    member = uic & 0xFFFFu;

    snprintf(buf, bufsize, "[%03o,%03o]", group, member);
}

uint32_t vms_parse_uic(const char *str)
{
    unsigned int group = 0, member = 0;

    if (!str)
        return 0;

    /* Strip a leading '[' (and matching trailing ']', if present) —
     * VMS UICs are conventionally written "[group,member]", but the
     * bracket-free "group,member" form is accepted too. Same strategy
     * as cmd_set_uic() in src/vmsdcl/dcl_cmd_set.c. */
    if (*str == '[')
        str++;

    if (sscanf(str, "%o,%o", &group, &member) != 2)
        return 0;

    return ((group & 0xFFFFu) << 16) | (member & 0xFFFFu);
}

vms_process_t *vms_get_current_process(void)
{
    static __thread vms_process_t proc;
    struct vms_pcb *pcb = vms_pcb_get();

    memset(&proc, 0, sizeof(proc));

    proc.linux_pid = getpid();
    proc.vms_pid   = vms_pid_from_linux(proc.linux_pid);
    proc.base_priority = VMS_DEFAULT_BASE_PRIORITY;

    if (pcb) {
        /* Prefer the identity recorded in the PCB (set via
         * vms_pcb_set_identity), falling back to the Linux-derived PID
         * if the PCB hasn't been assigned a VMS PID yet. */
        if (pcb->vms_pid != 0)
            proc.vms_pid = pcb->vms_pid;

        proc.uic = pcb->uic;
        strncpy(proc.username, pcb->username, sizeof(proc.username) - 1);
        strncpy(proc.prcnam, pcb->prcnam, sizeof(proc.prcnam) - 1);
    }

    return &proc;
}
