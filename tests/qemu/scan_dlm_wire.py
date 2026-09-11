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

# src/kernel-core/vms_cluster_codec_dlm.h, the wire-op block.
OPNAMES = {
    0x01: "enq",
    0x03: "deq",         # the cross-node RELEASE (vms-c03)
    0x04: "blkast",      # the master->holder BLOCKING AST (vms-c03)
    0x06: "grant",
    0x07: "convert",
    0x0d: "rebuild",
}


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


def scan(path):
    try:
        with open(path, "rb") as fh:
            blob = fh.read()
    except OSError as exc:
        return "%s: unreadable (%s)" % (path, exc)

    sca = 0
    counts = {}
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

    if not counts:
        return "%s: sca_frames=%d dlm=none" % (path, sca)
    body = " ".join("%s=%d" % (k, counts[k]) for k in sorted(counts))
    return "%s: sca_frames=%d %s" % (path, sca, body)


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    for path in argv[1:]:
        print(scan(path))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
