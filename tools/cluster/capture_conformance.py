#!/usr/bin/env python3
"""capture_conformance.py <frames.jsonl> <port> [grounding.tsv] -- check the frames a
REAL OVMX node put on a REAL wire against the frame-grounding table.

WHY THIS EXISTS BESIDE frame_conformance.py. That checker reads a DUMP the host tests
write (conformance_dump.h): it proves the senders this repo can drive under ctest emit
grounded bytes. It cannot see a frame type no host test drives, and it cannot see what a
booted node actually sent a real VAX. This one starts from a CAPTURE -- the demo page's
L2 switch log, or a pcap converted to it -- so the question it answers is the one a crash
asks: "of everything this node really sent that system, which frames does the gate
actually cover, and do the covered ones hold?"

It classifies each emitted 204-byte frame by its SYSAP (category, opcode) and reports:
  - frames whose type HAS a grounding row: checked, field by field, by the same
    tools/cluster/frame_conformance.py this repo already gates with;
  - frames whose type has NO row at all: listed, loudly, as UNCOVERED. A type OVMX emits
    at a system that has not proved it runs this implementation and that no row covers is
    a frame nobody has checked a byte of.

usage:
  capture_conformance.py frames.jsonl OVMXB
  capture_conformance.py frames.jsonl OVMXB --to VAXC      # only frames addressed there
"""
import collections
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_TABLE = os.path.join(HERE, '..', '..', 'tests', 'cluster', 'conformance',
                             'frame-grounding.tsv')
CHECKER = os.path.join(HERE, 'frame_conformance.py')

# (category, opcode) -> the grounding table's type name. ONLY pairs a row really
# covers appear here: an unmapped pair is reported as uncovered, never guessed into
# the nearest-looking type.
TYPE_OF = {
    (0x01, 0x03): 'cm_commit',
    (0x01, 0x14): 'cm_model',
    (0x8b, 0x01): 'cm_0b01_ans',
    (0x8b, 0x04): 'cm_lnm_ans',
    (0x0b, 0x05): 'cm_lnm_dbreq',
    (0x02, 0x04): 'dlm_dir_remove',
}
BODY_OFF = 72          # the SYSAP body's frame-absolute offset, and the base every
                       # CM/DLM row in the table is written against
BODY_LEN = 132         # the SYSAP body a row describes
FRAME_LEN = 204        # a VMS$VAXcluster frame


def frames(path, port, to_port):
    """Every 204-byte frame `port` sent (optionally only those addressed to `to_port`)."""
    macs = {}
    rows = []
    for line in open(path):
        line = line.strip()
        if not line:
            continue
        try:
            r = json.loads(line)
            b = bytes.fromhex(r['hex'])
        except Exception:
            continue
        macs.setdefault(r['port'], b[6:12])
        rows.append((r['port'], b))
    dst = macs.get(to_port) if to_port else None
    if to_port and dst is None:
        sys.exit('capture_conformance.py: %s never transmitted, so its address is '
                 'unknown -- cannot filter on it' % to_port)
    out = []
    for p, b in rows:
        if p != port or len(b) != FRAME_LEN:
            continue
        if dst is not None and b[0:6] != dst:
            continue
        out.append(b)
    return out


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    path, port = argv[1], argv[2]
    to_port = argv[argv.index('--to') + 1] if '--to' in argv else None
    table = argv[3] if len(argv) > 3 and not argv[3].startswith('--') else DEFAULT_TABLE

    fs = frames(path, port, to_port)
    if not fs:
        sys.exit('no 204-byte frames from %s%s' % (port, ' to ' + to_port if to_port else ''))

    covered, uncovered = [], collections.Counter()
    for b in fs:
        key = (b[BODY_OFF + 8], b[BODY_OFF + 9])
        typ = TYPE_OF.get(key)
        if typ is None:
            uncovered[key] += 1
        else:
            covered.append((typ, b))

    print('%s emitted %d frames%s: %d of a covered type, %d of an UNCOVERED type'
          % (port, len(fs), ' to ' + to_port if to_port else '',
             len(covered), sum(uncovered.values())))
    if uncovered:
        print('\nUNCOVERED -- no row in %s describes a byte of these:' % os.path.basename(table))
        for (cat, op), n in sorted(uncovered.items(), key=lambda kv: -kv[1]):
            print('    cat 0x%02x op 0x%02x   %5d frames' % (cat, op, n))
    if not covered:
        return 1 if uncovered else 0

    # The table's CM/DLM rows are written against the SYSAP body at abs 72, which
    # is the base the host tests dump with; a capture carries the whole 204-byte
    # frame, so hand the checker the same span it was written for.
    dump = '/tmp/.capture_conformance_dump'
    with open(dump, 'w') as f:
        for typ, b in covered:
            f.write('%s %d - %s\n'
                    % (typ, BODY_OFF, b[BODY_OFF:BODY_OFF + BODY_LEN].hex()))
    print('\n--- %s over the %d covered frames ---'
          % (os.path.basename(CHECKER), len(covered)))
    out = subprocess.run([sys.executable, '-I', CHECKER, table, dump],
                         capture_output=True, text=True)
    os.unlink(dump)
    # A row whose rule is `state:` asks the EMITTING TEST what the executive held
    # at the moment it built the frame. A capture cannot answer that, so such a
    # row is reported UNCHECKED here rather than failed: calling it a failure
    # would be as false as calling it a pass.
    unchecked, real = [], []
    for line in (out.stdout + out.stderr).splitlines():
        (unchecked if 'not recorded by the emitting test' in line else real).append(line)
    real = [l for l in real if not l.startswith('frame_conformance: ')]
    fails = [l for l in real if l.strip().startswith('FAIL')]
    print('%d field failure(s) among the covered frames%s'
          % (len(fails), ':' if fails else ''))
    print('\n'.join(fails))
    if unchecked:
        print('\n%d field(s) a capture cannot check (the row reads the emitter\'s own '
              'state):' % len(unchecked))
        seen = set()
        for line in unchecked:
            key = line.split('fails row')[-1]
            if key not in seen:
                seen.add(key)
                print('   ' + line.strip())
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
