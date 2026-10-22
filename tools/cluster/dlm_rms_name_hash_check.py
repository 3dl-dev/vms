#!/usr/bin/env python3
"""dlm_rms_name_hash_check.py -- does the PROVEN directory-hash function answer
for a BINARY resource name, and is its answer the one the real VAX put on the
wire? (rd vms-326.)

usage: dlm_rms_name_hash_check.py <pcap|dir> ...     the per-frame check
       dlm_rms_name_hash_check.py --selftest <repo>  pin the headline figures

WHY THIS EXISTS, and why it is a different question from the name's FORM.
tools/cluster/dlm_rms_name_census.py measures WHAT a real VAX calls an RMS
file-access lock -- the 26-byte 'RMS$' + 6 FID bytes + 0x02 + 12-byte volume
label + three NULs. This measures whether OVMX can ROUTE such a name.

The executive used to measure a resource's name with strnlen, so it hashed the
SEVEN or EIGHT bytes its first NUL ended and asked whatever directory node that
value named. rd vms-326 makes the name length-counted end to end, so the
executive now hashes all 26. That is a change to a value that goes ON THE WIRE
at body[128:132], and a WRONG value there is not benign: the directory node
scans the wrong chain, concludes the name is unknown, and installs the sender as
the master of a resource somebody else already masters -- the measured
35-per-second grant storm.

So the new value is checked against the real one. For every real cat-0x02 frame
in the corpus that names an RMS file-access lock, this compares
src/kernel-core/vms_dlm_hash.c's function (in its lab-tooling Python twin,
tools/cluster/dlm_hash/refhash.py, which is pinned to the C by
check_heldout_run.py) with the value the real VMS node itself put at
body[128:132].

WHAT THE RESULT MEANS. The function was determined black-box from 963 (name,
value) pairs and proven on 253 held out plus 55 pre-registered on live hardware
-- but every row of that corpus is a TEXT name. These 26-byte names with NULs
inside them are a family the determination never saw, at a name LENGTH (26) that
the coverage claims by exact-value membership. Agreement here is therefore a
fresh held-out confirmation on the exact family the counted-name change makes
reachable; a disagreement would mean OVMX must withhold the lookup for this
whole family (hash_known 0) and say so, not route by an extrapolation.

CLEAN-ROOM / Rule 8: this compares against values real VMS nodes broadcast IN
THE CLEAR on the cluster wire, which is the evidence class Baron's ruling on rd
vms-dc2 permits for this one function. Nothing is derived from a VSI/HPE binary,
disassembly, listing or source.
"""
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'dlm_hash'))
import remaster_census as rc      # noqa: E402
import refhash                    # noqa: E402

BODY_LEN = 132
REQ_CAT = 0x02
REAL_OUI = ('08002b', 'aa0004')
PREFIX = b'RMS$'

#
# THE cat-0x02 OPCODES THAT CARRY A RESOURCE NAME, AND THE HASH BESIDE IT, at
# the same four positions on each (vms_cluster_codec_dlm.h "THE DIRECTORY
# ROLE"): the ENQ lookup, the release, the directory-entry removal, the convert
# and the rebuild registration. All five are checked, because all five are
# frames this executive builds from the same resource block.
#
NAME_OPS = (0x01, 0x03, 0x04, 0x07, 0x0d)

OFF_NAME_LEN = 47
OFF_NAME = 48
OFF_GROUP = 44
OFF_ACMODE = 46
OFF_HASH = 128


def le32(body, off):
    return int.from_bytes(body[off:off + 4], 'little')


def rms_named_requests(paths):
    """Real-sender cat-0x02 requests of NAME_OPS naming an RMS$ resource."""
    rows = []
    for p in paths:
        for idx, src, _dst, body in rc.sca_bodies_with_src(p):
            if len(body) < BODY_LEN or body[8] != REQ_CAT:
                continue
            if body[9] not in NAME_OPS:
                continue
            if src[:3].hex() not in REAL_OUI:
                continue
            n = body[OFF_NAME_LEN]
            if n == 0 or n > 31:
                continue
            name = bytes(body[OFF_NAME:OFF_NAME + n])
            if not name.startswith(PREFIX):
                continue
            rows.append((os.path.basename(p), idx, name, bytes(body)))
    return rows


def computed_vs_wire(name, body):
    """(computed, wire) for this frame's identity, or (None, wire)."""
    wire = le32(body, OFF_HASH)
    try:
        got = refhash.name_hash(name, body[OFF_ACMODE],
                                int.from_bytes(body[OFF_GROUP:OFF_GROUP + 2],
                                               'little'))
    except ValueError:
        return None, wire
    return got, wire


