/*
 * tcpip_inetd_ident.h - establish a TCPIP$INETD service's run-as identity in the
 * forked child before execv (rd vms-8bd, R4 G1).
 *
 * THE VULNERABILITY (docs/security/tcpip-networking-r4-sweep.md, G1): INETD runs
 * as SYSTEM [0,0] with ALL privileges and spawns each service with a raw
 * fork()+execv() and NO privilege drop -- so a memory-corruption bug in any
 * enabled service image (or in INETD's own parse) executes as SYSTEM/all-privs.
 * That is the ceiling on the inbound boundary and the R4 hard gate before any
 * reading/privileged service (the SSH rung) may be enabled.
 *
 * THE FIX -- the same identity-drop LOGINOUT and sshd use (Rule 10, no drift): a
 * service carries a configured SYSUAF account (the SERVICE.DAT USERNAME field).
 * Before execv the (forked) child resolves that account, stamps the EXECUTIVE
 * identity (vms_kif_setident, via ovmx_ssh_establish_identity -- the row survives
 * execv, keyed by tgid; INETD holds SETPRV so it may establish any identity <=
 * its own), then drops its Linux credentials to the account's UIC
 * (ovmx_cred_drop_to_uic). The userspace PCB is NOT the authority (it does not
 * survive execv) -- the executive identity is.
 *
 * FAIL-CLOSED (this IS the security property, CLAUDE.md Rule 9 / INV-6): an empty
 * or unknown account, a refused setident (including /dev/vms absent), or a failed
 * cred drop returns -1 and the caller MUST NOT execv. There is NEVER a
 * run-as-SYSTEM fallback -- that fallback would BE the G1 hole.
 *
 * INJECTABLE + UNIT-TESTABLE: the apply half takes the ovmx_ident_syscalls /
 * ovmx_cred_syscalls tables (the same seams sshd's ssh_ident.c / cred_drop.c use),
 * so the fail-closed policy is exercised with mock tables and no live /dev/vms.
 */
#ifndef _OVMX_TCPIP_INETD_IDENT_H
#define _OVMX_TCPIP_INETD_IDENT_H

#include <stdint.h>

#include "ssh_ident.h"   /* struct ovmx_ident_syscalls, ovmx_ssh_establish_identity */
#include "cred_drop.h"   /* struct ovmx_cred_syscalls,  ovmx_cred_drop_to_uic       */

/*
 * Apply an ALREADY-RESOLVED service identity to the calling (forked, pre-execv)
 * process: stamp the executive identity, then drop Linux credentials. Split from
 * the SYSUAF resolve so this half is fully unit-testable with mock tables.
 * `ident_sc`/`cred_sc` NULL -> the real production tables. Returns 0 on a
 * complete drop; -1 (fail-closed) if the executive refuses the identity or the
 * credential drop fails/does not verify.
 */
int tcpip_inetd_apply_identity(const char *username, uint32_t uic,
                               uint32_t uic_group, uint32_t uic_member,
                               uint64_t privs,
                               const struct ovmx_ident_syscalls *ident_sc,
                               const struct ovmx_cred_syscalls *cred_sc);

/*
 * Resolve `user` in SYSUAF and apply its identity (executive + Linux creds) to
 * the calling child before execv. FAIL-CLOSED: empty/unknown user or any refused
 * step -> -1 (the caller MUST NOT execv, and MUST NOT run the service as SYSTEM).
 * `ident_sc`/`cred_sc` NULL -> the real production tables. Returns 0 on success.
 */
int tcpip_inetd_establish_service_identity(const char *user,
                               const struct ovmx_ident_syscalls *ident_sc,
                               const struct ovmx_cred_syscalls *cred_sc);

#endif /* _OVMX_TCPIP_INETD_IDENT_H */
