/*
 * PRXDEF.H - VMS Proxy Login (PRX$) Flag Definitions
 *
 * OpenVMX compatibility layer - Defines the PRX$M_ flag bits used
 * with sys$add_proxy, sys$display_proxy, sys$verify_proxy, and
 * sys$delete_proxy to manage network proxy login records.
 *
 * PROVENANCE: the proxy-services flags mechanism is documented in the
 * public OpenVMS System Services Reference Manual ($ADD_PROXY,
 * $DELETE_PROXY, $DISPLAY_PROXY). Exact numeric values not confirmed
 * against a fetchable public source this session — sequential OVMX
 * assignment, flagged in vms-531 findings.
 *
 * Reference: OpenVMS System Services Reference Manual ($ADD_PROXY,
 *            $DELETE_PROXY, $DISPLAY_PROXY, $VERIFY_PROXY)
 */

#ifndef __PRXDEF_H
#define __PRXDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * PRX$M_ — proxy-service flag bits
 * ================================================================ */

#define PRX$M_DEFAULT    0x00000001  /* Mark/select the default proxy */
#define PRX$M_EXACT      0x00000002  /* Require an exact node/user match */

#ifdef __cplusplus
}
#endif

#endif /* __PRXDEF_H */
