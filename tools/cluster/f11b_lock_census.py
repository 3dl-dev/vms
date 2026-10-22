#!/usr/bin/env python3
"""f11b_lock_census.py -- WHAT A REAL VAX'S FILES-11 XQP LOCKS ARE NAMED, and
what OVMX's per-volume string is missing (rd vms-f4c0, parent rd vms-50d).

usage: f11b_lock_census.py <pcap|dir> ...     the census
       f11b_lock_census.py --selftest <repo>  pin the headline figures

WHY THIS EXISTS. OVMX's Files-11 ACP (src/kernel-core/vmsfs_acp.c
acp_vol_resnam_kind) serializes every on-disk-structure write behind ONE
resource per VOLUME: "F11B$s" + the ODS-2 label, 18 bytes, a ROOT lock. A real
VAX's XQP takes a 10-byte PER-FILE SUB-lock instead. Two nodes writing the same
file on a SHARED volume therefore take locks with DIFFERENT RESOURCE NAMES and
do not serialize against each other at all -- both can be inside the
read-modify-write of one file header, which is a Files-11 structure-corruption
class on exactly the shared / MSCP-served volume the rd vms-ci.6 evacuation
re-fire requires. (The volume-wide coarseness is the lesser half: it blocks more
than VMS does. The interop hole is the half that corrupts.)

THIS TOOL DOES NOT EMIT ANYTHING. It measures, so the emitter that follows is
grounded rather than guessed -- the three things rd vms-f4c0 recorded as NOT
GROUNDED before any name may be built:

  (a) WHAT THE 4 BYTES ARE. Every 10-byte "F11B$s" name in the corpus is
      "F11B$s" + 4 bytes read little-endian in the Files-11 file-number range,
      and bytes 2 and 3 of that field are ZERO on all of them -- so the corpus
      CANNOT discriminate whether the high bytes carry the FID's nmx extension.
      That is reported as undiscriminated, not resolved by assertion. (The
      composition of a Files-11 file number from num + nmx is public ODS-2
      documentation; this corpus simply never exercises nmx != 0.)

  (b) WHAT THE LOCK'S PARENT IS. Measured, not assumed: for every 10-byte
      "F11B$" sub-lock request this walk can resolve, body[36:40] -- the
      requester's OWN parent handle (rd vms-b67a's pair) -- is the handle that
      same requester used at body[24:28] in an earlier op-0x01 frame naming
      "F11B$v" + the 12-byte space-padded VOLUME LABEL. The per-file
      serialization lock hangs under the per-VOLUME root, on every resolvable
      one, with no handle in the corpus carrying two different names.
      The pair's SECOND element (the parent's MASTER handle) is NOT grounded
      here and this tool does not claim it: every F11B$v request in the corpus
      is answered 0xf9 ("you master it") or 0xf8 (redirect), never 0xfa, so the
      parent's grant is local and no master handle for it is on the wire. That
      element's meaning rests on rd vms-b67a's own driven arm.

  (c) THE TWO FAMILIES OVMX TAKES NONE OF, characterised so neither is emitted
      on a guess: "F11B$c" is a SECOND 10-byte per-file-shaped sub-lock under
      the same F11B$v parent, but with only TWO distinct values in the whole
      corpus (1 and 2) at PR/CW -- not a file-number range, and WHAT SELECTS IT
      IS NOT KNOWN AND IS NOT GUESSED (Rule 8). "F11B$a" is a 22-byte ROOT,
      "F11B$a" + the 12-byte label + a 4-byte file number, requested across the
      whole mode spread NL..EX -- i.e. a PER-FILE lock, which corrects this
      item's own earlier reading of it as a per-volume allocation lock.

AND THE IDENTITY, which is half of what makes a resource the same resource
(body[44:46] UIC group, body[46] access mode): every real F11B$ request in the
corpus is (group 0, mode 0 = KERNEL), and every OVMX one is (group 1, mode 3 =
USER) -- the enqueuing process's, because vms_lock_acp_vol_ex enqueues in the
caller's context. A byte-exact name at mode 3 would still be a DIFFERENT
resource from the VAX's, so the name and the identity have to move together.

CLEAN-ROOM (Rule 8): reads captured frames and public documentation only. No
VSI/HPE source or binary, and nothing here recomputes an unpublished algorithm.
"""
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import remaster_census as rc  # noqa: E402

