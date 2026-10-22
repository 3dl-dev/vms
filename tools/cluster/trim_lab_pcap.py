#!/usr/bin/env python3
"""Trim a lab capture to the frames an arm's claims rest on (rd vms-c2b / vms-6ad1).

A RULE, not a hand-picked window, in two passes:
  pass 1 -- SELECT every 0x6007 SCS-MSG frame whose body names RA7T / S0 / S1 /
            S2, plus every frame carrying a lock-tree remaster opcode
            (cat 0x02 op 0x0e..0x14, or the 0x1a this arm saw refused);
  pass 2 -- ALSO keep every frame sharing a selected frame's SCS transaction id
            at body[4:8]. This is what keeps the ANSWERS: a cat-0x82 op-0x01
            grant overwrites the name span with the resource's value block, so a
            name rule alone drops exactly the frames that carry the handles.
Everything else in the pod's own br0 traffic (the VAXes' F11B$/CACHE$ chatter)
is dropped. Frame payloads are never altered.
usage: trim_pcap.py <in.pcap> <out.pcap>
"""
import struct, sys

# RA7U joins RA7T (2026-10-22): the rd vms-85f re-fire had to move to a fresh
# tree name because the pod's RA7T carried a GRANTED EX whose owner was gone --
# a leftover of three OVMX node cycles in one pod, which left the arm's own
# step 1 in LEF. Same sequence, a tree with no residue.
NAMES = (b'RA7T', b'RA7U', b'RA7V', b'S0', b'S1', b'S2')
RM_OPS = set(range(0x0e, 0x15)) | {0x1a}


def frames(data):
    off = 24
    while off + 16 <= len(data):
        hdr = data[off:off + 16]
        _ts, _us, caplen, _orig = struct.unpack('<IIII', hdr)
        frame = data[off + 16:off + 16 + caplen]
        off += 16 + caplen
        yield hdr, frame


def is_scs_msg(frame):
    return (len(frame) == 204 and frame[12:14] == b'\x60\x07'
            and frame[31] == 0x13 and frame[55] == 0x02)


def selected(body):
    nm = bytes(body[48:68]).rstrip(b'\x20\x00')
    cat, op = body[8], body[9]
    return nm in NAMES or (cat in (0x02, 0x82) and op in RM_OPS)


data = open(sys.argv[1], 'rb').read()
txns = set()
for _hdr, frame in frames(data):
    if is_scs_msg(frame) and selected(frame[72:]):
        t = bytes(frame[72 + 4:72 + 8])
        if t != b'\0\0\0\0':
            txns.add(t)

out = bytearray(data[:24])
kept = total = 0
for hdr, frame in frames(data):
    total += 1
    if not is_scs_msg(frame):
        continue
    body = frame[72:]
    if selected(body) or bytes(body[4:8]) in txns:
        out += hdr + frame
        kept += 1
open(sys.argv[2], 'wb').write(bytes(out))
print("trimmed %s: %d of %d frames kept" % (sys.argv[2], kept, total))