def truncated_vs_wire(name, body):
    """The value the PREDECESSOR would have produced: the name up to its first
    NUL. Reported so the check shows what it is protecting against -- an
    executive that routed by this value asked the wrong directory node."""
    cut = name.split(b'\0')[0]
    if not cut:
        return None
    try:
        return refhash.name_hash(cut, body[OFF_ACMODE],
                                 int.from_bytes(body[OFF_GROUP:OFF_GROUP + 2],
                                                'little'))
    except ValueError:
        return None


def report(paths):
    rows = rms_named_requests(paths)
    print("real cat-0x02 requests naming an RMS file-access lock: %d"
          % len(rows))
    if not rows:
        return 0
    agree = differ = uncomputable = 0
    trunc_agree = 0
    first_bad = None
    for _cap, _idx, name, body in rows:
        got, wire = computed_vs_wire(name, body)
        if got is None:
            uncomputable += 1
        elif got == wire:
            agree += 1
        else:
            differ += 1
            if first_bad is None:
                first_bad = (name, got, wire)
        t = truncated_vs_wire(name, body)
        if t is not None and t == wire:
            trunc_agree += 1
    print("the COUNTED name's computed hash == body[128:132]: %d of %d "
          "(%d differ, %d uncomputable)"
          % (agree, len(rows), differ, uncomputable))
    print("the TRUNCATED name's (the predecessor's) == body[128:132]: %d of %d"
          % (trunc_agree, len(rows)))
    if first_bad:
        print("  first disagreement: name=%r computed=0x%08x wire=0x%08x"
              % first_bad)
    lens = collections.Counter(len(nm) for _c, _i, nm, _b in rows)
    print("name lengths: %s" % sorted(lens.items()))
    idents = collections.Counter(
        (int.from_bytes(b[OFF_GROUP:OFF_GROUP + 2], 'little'), b[OFF_ACMODE])
        for _c, _i, _n, b in rows)
    print("identities (group, access mode): %s" % idents.most_common())
    return 0


def selftest(repo):
    paths = rc.walk_inputs([os.path.join(repo, 'tests', 'lab', 'captures')])
    rows = rms_named_requests(paths)
    ok = True

    def check(name, got, want):
        nonlocal ok
        good = got == want
        ok = ok and good
        print('  [%s] %s: %r%s' % ('PASS' if good else 'FAIL', name, got,
                                   '' if good else ' (want %r)' % (want,)))

    check('real RMS-named cat-0x02 requests in the corpus', len(rows), 1130)
    check('...of them op-0x01 directory lookups',
          sum(1 for _c, _i, _n, b in rows if b[9] == 0x01), 578)
    # The identity the value is computed over, and the length -- both inside
    # the function's PROVEN coverage (vms_dlm_hash.h: mode 1 observed, group 0
    # observed, length 26 observed), which is what makes computing it allowed.
    check('every one at (group 0, access mode 1)',
          sorted({(int.from_bytes(b[OFF_GROUP:OFF_GROUP + 2], 'little'),
                   b[OFF_ACMODE]) for _c, _i, _n, b in rows}), [(0, 1)])
    check('every name 26 bytes',
          sorted({len(nm) for _c, _i, nm, _b in rows}), [26])
    check('every name contains a NUL',
          sum(1 for _c, _i, nm, _b in rows if b'\0' in nm), len(rows))

    # *** THE RESULT THE COUNTED-NAME CHANGE RESTS ON. ***
    agree = sum(1 for _c, _i, nm, b in rows
                if computed_vs_wire(nm, b)[0] == computed_vs_wire(nm, b)[1])
    check('the COUNTED name hashes to the value the real VAX sent', agree,
          len(rows))

    # ...and the predecessor's truncated name does not, on any of them: the
    # defect was not harmless, and this is the measurement that says so.
    trunc = sum(1 for _c, _i, nm, b in rows
                if truncated_vs_wire(nm, b) == computed_vs_wire(nm, b)[1])
    check('the TRUNCATED name hashes to it on none of them', trunc, 0)

    print('dlm_rms_name_hash_check selftest: %s'
          % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main(argv):
    if len(argv) >= 2 and argv[1] == '--selftest':
        repo = argv[2] if len(argv) > 2 else os.path.join(HERE, '..', '..')
        return selftest(repo)
    if len(argv) < 2:
        print(__doc__)
        return 2
    return report(rc.walk_inputs(argv[1:]))


if __name__ == '__main__':
    sys.exit(main(sys.argv))