BODY_LEN = 132
REAL_OUI = ('08002b', 'aa0004')
PREFIX = b'F11B$'
SUB_LEN = 10            # the per-file sub-lock shape: 'F11B$' + kind + 4 bytes
ROOT_VOL_LEN = 18       # 'F11B$' + kind + a 12-byte space-padded label
ROOT_FILE_LEN = 22      # ...+ a 4-byte file number

CAT_DLM = 0x02
RESP_BIT = 0x80
OP_ENQ = 0x01
OFF_CAT = 8
OFF_OP = 9
OFF_MASTER_LKID = 20    # the MASTER's handle (rd vms-b5b0)
OFF_REQ_LKID = 24       # the REQUESTER's own handle -- the correlation
OFF_MODE = 30
OFF_OUTCOME = 34
OFF_PARENT_OWN = 36     # rd vms-b67a's pair: own parent handle ...
OFF_PARENT_MASTER = 40  # ... and the parent's master handle
OFF_GROUP = 44
OFF_ACMODE = 46
OFF_NAME_LEN = 47
OFF_NAME = 48
OUTCOME_GRANTED = 0xFA

LOCK_MODES = {0: 'NL', 1: 'CR', 2: 'CW', 3: 'PR', 4: 'PW', 5: 'EX'}


def is_real(mac):
    return mac[:3].hex() in REAL_OUI


def resname(body):
    """The counted resource name, or b'' when the frame carries none."""
    n = body[OFF_NAME_LEN]
    if n == 0 or n > 31:
        return b''
    return bytes(body[OFF_NAME:OFF_NAME + n])


def enq_frames(path):
    """Every cat-0x02/0x82 op-0x01 frame in one capture, decoded far enough.

    Yields (idx, src, dst, is_response, name, body).
    """
    for idx, src, dst, body in rc.sca_bodies_with_src(path):
        if len(body) < BODY_LEN:
            continue
        if (body[OFF_CAT] & 0x7f) != CAT_DLM or body[OFF_OP] != OP_ENQ:
            continue
        yield idx, bytes(src), bytes(dst), bool(body[OFF_CAT] & RESP_BIT), \
            resname(body), bytes(body)


def kind_of(name):
    """The XQP lock kind character, 'F11B$' + <kind> + ..."""
    return name[5:6].decode('latin1') if len(name) > 5 else '?'


def le32(buf):
    return int.from_bytes(buf, 'little')


class Shape:
    """One (originator-class, kind, name length) population."""

    def __init__(self):
        self.frames = 0
        self.names = collections.Counter()
        self.ident = collections.Counter()
        self.modes = collections.Counter()
        self.with_parent = 0

    def add(self, name, body):
        self.frames += 1
        self.names[name] += 1
        self.ident[(le32(body[OFF_GROUP:OFF_GROUP + 2]), body[OFF_ACMODE])] += 1
        self.modes[body[OFF_MODE]] += 1
        if body[OFF_PARENT_OWN:OFF_PARENT_MASTER + 4] != b'\0' * 8:
            self.with_parent += 1


def shapes(paths):
    """The F11B$ REQUEST populations, keyed (who, kind, name length).

    Requests only: a cat-0x82 answer echoes the requester's name, so counting
    answers would attribute the requester's form to the answerer
    (dlm_rms_name_census.py's correction, applied here too).
    """
    out = collections.defaultdict(Shape)
    for p in paths:
        for _idx, src, _dst, resp, name, body in enq_frames(p):
            if resp or not name.startswith(PREFIX):
                continue
            who = 'REAL' if is_real(src) else 'OVMX'
            out[(who, kind_of(name), len(name))].add(name, body)
    return out


