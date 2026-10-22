#!/usr/bin/env python3
"""pcap_to_conformance.py -- put a LAB PCAP's OVMX-originated frames in front of
the frame-conformance checker (rd vms-c2b / rd vms-6ad1 RA7).

run_gate.sh asks the question of OVMX's senders under their host tests.
run_capture_gate.py asks it of the committed .jsonl frame logs, which carry the
sending port's NAME. Neither reaches a lab pcap, and a pcap is what a real-VAX
arm comes home with -- so the frames an OVMX node actually put in front of a
real executive could not be checked against the grounding table at all.

This bridges that: given the OVMX node's source MAC, it writes the
conformance-dump format (tests/cluster/host/conformance_dump.h) for every
SCS-MSG frame that node ORIGINATED whose (category, opcode) the grounding table
covers, so tools/cluster/frame_conformance.py can check it.

   pcap_to_conformance.py <pcap> <src-mac> [--state k=v,...] > dump.txt
   frame_conformance.py tests/cluster/conformance/frame-grounding.tsv dump.txt

WHAT IT DOES NOT DO, and you must read the result knowing it: the frame goes in
AS CAPTURED, including body[0:4] (the connection manager's send/ack message
numbers) and body[4:8] (the SCS transaction id). The grounding table's rows
describe what the BUILDER produced -- the CM stamps those two spans afterwards
-- so those rows are EXPECTED to differ on a wire frame and the checker will say
so. Nothing is zeroed to make a row pass: a tool that edited the bytes before
checking them would be checking its own edit (INV-6).

CLEAN-ROOM: reads captured frames only.
"""
import struct
import sys

SCA_ETHERTYPE = b'\x60\x07'
SCA_FRAME_LEN = 204
BODY_OFF = 72
BODY_LEN = 132

# (category, opcode) -> the grounding table's type name. ONLY pairs a row really
# covers; an unmapped pair is REPORTED on stderr, never guessed into the
# nearest-looking type (tools/cluster/capture_conformance.py's own rule).
TYPE_OF = {
    (0x02, 0x01): 'dlm_enq_req',
    (0x02, 0x04): 'dlm_dir_remove',
    (0x02, 0x14): 'dlm_rm_14',
    (0x01, 0x03): 'cm_commit',
    (0x01, 0x14): 'cm_model',
    (0x04, 0x00): 'cm_ack',
    (0x0b, 0x05): 'cm_lnm_dbreq',
}


def sca_frames(path):
    data = open(path, 'rb').read()
    off = 24
    while off + 16 <= len(data):
        _ts, _us, caplen, _orig = struct.unpack('<IIII', data[off:off + 16])
        off += 16
        frame = data[off:off + caplen]
        off += caplen
        if len(frame) != SCA_FRAME_LEN or frame[12:14] != SCA_ETHERTYPE:
            continue
        if frame[31] != 0x13 or frame[55] != 0x02:
            continue
        yield frame


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    path, mac = argv[0], argv[1].lower().replace(':', '')
    state = '-'
    if len(argv) >= 4 and argv[2] == '--state':
        state = argv[3]
    want = bytes.fromhex(mac)
    emitted, skipped = 0, {}
    for frame in sca_frames(path):
        if frame[6:12] != want:
            continue
        body = frame[BODY_OFF:BODY_OFF + BODY_LEN]
        key = (body[8], body[9])
        if key not in TYPE_OF:
            skipped[key] = skipped.get(key, 0) + 1
            continue
        print("%s %u %s %s" % (TYPE_OF[key], BODY_OFF, state, body.hex()))
        emitted += 1
    sys.stderr.write("pcap_to_conformance: %d frame(s) emitted from %s\n"
                     % (emitted, argv[1]))
    for (cat, op), n in sorted(skipped.items()):
        sys.stderr.write("pcap_to_conformance: no table type for cat %02x op %02x"
                         " (%d frame(s)) -- NOT guessed\n" % (cat, op, n))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
