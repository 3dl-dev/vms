#!/usr/bin/env python3
"""dlm_rms_name_census.py -- WHAT A REAL VAX CALLS AN RMS FILE-ACCESS LOCK
(rd vms-326, root cause of the collision rd vms-acf3 and #1845 both hit).

usage: dlm_rms_name_census.py <pcap|dir> ...     the census
       dlm_rms_name_census.py --selftest <repo>  pin the headline figures

WHY THIS EXISTS. OVMX names RMS's file-access DLM lock after the FID alone
(`RMS$<hexfid>.<seq>.<rvn>`, src/vmsrms/rms_core.c rms_file_lock_resnam). The
DLM is CLUSTER-WIDE, so two different files that happen to share a FID on two
members' disks contend for one lock: node A writing NETACP.LOG at EX refused
node B's read of NETNODE_REMOTE.DAT, and the same collision split OPERATOR.LOG
in a two-node e2e. A real VAX's name carries the VOLUME as well, so it cannot
collide that way.

WHAT THIS MEASURES, and the one thing it adds to an eyeball scan: it classifies
each name by WHO ORIGINATED IT, not by who sent the frame. A real VAX ANSWERS a
request by echoing the requester's name back, so an answer carrying a name is
evidence about the REQUESTER's form, not the answerer's. Split that way:

  * EVERY name a REAL VMS node originates is the 26-byte binary form.
  * EVERY short text-form name in the corpus originates at an OVMX node -- and
    real VAXes have ANSWERED them, which is how OVMX's form reached a real
    cluster's behaviour rather than merely its wire.

THE FORM, per byte, over the real-originated population:

    name[0:4]    'RMS$'
    name[4:10]   the 6-byte Files-11 FID: num(2 LE) seq(2 LE) rvn(1) nmx(1).
                 name[8] and name[9] (rvn, nmx) are 0 on every volume in the
                 corpus -- recorded, not assumed to be constant in general.
    name[10]     0x02 on all but one frame. NOT part of the FID and NOT part of
                 the label. WHAT SELECTS IT IS NOT KNOWN AND IS NOT GUESSED.
    name[11:23]  the VOLUME LABEL, 12 bytes, SPACE-padded.
    name[23:26]  00 00 00.

and the IDENTITY that qualifies it, which is half the routing and is not in the
name at all: body[44:46] group = 0 and body[46] access mode = 1, on every one.

*** AND THE BLOCKER THIS CENSUS EXISTS TO PIN: every real-originated name
    CONTAINS AT LEAST ONE 0x00 BYTE (name[8], name[9] and name[23:26] always
    do). The executive carries DLM resource names as NUL-TERMINATED C strings
    (struct res_key.name, resource_hash_key, the strncmp comparisons in
    vms_lock.c, strscpy into vms_dlm_proxy_post.resnam, and
    `args.resnam[31] = '\\0'` in vms_enq_core_ex), so the real form would be
    TRUNCATED AT name[8] -- eight bytes -- and every file whose (number,
    sequence-low) prefix matched would collapse onto ONE resource. That is the
    same false sharing, reintroduced by the string handling. The name cannot
    land before the engine's resource name is length-counted. ***

CLEAN-ROOM: reads captured frames only. Nothing here is computed from a VMS
algorithm; the per-byte map is what the bytes say.
"""
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import remaster_census as rc  # noqa: E402

BODY_LEN = 132
REAL_OUI = ('08002b', 'aa0004')
PREFIX = b'RMS$'
FORM_LEN = 26

OFF_NAME_LEN = 47
OFF_NAME = 48
OFF_GROUP = 44
OFF_ACMODE = 46


def is_real(mac):
    return mac[:3].hex() in REAL_OUI


