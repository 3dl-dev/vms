/*
 * tcpip_inetd_ident.h - resolve a TCPIP$INETD service's run-as identity, in the
 * auxiliary server, before it creates the service's process (rd vms-8bd, R4 G1;
 * rd vms-1f8).
 *
 * THE VULNERABILITY (docs/security/tcpip-networking-r4-sweep.md, G1): INETD runs
 * as SYSTEM [1,4] with ALL privileges, so a service process that inherited its
 * identity would run every enabled service image -- and any memory-corruption
 * bug in it -- as SYSTEM/all-privs. That is the ceiling on the inbound boundary
 * and the R4 hard gate before any reading/privileged service (the SSH rung) may
 * be enabled.
 *
 * THE FIX, THE WAY VMS DOES IT. A service carries a configured SYSUAF account
 * (the SERVICE.DAT USERNAME field). The auxiliary server resolves that account
 * (user name, UIC, authorized privileges) and creates the service process
 * DETACHED with that identity ($CREPRC; VMS_IOCTL_CREPRC_PCB, which checks INETD
 * may give it). The service process never establishes an identity itself.
 *
 * FAIL-CLOSED (this IS the security property, Rule 9 / INV-6): an empty or
 * unknown account, or an unreadable SYSUAF, returns -1 and NO process is
 * created. There is NEVER a run-as-SYSTEM fallback -- that fallback would BE
 * the G1 hole.
 */
#ifndef _OVMX_TCPIP_INETD_IDENT_H
#define _OVMX_TCPIP_INETD_IDENT_H

#include "tcpip_inetd.h"   /* struct tcpip_service, struct tcpip_inetd_identity */

/*
 * Resolve `user` in SYSUAF into *id (user name, UIC, authorized privileges).
 * FAIL-CLOSED: an empty/unknown user or an unreadable SYSUAF -> -1 with the
 * detail on stderr (the operator log); the caller MUST NOT create the service
 * process. Returns 0 on success.
 */
int tcpip_inetd_resolve_account(const char *user, struct tcpip_inetd_identity *id);

/* The tcpip_inetd_identity_fn the production auxiliary server passes:
 * tcpip_inetd_resolve_account(svc->user, id). */
int tcpip_inetd_resolve_service_identity(const struct tcpip_service *svc,
                                         struct tcpip_inetd_identity *id);

#endif /* _OVMX_TCPIP_INETD_IDENT_H */
