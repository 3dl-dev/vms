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
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_TABLE = os.path.join(HERE, '..', '..', 'tests', 'cluster', 'conformance',
                             'frame-grounding.tsv')
CHECKER = os.path.join(HERE, 'frame_conformance.py')

# (category, opcode) -> the grounding table's type name. ONLY pairs a row really
# covers appear here: an unmapped pair is reported as uncovered, never guessed into
# the nearest-looking type.
TYPE_OF = {
    (0x01, 0x03): 'cm_commit',
    (0x04, 0x00): 'cm_ack',
    (0x01, 0x14): 'cm_model',
    (0x8b, 0x01): 'cm_0b01_ans',
    (0x8b, 0x04): 'cm_lnm_ans',
    (0x0b, 0x05): 'cm_lnm_dbreq',
    (0x02, 0x01): 'dlm_enq_req',
    (0x02, 0x04): 'dlm_dir_remove',
    (0x82, 0x0d): 'cm_dlm_op0d_echo',
}
BODY_OFF = 72          # the SYSAP body's frame-absolute offset, and the base every
                       # CM/DLM row in the table is written against
BODY_LEN = 132         # the SYSAP body a row describes
FRAME_LEN = 204        # a VMS$VAXcluster frame
DIR_ANS_STATUS = 34    # body[34] on a cat-0x82 op-0x01, the directory's outcome


def dir_answer_type(body):
    """The directory answer's grounding type, chosen by the outcome byte.

    One (category, opcode) pair, two SHAPES: on 'you master it' body[28:32] is a
    cleared cell and on a redirect it is the master's CSID, so one row set that
    described both would describe neither. A status this codec never builds gets
    no type at all -- it is reported UNCOVERED rather than squeezed into the
    nearest-looking row set.
    """
    return {0xf9: 'dlm_dir_ans_you', 0xf8: 'dlm_dir_ans_redir'}.get(
        body[DIR_ANS_STATUS])


# (category, opcode) pairs whose type a byte of the body decides. Kept apart from
# TYPE_OF so the flat table stays readable as "one pair, one shape".
DISCRIMINATED = {
    (0x82, 0x01): dir_answer_type,
}


def type_of(body):
    """The grounding table's type name for one 132-byte SYSAP body, or None."""
    key = (body[8], body[9])
    if key in TYPE_OF:
        return TYPE_OF[key]
    pick = DISCRIMINATED.get(key)
    return pick(body) if pick is not None else None


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


def classify(fs):
    """(covered, uncovered): the frames whose SHAPE the table describes, paired
    with their type name, and a per-(category, opcode) count of the ones it does
    not describe a byte of."""
    covered, uncovered = [], collections.Counter()
    for b in fs:
        body = b[BODY_OFF:BODY_OFF + BODY_LEN]
        typ = type_of(body)
        if typ is None:
            uncovered[(body[8], body[9])] += 1
        else:
            covered.append((typ, b))
    return covered, uncovered


def check_fields(covered, table):
    """Run the per-field checker over the covered frames: (fails, unchecked).

    A row whose rule is `state:` asks the EMITTING TEST what the executive held
    at the moment it built the frame. A capture cannot answer that, so such a
    row is reported UNCHECKED rather than failed -- calling it a failure would
    be as false as calling it a pass.
    """
    # The table's CM/DLM rows are written against the SYSAP body at abs 72, which
    # is the base the host tests dump with; a capture carries the whole 204-byte
    # frame, so hand the checker the same span it was written for.
    dump = tempfile.NamedTemporaryFile('w', suffix='.capconf', delete=False)
    with dump:
        for typ, b in covered:
            dump.write('%s %d - %s\n'
                       % (typ, BODY_OFF, b[BODY_OFF:BODY_OFF + BODY_LEN].hex()))
    out = subprocess.run([sys.executable, '-I', CHECKER, table, dump.name],
                         capture_output=True, text=True)
    os.unlink(dump.name)
    unchecked, real = [], []
    for line in (out.stdout + out.stderr).splitlines():
        (unchecked if 'not recorded by the emitting test' in line
         else real).append(line)
    fails = [l for l in real
             if l.strip().startswith('FAIL') and
             not l.startswith('frame_conformance: ')]
    return fails, unchecked


def report(port, to_port, fs, covered, uncovered, table):
    """The human reading: how much of what this node really sent is described."""
    print('%s emitted %d frames%s: %d of a covered type, %d of an UNCOVERED type'
          % (port, len(fs), ' to ' + to_port if to_port else '',
             len(covered), sum(uncovered.values())))
    if not uncovered:
        return
    print('\nUNCOVERED -- no row in %s describes a byte of these:'
          % os.path.basename(table))
    for (cat, op), n in sorted(uncovered.items(), key=lambda kv: -kv[1]):
        print('    cat 0x%02x op 0x%02x   %5d frames' % (cat, op, n))


def report_fields(covered, fails, unchecked):
    print('\n--- %s over the %d covered frames ---'
          % (os.path.basename(CHECKER), len(covered)))
    print('%d field failure(s) among the covered frames%s'
          % (len(fails), ':' if fails else ''))
    print('\n'.join(fails))
    if not unchecked:
        return
    print('\n%d field(s) a capture cannot check (the row reads the emitter\'s own '
          'state):' % len(unchecked))
    seen = set()
    for line in unchecked:
        key = line.split('fails row')[-1]
        if key not in seen:
            seen.add(key)
            print('   ' + line.strip())


def as_json(port, to_port, fs, covered, uncovered, fails):
    """The machine reading, for the ratchet (tests/cluster/conformance)."""
    return {
        'port': port, 'to': to_port, 'frames': len(fs),
        'covered': len(covered),
        'uncovered': {'%02x/%02x' % k: n for k, n in sorted(uncovered.items())},
        'fails': fails,
    }


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    path, port = argv[1], argv[2]
    to_port = argv[argv.index('--to') + 1] if '--to' in argv else None
    table = (argv[3] if len(argv) > 3 and not argv[3].startswith('--')
             else DEFAULT_TABLE)
    want_json = '--json' in argv

    fs = frames(path, port, to_port)
    if not fs:
        sys.exit('no 204-byte frames from %s%s'
                 % (port, ' to ' + to_port if to_port else ''))
    covered, uncovered = classify(fs)
    fails = []
    if covered:
        fails, unchecked = check_fields(covered, table)
    if want_json:
        json.dump(as_json(port, to_port, fs, covered, uncovered, fails),
                  sys.stdout)
        print()
        return 1 if fails else 0
    report(port, to_port, fs, covered, uncovered, table)
    if covered:
        report_fields(covered, fails, unchecked)
        return 1 if fails else 0
    return 1 if uncovered else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