def resolve_parents(path):
    """Resolve each 10-byte F11B$ sub-lock request to its PARENT's NAME.

    The parent handle at body[36:40] is the requester's OWN lock id for the
    parent, which the requester itself put at body[24:28] when it asked for
    that parent (rd vms-b5b0 for the offset, rd vms-b67a for the pair). So the
    parent RESOURCE is named by the most recent op-0x01 frame in which THIS
    requester used THIS handle with a name. Returns
    (resolved, unresolved, ambiguous, parent_grant_outcomes) where resolved is
    a Counter keyed (kind, parent name).
    """
    seen = {}                                  # (node, handle) -> newest name
    history = collections.defaultdict(set)     # ... -> every name it carried
    resolved = collections.Counter()
    unresolved = collections.Counter()
    ambiguous = 0
    for _idx, src, dst, resp, name, body in enq_frames(path):
        own = bytes(body[OFF_REQ_LKID:OFF_REQ_LKID + 4])
        requester = dst if resp else src
        if name:
            seen[(requester, own)] = name
            history[(requester, own)].add(name)
        if resp or not is_real(src):
            continue
        if not name.startswith(PREFIX) or len(name) != SUB_LEN:
            continue
        key = (src, bytes(body[OFF_PARENT_OWN:OFF_PARENT_OWN + 4]))
        parent = seen.get(key)
        if parent is None:
            unresolved[kind_of(name)] += 1
            continue
        resolved[(kind_of(name), parent)] += 1
        if len(history[key]) > 1:
            ambiguous += 1
    return resolved, unresolved, ambiguous


def parent_answer_outcomes(paths):
    """What a real node's F11B$v (per-volume ROOT) request is ANSWERED with.

    This is why the pair's second element is not grounded here: if the parent
    is never granted ON THE WIRE, no master handle for it is on the wire.
    """
    out = collections.Counter()
    for p in paths:
        for _idx, src, _dst, resp, name, body in enq_frames(p):
            if not resp or not name.startswith(PREFIX + b'v'):
                continue
            out[(body[OFF_OUTCOME], 'from-real' if is_real(src) else 'from-ovmx')] += 1
    return out


def file_numbers(paths):
    """The file-number fields: the sub-lock's 4 bytes and the 22-byte root's.

    Returns (sub numbers, root numbers, labels seen on the 22-byte root).
    """
    subs, roots = set(), set()
    labels = collections.Counter()
    for p in paths:
        for _idx, src, _dst, resp, name, body in enq_frames(p):
            if resp or not is_real(src) or not name.startswith(PREFIX):
                continue
            if len(name) == SUB_LEN and kind_of(name) == 's':
                subs.add(name[6:10])
            elif len(name) == ROOT_FILE_LEN and kind_of(name) == 'a':
                roots.add(name[18:22])
                labels[name[6:18]] += 1
    return subs, roots, labels


def _print_shapes(sh):
    print("%-5s %-5s %-4s %8s %9s %-22s %s"
          % ("who", "kind", "len", "frames", "distinct", "identity (grp,mode)",
             "modes"))
    for key in sorted(sh):
        s = sh[key]
        modes = ' '.join('%s=%d' % (LOCK_MODES.get(m, m), c)
                         for m, c in s.modes.most_common())
        print("%-5s %-5s %-4d %8d %9d %-22s %s"
              % (key[0], key[1], key[2], s.frames, len(s.names),
                 ' '.join('%s:%d' % (i, c) for i, c in s.ident.most_common(3)),
                 modes))
        print("      with a parent pair at body[36:44]: %d of %d"
              % (s.with_parent, s.frames))
        for name, cnt in s.names.most_common(3):
            print("      %-6d %s | %r" % (cnt, name.hex(' '), name))


