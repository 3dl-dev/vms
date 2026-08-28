/*
 * ovmx_boot_acp_read.h - PID 1's ACP-read bootstrap bridge (vms-5f0).
 *
 * See ovmx_boot_acp_read.c for the model (and the no-POSIX-fallback / INV-6
 * contract). These read the genuine ODS-2 boot volume THROUGH the executive
 * Files-11 ACP; `acp_path` is a '/'-separated Linux-form path into the volume
 * (a leading "/vms" component, if present, is stripped by imgact_acp.c), e.g.
 * "/vms/SYS0/SYSCOMMON/SYSEXE/DCL.EXE".
 */
#ifndef OVMX_BOOT_ACP_READ_H
#define OVMX_BOOT_ACP_READ_H

#include <stdint.h>

/* True iff `acp_path` is present on the ACP-mounted boot volume (a pure
 * IO$_ACCESS probe). Fail-honest: absent file or no executive both read as 0. */
int ovmx_boot_acp_present(const char *acp_path);

/* Copy `acp_path` off the ACP-mounted boot volume into the tmpfs file `dest`
 * (mode 0755). Returns a VMS status (bit 0 set == success): SS$_NORMAL, or the
 * ACP open status on a read failure, or SS$_ABORT on a tmpfs write failure. */
uint32_t ovmx_boot_acp_stage(const char *acp_path, const char *dest);

#endif /* OVMX_BOOT_ACP_READ_H */
