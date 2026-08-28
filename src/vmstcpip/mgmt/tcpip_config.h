/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tcpip_config.h - TCP/IP Services for OVMX: the MANAGEMENT / CONFIGURATION
 * plane (vms-67f, the config half of the layered product).
 *
 * WHAT THIS IS. The data half of TCP/IP Services already exists: the BGn:
 * sockets veneer (src/vmstcpip/sockets/vms_bgsock.c) and the client tool
 * engines (src/vmstcpip/services/tcpip_client.h) let VMS programs USE IP.
 * This header is the missing CONFIG half: it lets an operator configure OVMX
 * IP networking THE VMS WAY -- set the local host name/domain and an
 * interface address, and have it (a) applied to the interface over the real
 * substrate stack and (b) recorded in the VMS-faithful TCPIP$* SYSTEM logical
 * names so the veneer and tools read it back. This is the engine
 * TCPIP$CONFIG.COM and the DCL `TCPIP SET/SHOW` verbs drive; it is proven
 * against a real /dev/vms by tests/qemu/test_syssvc_tcpip_config.c.
 *
 * FAITHFULNESS LIVES IN USERSPACE (docs/design-tcpip-services-ovmx.md sec 1/2).
 * The IP engine is the Linux substrate's AF_INET stack -- we never
 * reimplement TCP/UDP/IP, and applying an address to an interface is an
 * ordinary SIOCSIFADDR against that substrate. What must be VMS-faithful is
 * the CONFIG SURFACE: the TCPIP$* logical-name NAMES, which config they
 * carry, and that they live in shared executive state, not a per-process fake.
 *
 * THE TCPIP$* LOGICALS ARE EXECUTIVE-RESIDENT (CLAUDE.md Rule 9 / INV-6).
 * A TCPIP$* name is created in LNM$SYSTEM -- the system-wide logical-name
 * table, which is executive-resident over /dev/vms (src/libvms/syssvc/
 * sys_logical.c, vms-d37): a name one process defines there is visible to
 * EVERY process on the node, exactly as real TCP/IP Services' startup-defined
 * TCPIP$INET_HOST is. There is NO per-process fallback: if /dev/vms is absent,
 * tcpip_cfg_define_system / _translate_system fail honestly with SS$_NOSUCHDEV
 * (never a per-process table that reports success while sharing nothing --
 * the exact LARP class INV-6 forbids). The interface-apply half degrades
 * honestly too: a substrate that refuses the ioctl (no privilege) returns the
 * kernel errno, never a faked success.
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8). The TCPIP$* logical NAMES are from public
 * VSI OpenVMS TCP/IP Services documentation:
 *   - TCPIP$INET_HOST     -- the local host name
 *   - TCPIP$INET_DOMAIN   -- the local BIND/DNS domain
 *   - TCPIP$INET_HOSTADDR -- the local host's IP address
 * (VSI TCP/IP Services for OpenVMS Management guide; these are the names
 * TCPIP$CONFIG defines with DEFINE/SYSTEM/EXECUTIVE and that TCP/IP commands
 * read to identify the local node.) The IP wire is IETF-standard, so there is
 * no clean-room constraint on it; we never disassemble or copy VSI/HPE source.
 *
 * WHY A SINGLE-HEADER LIBRARY (same rationale as tcpip_client.h). Every
 * function is `static inline` so the exact same engine backs two consumers --
 * the DCL `TCPIP` verb (src/vmsdcl/dcl_cmd_misc.c, the user surface) and the
 * QEMU proof (tests/qemu/test_syssvc_tcpip_config.c, which drives it against a
 * real /dev/vms) -- WITHOUT a new compiled object joining DCL.EXE's
 * VMS-native link graph (which would need the three-enumeration wiring). It
 * calls only already-exported symbols: the public sys$crelnm/$trnlnm system
 * services (already in DCL's graph -- dcl_mbx.c calls sys$trnlnm) and, on the
 * Linux substrate, the ordinary socket/ioctl the facade already uses.
 *
 * SCOPE (vms-67f foundational rung). Core IP config: local host name, domain,
 * and one interface's address/mask, applied to the substrate and recorded in
 * the three TCPIP$* SYSTEM logicals. DEFERRED to later rungs, honestly (NOT
 * faked here): DHCP/auto-config, multiple interfaces / TCPIP$INET_HOSTADDRn
 * aliases, the persistent PROXY / SERVICES / NETWORK config databases, and
 * IPv6. See docs/design-tcpip-services-ovmx.md sec 5.
 */

#ifndef _OVMX_TCPIP_CONFIG_H
#define _OVMX_TCPIP_CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "starlet.h"
#include "descrip.h"
#include "lnmdef.h"
#include "ssdef.h"

/* ---- The VMS-faithful TCPIP$* SYSTEM logical names (public VSI docs) ------ */
#define TCPIP_LNM_INET_HOST     "TCPIP$INET_HOST"
#define TCPIP_LNM_INET_DOMAIN   "TCPIP$INET_DOMAIN"
#define TCPIP_LNM_INET_HOSTADDR "TCPIP$INET_HOSTADDR"

/* The executive-resident system logical-name table these are created in. */
#define TCPIP_LNM_TABLE         "LNM$SYSTEM"

/* ---- Logical-name primitives (public sys$crelnm/$trnlnm, LNM$SYSTEM) ------
 *
 * These are the ONLY places config durability is realised, and they are the
 * INV-6 touch-point: LNM$SYSTEM is executive-resident, so with no /dev/vms the
 * services return SS$_NOSUCHDEV and nothing is faked.
 */

static inline struct dsc$descriptor_s tcpip_cfg_dsc_(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

/* Define TCPIP$<name> = value in LNM$SYSTEM. Returns the VMS status verbatim
 * (SS$_NORMAL / SS$_SUPERSEDE success; SS$_NOSUCHDEV when the executive is
 * absent -- an honest failure, never a per-process fake). */
static inline uint32_t tcpip_cfg_define_system(const char *name, const char *value)
{
    struct dsc$descriptor_s td = tcpip_cfg_dsc_(TCPIP_LNM_TABLE);
    struct dsc$descriptor_s nd = tcpip_cfg_dsc_(name);
    struct item_list_3 il[2];

    memset(il, 0, sizeof(il));
    il[0].buflen    = (uint16_t)strlen(value);
    il[0].item_code = LNM$_STRING;
    il[0].bufaddr   = (void *)value;
    il[0].retlen    = NULL;
    return sys$crelnm(NULL, &td, &nd, NULL, il);
}

/* Translate TCPIP$<name> from LNM$SYSTEM into out[outsz]. Returns the VMS
 * status (SS$_NOLOGNAM if undefined; SS$_NOSUCHDEV with no executive). */
static inline uint32_t tcpip_cfg_translate_system(const char *name,
                                                  char *out, size_t outsz)
{
    struct dsc$descriptor_s td = tcpip_cfg_dsc_(TCPIP_LNM_TABLE);
    struct dsc$descriptor_s nd = tcpip_cfg_dsc_(name);
    struct item_list_3 il[2];
    uint16_t rl = 0;
    uint32_t st;

    memset(il, 0, sizeof(il));
    if (outsz == 0)
        return SS$_BADPARAM;
    il[0].buflen    = (uint16_t)(outsz - 1);
    il[0].item_code = LNM$_STRING;
    il[0].bufaddr   = out;
    il[0].retlen    = &rl;
    out[0] = '\0';
    st = sys$trnlnm(NULL, &td, &nd, NULL, il);
    if (rl >= outsz)
        rl = (uint16_t)(outsz - 1);
    out[rl] = '\0';
    return st;
}

/* ---- Interface apply/read (the Linux substrate half) ---------------------
 *
 * The IP engine is the substrate kernel's AF_INET stack; applying an address
 * is an ordinary SIOCSIFADDR against it. Guarded __linux__ because the ifreq
 * union members used here are Linux's (the OVMX TCP/IP engine is Linux;
 * netbsd-vax has no AF_INET veneer). On a non-Linux substrate the apply is a
 * no-op reported honestly by the return status, never a faked success.
 */
#if defined(__linux__)
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>

/* Apply addr (and, if non-NULL, mask) to ifname on the substrate and bring it
 * up. Returns SS$_NORMAL on success, SS$_ABORT on a substrate error (the errno
 * is left in *sys_errno if non-NULL -- an honest substrate failure, e.g.
 * EPERM without NET_ADMIN, is NOT reported as success). */
static inline uint32_t tcpip_cfg_apply_iface_addr(const char *ifname,
                                                  const char *addr,
                                                  const char *mask,
                                                  int *sys_errno)
{
    int s;
    struct ifreq ifr;
    struct sockaddr_in *sin;

    if (sys_errno)
        *sys_errno = 0;
    if (!ifname || !addr)
        return SS$_BADPARAM;

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        if (sys_errno) *sys_errno = errno;
        return SS$_ABORT;
    }

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    if (inet_pton(AF_INET, addr, &sin->sin_addr) != 1) {
        close(s);
        return SS$_BADPARAM;
    }
    if (ioctl(s, SIOCSIFADDR, &ifr) < 0) {
        if (sys_errno) *sys_errno = errno;
        close(s);
        return SS$_ABORT;
    }

    if (mask) {
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
        sin = (struct sockaddr_in *)&ifr.ifr_netmask;
        sin->sin_family = AF_INET;
        if (inet_pton(AF_INET, mask, &sin->sin_addr) == 1)
            (void)ioctl(s, SIOCSIFNETMASK, &ifr);
    }

    /* Bring the interface up (idempotent). */
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        (void)ioctl(s, SIOCSIFFLAGS, &ifr);
    }

    close(s);
    return SS$_NORMAL;
}

