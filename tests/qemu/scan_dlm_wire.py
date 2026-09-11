#!/usr/bin/env python3
"""scan_dlm_wire.py - count the cat-0x02 DLM opcodes in a rig pcap (rd vms-94c).

WHAT THIS IS FOR, AND WHAT IT IS NOT EVIDENCE OF. The cross-node proof needs
two independent facts and this file supplies exactly one of them:

  "a byte reached the segment"   <- THIS. The rig's passive per-node capture
      (sca_l2probe in recv-only mode; it never transmits) reconstructed to a
      pcap, decoded here.
  "which executive emitted it"   <- NOT this. Nothing on a shared segment can
      say that. Only the counters the arm itself incremented at the moment it
      sent can, and they are read back through VMS_IOCTL_CLUSTER_DIAG_DLM.

So a run is believed when BOTH agree, and this half is the weaker one: it is
reported beside the executive's, never instead of it.

EVERY OFFSET HERE IS A PUBLISHED CONSTANT OF THIS CODEBASE, not a byte position
guessed off a hexdump:

  ethertype 0x6007          the SCA cluster protocol (VMS_ETH_TYPE_SCA)
  frame[80] = category      src/kernel-core/vms_cluster_codec_dlm.h
                            VMS_OFF_DLM_CAT  ("body[8] category: 0x02 req")
  frame[81] = opcode        VMS_OFF_DLM_OP   ("body[9] opcode")

where frame[0] is the first byte of the Ethernet header, which is what a pcap
record holds. The DLM opcode names come from the same header's wire-op block.

Usage:  scan_dlm_wire.py <file.pcap> [<file.pcap> ...]
Prints one line per file: the per-opcode counts it really found.
"""
import struct
import sys

ETH_TYPE_SCA = 0x6007
OFF_ETHERTYPE = 12          # standard Ethernet II
OFF_DLM_CAT = 80            # VMS_OFF_DLM_CAT
OFF_DLM_OP = 81             # VMS_OFF_DLM_OP
CAT_DLM_REQUEST = 0x02
RESPONSE_BIT = 0x80

# src/kernel-core/vms_cluster_codec_dlm.h, the wire-op block. 0x06 is
# VMS_DLM_WIREOP_CONVERT_VALBLK -- the CONVERT that carries the lock value
# block (rd vms-727) -- NOT a grant; VMS never puts a grant in its own cat-0x02
# opcode (a grant is the op-0x01/0x07 cat-0x82 RESPONSE, see the codec header).
OPNAMES = {
    0x01: "enq",
    0x03: "deq",         # the cross-node RELEASE (vms-c03)
    0x04: "blkast",      # the master->holder BLOCKING AST (vms-c03)
    0x06: "valblk",       # CONVERT-with-VALBLK, the op-0x06 LVB write (vms-727)
    0x07: "convert",
    0x0d: "rebuild",
}

# src/kernel-core/vms_cluster_codec_dlm.h VMS_OFF_DLM_MASTER_LKID / _VALBLK.
OFF_DLM_MASTER_LKID = 96     # body[24:28] LE u32
OFF_DLM_VALBLK = 108         # body[36:52], 16 bytes
DLM_VALBLK_LEN = 16
WIREOP_CONVERT_VALBLK = 0x06


def pcap_records(blob):
    """Yield each record's packet bytes. Handles both endiannesses."""
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


def dlm_opcode(pkt):
    """The cat-0x02 opcode this frame carries, or None if it is not DLM."""
    if len(pkt) <= OFF_DLM_OP:
        return None
    cat = pkt[OFF_DLM_CAT]
    if (cat & ~RESPONSE_BIT) != CAT_DLM_REQUEST:
        return None
    return (cat & RESPONSE_BIT, pkt[OFF_DLM_OP])


def mac_str(pkt, off):
    return ":".join("%02x" % b for b in pkt[off:off + 6])


def op06_detail(pkt):
    """For an op-0x06 CONVERT-with-VALBLK REQUEST frame: the fields the
    return-contract evidence needs, decoded from this codebase's own
    published offsets -- never guessed off a hexdump. None if the frame is
    too short to hold them."""
    if len(pkt) < OFF_DLM_VALBLK + DLM_VALBLK_LEN:
        return None
    master_lkid = struct.unpack_from("<I", pkt, OFF_DLM_MASTER_LKID)[0]
    valblk = pkt[OFF_DLM_VALBLK:OFF_DLM_VALBLK + DLM_VALBLK_LEN]
    return {
        "eth_dst": mac_str(pkt, 0),
        "eth_src": mac_str(pkt, 6),
        "master_lkid": master_lkid,
        "valblk_hex": valblk.hex(),
        # 0x21 (not 0x20): a literal space would split this token when the
        # host script's `tr ' ' '\n'` scraper tokenises the line.
        "valblk_ascii": "".join(chr(b) if 0x21 <= b < 0x7f else "." for b in valblk),
    }


def scan(path):
    try:
        with open(path, "rb") as fh:
            blob = fh.read()
    except OSError as exc:
        return "%s: unreadable (%s)" % (path, exc)

    sca = 0
    counts = {}
    op06_frames = []
    for pkt in pcap_records(blob):
        if not is_sca(pkt):
            continue
        sca += 1
        got = dlm_opcode(pkt)
        if got is None:
            continue
        resp, op = got
        key = "%s%s" % (OPNAMES.get(op, "op%02x" % op), "-resp" if resp else "")
        counts[key] = counts.get(key, 0) + 1
        if op == WIREOP_CONVERT_VALBLK and not resp:
            detail = op06_detail(pkt)
            if detail is not None:
                op06_frames.append(detail)

    if not counts:
        lines = ["%s: sca_frames=%d dlm=none" % (path, sca)]
    else:
        body = " ".join("%s=%d" % (k, counts[k]) for k in sorted(counts))
        lines = ["%s: sca_frames=%d %s" % (path, sca, body)]

    # THE OP-0x06 WIRE-FRAME EVIDENCE (rd vms-727 return contract): every
    # value below is decoded straight off the frame bytes this node's own
    # passive capture recorded, at the codec's own published offsets --
    # nothing here is asserted, only reported alongside the gate.
    # NOTE: these detail lines are prefixed "op06_" (never bare "master_lkid="
    # or "valblk=") so the host script's `key=value` scrapers -- which sum
    # every token matching a bare opcode-count key across this whole blob --
    # cannot mistake a hex value block for another opcode's count.
    for d in op06_frames[:8]:
        lines.append(
            "  OP06 op06_eth_src=%s op06_eth_dst=%s op06_master_lkid=0x%08x "
            "op06_valblk_hex=%s op06_valblk_ascii=%s"
            % (d["eth_src"], d["eth_dst"], d["master_lkid"],
               d["valblk_hex"], d["valblk_ascii"])
        )
    return "\n".join(lines)


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    for path in argv[1:]:
        print(scan(path))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