def named_frames(paths):
    """Every frame carrying an 'RMS$...' resource name, with its ORIGINATOR.

    A cat-0x82 ANSWER echoes the requester's name, so the name in it is
    evidence about the node the frame is addressed TO. Yielding the originator
    rather than the sender is the whole point of this walk.
    """
    for p in paths:
        for idx, src, dst, body in rc.sca_bodies_with_src(p):
            if len(body) < BODY_LEN:
                continue
            n = body[OFF_NAME_LEN]
            if n == 0 or n > 31:
                continue
            name = bytes(body[OFF_NAME:OFF_NAME + n])
            if not name.startswith(PREFIX):
                continue
            answer = (body[8] & 0x80) != 0
            originator = dst if answer else src
            yield (os.path.basename(p), idx, bytes(originator), answer, name,
                   bytes(body))


def by_originator(paths):
    """(real_originated, ovmx_originated) name->count, over ORIGINATORS."""
    real, ovmx = collections.Counter(), collections.Counter()
    bodies = {}
    for cap, idx, orig, _ans, name, body in named_frames(paths):
        (real if is_real(orig) else ovmx)[name] += 1
        bodies.setdefault((is_real(orig), name), (cap, idx, body))
    return real, ovmx, bodies


def cell_tally(names):
    """Per-byte value counts over a population of equal-length names."""
    out = []
    for o in range(FORM_LEN):
        out.append(collections.Counter(nm[o] for nm in names))
    return out


def report(paths):
    real, ovmx, bodies = by_originator(paths)
    rn = sum(real.values())
    on = sum(ovmx.values())
    print('frames carrying an RMS$ name: %d  (real-originated %d, '
          'OVMX-originated %d)' % (rn + on, rn, on))
    print('distinct names: real %d, OVMX %d' % (len(real), len(ovmx)))
    print()
    print('BY LENGTH, real-originated:')
    for ln, c in sorted(collections.Counter(len(nm) for nm in real).items()):
        print('  %2d bytes  %d distinct' % (ln, c))
    print('BY LENGTH, OVMX-originated:')
    for ln, c in sorted(collections.Counter(len(nm) for nm in ovmx).items()):
        print('  %2d bytes  %d distinct' % (ln, c))
    print()
    pop = [nm for nm in real if len(nm) == FORM_LEN]
    print('the %d-byte form, per byte over %d distinct real names:'
          % (FORM_LEN, len(pop)))
    for o, c in enumerate(cell_tally(pop)):
        if len(c) == 1:
            v = next(iter(c))
            print('  name[%2d] CONSTANT %02x %r' % (o, v, chr(v)))
        else:
            print('  name[%2d] %d distinct: %s' % (o, len(c), ' '.join(
                '%02x:%d' % kv for kv in c.most_common(4))))
    print()
    print('the 12-byte field at name[11:23]:')
    for lbl, c in collections.Counter(nm[11:23] for nm in pop).most_common(8):
        print('  %r  %d' % (lbl.decode('latin1'), c))
    print()
    print('containing at least one 0x00: %d of %d'
          % (sum(1 for nm in pop if b'\0' in nm), len(pop)))
    print('would truncate as a C string at byte: %s'
          % sorted({nm.index(b'\0') for nm in pop if b'\0' in nm}))
    print()
    ident = collections.Counter()
    for (isreal, name), (_c, _i, body) in bodies.items():
        if isreal and len(name) == FORM_LEN:
            ident[(int.from_bytes(body[OFF_GROUP:OFF_GROUP + 2], 'little'),
                   body[OFF_ACMODE])] += 1
    print('the identity (body[44:46] group, body[46] access mode):')
    for k, v in ident.most_common(6):
        print('  group=%d mode=%d  %d' % (k[0], k[1], v))