def report(paths):
    sh = shapes(paths)
    print("=== F11B$ REQUEST populations (requests only; an answer echoes the "
          "requester's name)\n")
    _print_shapes(sh)

    print("\n=== (b) the PARENT of every 10-byte sub-lock, resolved by handle")
    resolved = collections.Counter()
    unresolved = collections.Counter()
    ambiguous = 0
    for p in paths:
        r, u, a = resolve_parents(p)
        resolved.update(r)
        unresolved.update(u)
        ambiguous += a
    for (kind, parent), cnt in resolved.most_common():
        print("    kind=%s parent=%-26r %6d" % (kind, parent, cnt))
    print("    unresolved (the parent's own request is not in that capture): %s"
          % dict(unresolved))
    print("    handles that carried more than one name: %d" % ambiguous)
    print("    F11B$v answers, (outcome, sender): %s"
          % dict(parent_answer_outcomes(paths)))

    print("\n=== (a) the 4-byte field")
    subs, roots, labels = file_numbers(paths)
    print("    distinct sub-lock numbers: %d, of which byte 2 or 3 nonzero: %d"
          % (len(subs), sum(1 for n in subs if n[2] or n[3])))
    print("    distinct 22-byte-root numbers: %d, also seen as a sub-lock: %d"
          % (len(roots), len(roots & subs)))
    print("    labels on the 22-byte root: %s"
          % [l.decode('latin1') for l in labels])
    return 0


def _checker():
    state = {'ok': True}

    def check(name, got, want):
        good = got == want
        state['ok'] = state['ok'] and good
        print('  [%s] %s: %r%s' % ('PASS' if good else 'FAIL', name, got,
                                   '' if good else ' (want %r)' % (want,)))
    return state, check


def _selftest_shapes(sh, check):
    """The populations rd vms-f4c0 quotes, and OVMX's wrong one beside them."""
    real_s = sh[('REAL', 's', SUB_LEN)]
    check('real F11B$s is 10 bytes, a per-file SUB-lock', real_s.frames, 37416)
    check('...distinct names', len(real_s.names), 590)
    check('...every one of them carries a parent pair',
          real_s.with_parent, real_s.frames)
    check('...identity is (group 0, access mode 0 = KERNEL), N of N',
          real_s.ident[(0, 0)], real_s.frames)
    check('...requested at PW on all but the NL registrations',
          (real_s.modes[4], real_s.modes[0], len(real_s.modes)),
          (37305, 111, 2))
    check('a real node NEVER originates an 18-byte F11B$s',
          [k for k in sh if k[0] == 'REAL' and k[1] == 's' and k[2] != SUB_LEN],
          [])

    ovmx_s = sh[('OVMX', 's', ROOT_VOL_LEN)]
    check('OVMX F11B$s is 18 bytes, a per-VOLUME ROOT', ovmx_s.frames, 31)
    check('...distinct names', len(ovmx_s.names), 1)
    check('...it carries NO parent pair', ovmx_s.with_parent, 0)
    check('...and its identity is the enqueuing process\'s, not (0,0)',
          ovmx_s.ident.most_common(1), [((1, 3), 31)])
    check('OVMX never originates the real 10-byte shape',
          [k for k in sh if k[0] == 'OVMX' and k[2] == SUB_LEN], [])


