#!/usr/bin/env python3
"""scan_connect_wire.py - census the SCS CONNECT_REQ frames in a rig pcap
(rd vms-4838, the EVACUATE->REJOIN proof).

WHAT THIS IS FOR. rd vms-4838's fix (join_cm_take_held(), src/kernel-core/
vms_cnxman_join_fsm.c) makes a REJOINING node adopt the executive's own
already-held Con.ID for its pair with a member instead of opening a second
VMS$VAXcluster connection of its own. The observable consequence on the wire
is that a rejoining node's own passive capture of ITS OWN boot contains ZERO
VMS$VAXcluster CONNECT_REQ frames whose Ethernet SOURCE address is that node's
own NIC -- everything it does from there rides the connection the OTHER side
already opened. A first join is the opposite: the codec's own reference drive
(E67) has it dial out, so its capture shows at least one.

Like scan_dlm_wire.py, this decodes ONLY published offsets of this codebase's
own wire codec -- nothing here is a guessed byte position, and nothing here
is VMS's own unpublished internals (Rule 8): every offset cited below is a
`#define` in src/kernel-core/vms_cluster_codec.h, the SAME header and the
SAME classification rule (VMS_FCLS_SCS_CONN_CTRL, vms_cluster_codec.c
g_rules[]) the executive's own parser applies to every inbound frame.

THE DISCRIMINATOR, exactly as the C classifier applies it:
  ethertype == 0x6007                    VMS_SCA_ETHERTYPE
  SCA content length == 110              content = LE16(payload[0]) + 2
                                          (VMS_OFF_SCA_LEN, "+2 == content");
                                          110 is the ONLY content class that
                                          carries the two 16-byte SYSAP names
                                          (a CONNECT_REQ/RSP/ACCEPT/REJECT
                                          body), spec sec 4(h)(1)/(1a)
  msgtype (payload[16]) in               VMS_OFF_SCS_MSGTYPE == abs 30;
    {MSG, SETUP, ALT}                    {0x4b, 0x5b, 0x7b}
  ctrl_type (payload[46], LE16) == 0     VMS_OFF_SCS_CTRL_TYPE == abs 60;
                                          VMS_SCS_CTRL_CONNECT_REQ == 0
                                          (CONNECT_RSP=1, ACCEPT_REQ=2, ...)

WHAT TELLS ORIGIN. The Ethernet source address (payload precedes it; it is
abs 6 of the frame, an ordinary 802.3 field, not a VMS wire field at all) --
compared against the two MACs the caller supplies. A frame this node's own
NIC put on the wire has THAT MAC at abs 6; one it received has the PEER's.

Usage:
  scan_connect_wire.py --self-mac AA:BB:.. --peer-mac CC:DD:.. <file.pcap>
Prints one line: connect_req_from_self=N connect_req_from_peer=M
                 connect_req_from_other=K sca_frames=T
(from_other counts a CONNECT_REQ whose source matches NEITHER supplied MAC --
 always 0 on this rig's 2-node segment; reported rather than silently folded
 into either bucket, so a MAC typo shows up as a number instead of a false
 zero.)
"""
import argparse
import struct
import sys

ETH_TYPE_SCA = 0x6007
OFF_ETH_SRC = 6
OFF_ETHERTYPE = 12
OFF_SCA_LEN = 14            # VMS_OFF_SCA_LEN: LE16, +2 == content length
OFF_SCS_MSGTYPE = 30        # VMS_OFF_SCS_MSGTYPE
OFF_SCS_CTRL_TYPE = 60      # VMS_OFF_SCS_CTRL_TYPE, LE16

CONN_CTRL_CONTENT = 110     # the CONNECT_REQ/RSP/ACCEPT/REJECT content class
MSGTYPES_CONN_CTRL = (0x4b, 0x5b, 0x7b)   # MT_MSG, MT_SETUP, MT_ALT
CTRL_TYPE_CONNECT_REQ = 0   # VMS_SCS_CTRL_CONNECT_REQ


def pcap_records(blob):
    """Yield each record's packet bytes. Handles both pcap endiannesses."""
    if len(blob) < 24:
        return
    magic = blob[:4]
    if magic == b"\xd4\xc3\xb2\xa1":
        end = "<"
    elif magic == b"\xa1\xb2\xc3\xd4":
        end = ">"
    else:
        return
    off = 24
    while off + 16 <= len(blob):
        _, _, incl, _ = struct.unpack_from(end + "IIII", blob, off)
        off += 16
        if off + incl > len(blob):
            return
        yield blob[off:off + incl]
        off += incl


def is_sca(pkt):
    if len(pkt) < OFF_ETHERTYPE + 2:
        return False
    return struct.unpack_from(">H", pkt, OFF_ETHERTYPE)[0] == ETH_TYPE_SCA


def is_connect_req(pkt):
    """This codebase's own VMS_FCLS_SCS_CONN_CTRL classification, narrowed to
    the CONNECT_REQ op specifically -- never a byte position guessed off a
    hexdump, see the module docstring for every offset's source line."""
    if len(pkt) < OFF_SCA_LEN + 2:
        return False
    content = struct.unpack_from("<H", pkt, OFF_SCA_LEN)[0] + 2
    if content != CONN_CTRL_CONTENT:
        return False
    if len(pkt) < OFF_SCA_LEN + content:
        return False   # the frame is shorter than its own declared content
    if pkt[OFF_SCS_MSGTYPE] not in MSGTYPES_CONN_CTRL:
        return False
    if len(pkt) < OFF_SCS_CTRL_TYPE + 2:
        return False
    ctrl_type = struct.unpack_from("<H", pkt, OFF_SCS_CTRL_TYPE)[0]
    return ctrl_type == CTRL_TYPE_CONNECT_REQ


def eth_src_str(pkt):
    return ":".join("%02x" % b for b in pkt[OFF_ETH_SRC:OFF_ETH_SRC + 6])


def norm_mac(s):
    return s.strip().lower()


def scan(path, self_mac, peer_mac):
    try:
        with open(path, "rb") as fh:
            blob = fh.read()
    except OSError as exc:
        return "%s: unreadable (%s)" % (path, exc)

    sca = 0
    from_self = 0
    from_peer = 0
    from_other = 0
    others = []
    for pkt in pcap_records(blob):
        if not is_sca(pkt):
            continue
        sca += 1
        if not is_connect_req(pkt):
            continue
        src = eth_src_str(pkt)
        if src == self_mac:
            from_self += 1
        elif src == peer_mac:
            from_peer += 1
        else:
            from_other += 1
            if len(others) < 8:
                others.append(src)

    line = ("%s: sca_frames=%d connect_req_from_self=%d "
            "connect_req_from_peer=%d connect_req_from_other=%d"
            % (path, sca, from_self, from_peer, from_other))
    if others:
        line += "\n  OTHER-SRC connect_req_other_macs=%s" % ",".join(others)
    return line


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-mac", required=True)
    ap.add_argument("--peer-mac", required=True)
    ap.add_argument("pcaps", nargs="+")
    args = ap.parse_args(argv[1:])

    self_mac = norm_mac(args.self_mac)
    peer_mac = norm_mac(args.peer_mac)
    for path in args.pcaps:
        print(scan(path, self_mac, peer_mac))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