def selftest(repo):
    paths = rc.walk_inputs([os.path.join(repo, 'tests', 'lab', 'captures')])
    ok = True

    def check(name, got, want):
        nonlocal ok
        good = got == want
        ok = ok and good
        print('  [%s] %s: %r%s' % ('PASS' if good else 'FAIL', name, got,
                                   '' if good else ' (want %r)' % (want,)))

    real, ovmx, bodies = by_originator(paths)
    pop = [nm for nm in real if len(nm) == FORM_LEN]

    # 1. WHO USES WHICH FORM. This is the figure the item turns on: the binary
    #    form is what a real node ORIGINATES, and the text form is OVMX's own.
    check('distinct real-ORIGINATED RMS$ names', len(real), 64)
    check('*** ...all of them the 26-byte binary form ***', len(pop), 64)
    check('frames carrying a real-originated RMS$ name',
          sum(real.values()), 1888)
    # rd vms-083's arm capture added a FIFTH OVMX-originated text name --
    # that node's own RMS lock during its boot. The figure is re-derived
    # with the corpus, exactly as the denominators above are.
    check('distinct OVMX-ORIGINATED RMS$ names', len(ovmx), 5)
    check('*** ...none of them the 26-byte form ***',
          sum(1 for nm in ovmx if len(nm) == FORM_LEN), 0)
    check('...their lengths', sorted({len(nm) for nm in ovmx}), [9, 10, 12])

    # 2. THE FORM, per cell, over the real-originated population.
    cells = cell_tally(pop)
    check('name[0:4] is RMS$ on every one',
          all(len(cells[o]) == 1 for o in range(4)), True)
    check('name[8] (the FID\'s rvn) is 0 on every volume here',
          list(cells[8]), [0])
    check('name[9] (the FID\'s nmx) likewise', list(cells[9]), [0])
    check('name[10] distinct values', sorted(cells[10]), [0, 2])
    check('*** name[10] == 0x02 ***', cells[10][2], 63)
    check('name[19:23] are all SPACE',
          all(list(cells[o]) == [0x20] for o in range(19, 23)), True)
    check('name[23:26] are all NUL',
          all(list(cells[o]) == [0] for o in range(23, 26)), True)
    check('distinct 12-byte fields at name[11:23]',
          sorted(lbl.decode('latin1') for lbl in {nm[11:23] for nm in pop}),
          ['\x00YSDSK1     ', 'SYSDSK1     ', 'VAX1DATA    ', 'VAXCSYS     '])

    # 3. *** THE BLOCKER. *** Every one of them has an embedded NUL, and a C
    #    string truncates it at byte 8 -- which is the same false sharing the
    #    volume in the name exists to prevent.
    check('*** real names containing at least one 0x00 ***',
          sum(1 for nm in pop if b'\0' in nm), len(pop))
    # AND IT IS WORSE THAN name[8]. A FID byte can itself be 0 -- a file
    # number whose low byte is zero truncates the name to 'RMS$' ALONE, so
    # every such file on every volume in the cluster shares ONE lock.
    check('*** the bytes a C string would truncate them at ***',
          sorted({nm.index(b'\0') for nm in pop if b'\0' in nm}), [4, 7, 8])
    check('*** ...and how many truncate to \'RMS$\' alone (4 bytes) ***',
          sum(1 for nm in pop if nm.index(b'\0') == 4), 1)
    check('...to 7 bytes', sum(1 for nm in pop if nm.index(b'\0') == 7), 37)
    check('...to 8 bytes', sum(1 for nm in pop if nm.index(b'\0') == 8), 26)

    # 4. THE IDENTITY, which is not in the name and routes it.
    ident = {(int.from_bytes(b[OFF_GROUP:OFF_GROUP + 2], 'little'),
              b[OFF_ACMODE])
             for (isreal, nm), (_c, _i, b) in bodies.items()
             if isreal and len(nm) == FORM_LEN}
    check('(group, access mode) on every real-originated 26-byte name',
          sorted(ident), [(0, 1)])

    print('\ndlm_rms_name_census selftest: %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main(argv):
    if len(argv) >= 2 and argv[1] == '--selftest':
        repo = argv[2] if len(argv) > 2 else os.path.join(HERE, '..', '..')
        return selftest(repo)
    if len(argv) < 2:
        print(__doc__)
        return 2
    report(rc.walk_inputs(argv[1:]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