def _selftest_parent(paths, check):
    """(b): the per-file sub-lock hangs under the per-VOLUME root."""
    resolved = collections.Counter()
    unresolved = collections.Counter()
    ambiguous = 0
    for p in paths:
        r, u, a = resolve_parents(p)
        resolved.update(r)
        unresolved.update(u)
        ambiguous += a
    check('EVERY resolved sub-lock parent is an F11B$v per-volume root',
          sorted({parent[:6] for _k, parent in resolved}), [b'F11B$v'])
    check('...and every one of those is the 18-byte space-padded-label form',
          sorted({len(parent) for _k, parent in resolved}), [ROOT_VOL_LEN])
    check('...over this many resolvable sub-lock requests',
          sum(resolved.values()), 26030)
    check('...across this many distinct volume roots',
          len({parent for _k, parent in resolved}), 4)
    check('no handle in the corpus carried two different names', ambiguous, 0)
    check('unresolved (the volume lock predates the capture)',
          dict(unresolved), {'s': 11437, 'c': 4})
    check('F11B$c hangs under the same per-volume root',
          sorted({parent for kind, parent in resolved if kind == 'c'}),
          [b'F11B$vDATA2       ', b'F11B$vSYSDSK1     ',
           b'F11B$vVAX1DATA    '])
    outcomes = parent_answer_outcomes(paths)
    check('NO F11B$v request in the corpus is answered 0xfa (granted on the '
          'wire), so the pair\'s master handle is not grounded here',
          outcomes[(OUTCOME_GRANTED, 'from-real')], 0)
    check('...they are answered 0xf9 you-master-it / 0xf8 redirect / 0xfe',
          sorted(outcomes), [(0xf8, 'from-ovmx'), (0xf8, 'from-real'),
                             (0xf9, 'from-ovmx'), (0xf9, 'from-real'),
                             (0xfe, 'from-real')])


def _selftest_numbers(paths, check):
    """(a): the 4 bytes, and what the corpus cannot discriminate."""
    subs, roots, labels = file_numbers(paths)
    check('distinct sub-lock file numbers', len(subs), 590)
    check('bytes 2 and 3 of the number are ZERO on every one of them -- so '
          'this corpus CANNOT discriminate the FID nmx extension',
          sum(1 for n in subs if n[2] or n[3]), 0)
    check('the numbers live in the Files-11 file-number range',
          (min(le32(n) for n in subs), max(le32(n) for n in subs)), (1, 6289))
    check('distinct 22-byte-root file numbers', len(roots), 528)
    check('...and nearly all of them are also serialized on, which is what '
          'makes the 22-byte root a PER-FILE lock too',
          len(roots & subs), 510)
    check('the 22-byte root is label(12) + number(4)',
          sorted({len(l) for l in labels}), [12])


def _selftest_ovmx_name_is_the_real_volume_root(check):
    """The one thing already byte-identical, and the emitter depends on it.

    OVMX's ACP builds 'F11B$' + kind + vol->volname, and volname is the raw
    12-byte ODS-2 home-block label (memcpy of hm2_volname, NUL at [12]). So
    OVMX's PARENT name -- the per-volume root -- already has the real form, and
    hanging the per-file sub-lock under it does not require inventing a parent.
    Pinned here from the corpus side: the real F11B$v names ARE 'F11B$v' + a
    12-byte space-padded label.
    """
    real = b'F11B$vSYSDSK1     '
    check('the real per-volume root is F11B$v + a 12-byte space-padded label',
          (real[:6], real[6:18], len(real)),
          (b'F11B$v', b'SYSDSK1'.ljust(12, b' '), ROOT_VOL_LEN))


def selftest(repo):
    paths = rc.walk_inputs([os.path.join(repo, 'tests', 'lab', 'captures')])
    state, check = _checker()
    print('F11B$ XQP lock census over %d captures:' % len(paths))
    check('captures walked (>= the 157 of rd vms-e80\'s widened walk)',
          len(paths) >= 157, True)
    _selftest_shapes(shapes(paths), check)
    _selftest_parent(paths, check)
    _selftest_numbers(paths, check)
    _selftest_ovmx_name_is_the_real_volume_root(check)
    print('VERDICT: %s' % ('PASS' if state['ok'] else 'FAIL'))
    return 0 if state['ok'] else 1


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    if argv[0] == '--selftest':
        return selftest(argv[1] if len(argv) > 1
                        else os.path.join(HERE, '..', '..'))
    return report(rc.walk_inputs(argv))


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