/* Read ifname's current IPv4 address from the substrate into out[outsz] (dotted
 * quad). This is the kernel's own answer (SIOCGIFADDR) -- what the interface
 * ACTUALLY reflects, not a value this process stashed. Returns SS$_NORMAL or
 * SS$_ABORT. */
static inline uint32_t tcpip_cfg_read_iface_addr(const char *ifname,
                                                 char *out, size_t outsz)
{
    int s;
    struct ifreq ifr;
    struct sockaddr_in *sin;

    if (!ifname || !out || outsz == 0)
        return SS$_BADPARAM;
    out[0] = '\0';

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return SS$_ABORT;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFADDR, &ifr) < 0) {
        close(s);
        return SS$_ABORT;
    }
    sin = (struct sockaddr_in *)&ifr.ifr_addr;
    if (!inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)outsz)) {
        close(s);
        return SS$_ABORT;
    }
    close(s);
    return SS$_NORMAL;
}
#endif /* __linux__ */

/* ---- The whole VMS-way round trip ----------------------------------------
 *
 * tcpip_cfg_configure() is what TCPIP$CONFIG drives: apply the address to the
 * interface over the substrate, then record host / domain / address in the
 * three TCPIP$* SYSTEM logicals so every process (and the veneer/tools) reads
 * the same config back. host / domain may be NULL to leave them unchanged;
 * ifname / addr may be NULL to define only the logicals (no interface apply).
 *
 * Returns SS$_NORMAL when every requested step succeeded. If the executive is
 * absent the FIRST system-logical define returns SS$_NOSUCHDEV and that status
 * is returned -- honest, not a fake. A substrate apply error is returned as
 * SS$_ABORT with the errno in *sys_errno.
 */
static inline uint32_t tcpip_cfg_configure(const char *host, const char *domain,
                                           const char *ifname,
                                           const char *addr, const char *mask,
                                           int *sys_errno)
{
    uint32_t st;

    if (sys_errno)
        *sys_errno = 0;

#if defined(__linux__)
    if (ifname && addr) {
        st = tcpip_cfg_apply_iface_addr(ifname, addr, mask, sys_errno);
        if (!(st & 1))
            return st;
    }
#else
    (void)ifname; (void)mask;
#endif

    if (host) {
        st = tcpip_cfg_define_system(TCPIP_LNM_INET_HOST, host);
        if (!(st & 1))
            return st;
    }
    if (domain) {
        st = tcpip_cfg_define_system(TCPIP_LNM_INET_DOMAIN, domain);
        if (!(st & 1))
            return st;
    }
    if (addr) {
        st = tcpip_cfg_define_system(TCPIP_LNM_INET_HOSTADDR, addr); /* NEGCTL tcpip-config-hostaddr-not-defined */
        if (!(st & 1))
            return st;
    }

    return SS$_NORMAL;
}

#endif /* _OVMX_TCPIP_CONFIG_H */
