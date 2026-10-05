/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_l2_station.h - the station (Ethernet SOURCE) address policy for an
 * executive L2 handle (rd vms-1f69). Shared, header-only and dependency-free
 * (it needs only uint8_t in scope), so the SAME rule the executive enforces in
 * VMS_IOCTL_L2_OPEN (src/kernel-core/vms_l2.c) is unit-tested on the host
 * (tests/vmsdecnet/test_l2_station.c) without a kernel.
 *
 * WHY A STATION ADDRESS. A DECnet Phase IV node does not talk from its NIC's
 * burned-in address: its Ethernet station address is ALGORITHMIC, the HIORD
 * prefix AA-00-04-00 followed by the 16-bit DECnet address (area<<10 | node)
 * low byte first (DNA Phase IV Routing spec, "Ethernet data link"; the
 * vms-3be lab capture shows a real VAX 1.1 sourcing from AA-00-04-00-01-04).
 * NETACP must therefore be able to SOURCE frames from that address, and to
 * receive unicast addressed to it (the executive already puts the NIC in
 * promiscuous mode on L2_OPEN for the cluster wire's identical aa:00:04:00
 * SCA address, exec_kbackend_linux.h exec_l2_open, so such unicast reaches the
 * handle's socket).
 *
 * THE RULE (privileged PHY_IO open; anything else is SS$_BADPARAM). A caller
 * may request, as the station address its handle sources every frame from:
 *   - all zero        -> the NIC's own hardware address (the pre-vms-1f69
 *                        behaviour; every in-kernel caller and every caller
 *                        that does not ask stays exactly as it was);
 *   - the NIC hwaddr  -> accepted (an explicit spelling of the default);
 *   - AA-00-04-00-xx-xx with xx-xx != 00-00
 *                     -> accepted: the DECnet Phase IV algorithmic block.
 *                        (00-00 is DECnet address 0.0, which names no node --
 *                        it is never a station.)
 * Anything else -- a different vendor's unicast, a multicast/broadcast, an
 * arbitrary spoof -- is REFUSED. Holding PHY_IO lets a process do raw L2 I/O;
 * it does not let it impersonate an arbitrary other station. This is OVMX's
 * stated policy (CLAUDE.md Rule 8): the AA-00-04-00 block and the algorithm
 * are public DNA values; the decision to gate the source address at OPEN is
 * the executive's own.
 */
#ifndef _VMS_L2_STATION_H
#define _VMS_L2_STATION_H

#define VMS_L2_STATION_USE_NIC    1   /* requested all-zero: source = NIC hwaddr */
#define VMS_L2_STATION_OK         0   /* requested address accepted verbatim     */
#define VMS_L2_STATION_REFUSED  (-1)  /* not the NIC and not Phase IV -> BADPARAM */

/* The DECnet Phase IV HIORD prefix (public DNA value). */
#define VMS_L2_DNA_HIORD0 0xAAu
#define VMS_L2_DNA_HIORD1 0x00u
#define VMS_L2_DNA_HIORD2 0x04u
#define VMS_L2_DNA_HIORD3 0x00u

static inline int vms_l2_station_check(const uint8_t req[6], const uint8_t nic[6])
{
    int i, zero = 1, is_nic = 1;

    for (i = 0; i < 6; i++) {
        if (req[i] != 0)
            zero = 0;
        if (req[i] != nic[i])
            is_nic = 0;
    }
    if (zero)
        return VMS_L2_STATION_USE_NIC;
    if (is_nic)
        return VMS_L2_STATION_OK;
    if (req[0] == VMS_L2_DNA_HIORD0 && req[1] == VMS_L2_DNA_HIORD1 &&
        req[2] == VMS_L2_DNA_HIORD2 && req[3] == VMS_L2_DNA_HIORD3 &&
        (req[4] != 0 || req[5] != 0))
        return VMS_L2_STATION_OK;
    return VMS_L2_STATION_REFUSED;
}

/* The Phase IV algorithmic station address for DECnet address area.node:
 * AA-00-04-00 then LE16(area<<10 | node). Pure; for callers and tests. */
static inline void vms_l2_dna_station(unsigned area, unsigned node, uint8_t out[6])
{
    unsigned a = ((area & 0x3fu) << 10) | (node & 0x3ffu);

    out[0] = VMS_L2_DNA_HIORD0;
    out[1] = VMS_L2_DNA_HIORD1;
    out[2] = VMS_L2_DNA_HIORD2;
    out[3] = VMS_L2_DNA_HIORD3;
    out[4] = (uint8_t)(a & 0xffu);
    out[5] = (uint8_t)((a >> 8) & 0xffu);
}

#endif /* _VMS_L2_STATION_H */
