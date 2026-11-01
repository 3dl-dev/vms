#!/usr/bin/env python3
"""transition_dlm_census.py -- what a node puts on the lock connection WHILE A
CLUSTER STATE TRANSITION IS RUNNING, measured from captures.

WHY THIS EXISTS (rd vms-c55). A VMScluster state transition quiesces the lock
manager: Phase 1 discards all directory information cluster-wide and Phase 2
rebuilds it, and the barrier's steps gate the phases (Davis, *VAXcluster
Principles*, pp. 7-40/7-41, p. 6-33). What a real member actually does on the
wire during that window is a measurable fact, and this tool measures it -- so
"OVMX must not send lock traffic into a rebuild" is a census result, not a
reading of a book.

It answers three questions, one per subcommand:

  census   per (category, opcode): how many cat-0x02/0x82 frames flew INSIDE a
           transition window and how many outside.
  steps    for each non-0x0d frame that flew inside one: which BARRIER STEP was
           in force. This is the sharp one -- real nodes resume lock traffic
           only at the tail of the barrier.
  emitter  per capture: did an OVMX node ORIGINATE a cat-0x02 request inside a
           transition, and at which step. Reads the sibling VAXC.console.log,
           when there is one, for whether the real VAX bugchecked.

THE WINDOW is read only from fields this repo already grounds: the cat-0x01
EPOCH at body[12:16] (VMS_OFF_CM_EPOCH, abs 84) carried by the transition-open
family -- op 0x08/0x09 (open), 0x0a (GO), 0x0b/0x0c (step, release) -- and the
barrier STEP at body[16:20] (VMS_OFF_CM_STEP, abs 88) on op 0x0b/0x0c only. A
window is [first, last] frame of one epoch. No new offset is read, no opcode
semantics are assumed, and nothing is derived from a field this tree has not
already named.

Reads a classic pcap or the cluster demo gate's frames.jsonl.

usage:
  transition_dlm_census.py census  <capture> [...] [--real-only] [--detail]
  transition_dlm_census.py steps   <capture> [...]
  transition_dlm_census.py emitter <capture> [...]
  transition_dlm_census.py --selftest        # re-derive this item's figures
"""
import argparse
import collections
import json
import os
import struct
import sys

ETH_SCA = b'\x60\x07'
FRAME_LEN = 204
BODY_OFF = 72           # VMS_OFF_SYSAP_BODY
OFF_EPOCH = 12          # body-relative; VMS_OFF_CM_EPOCH is abs 84
OFF_STEP = 16           # body-relative; VMS_OFF_CM_STEP is abs 88, op 0b/0c
CM_CAT = 0x01
DLM_CAT = 0x02
RESPONSE_BIT = 0x80
REBUILD_OP = 0x0d       # the directory rebuild record, the one op that belongs
BARRIER_OPS = (0x08, 0x09, 0x0a, 0x0b, 0x0c)
STEP_OPS = (0x0b, 0x0c)
STEP_SANE_MAX = 1000    # an 0x81/0x0b ack reuses the span for something else
REAL_OUIS = (b'\xaa\x00\x04', b'\x08\x00\x2b')

HERE = os.path.dirname(os.path.abspath(__file__))
CAPTURES = os.path.join(HERE, '..', '..', 'tests', 'lab', 'captures')


# --------------------------------------------------------------------------
# Readers
# --------------------------------------------------------------------------
def _pcap_records(path):
    with open(path, 'rb') as f:
        gh = f.read(24)
        if len(gh) < 24:
            return
        magic = gh[0:4]
        if magic in (b'\xd4\xc3\xb2\xa1', b'\x4d\x3c\xb2\xa1'):
            end, nano = '<', magic[0] == 0x4d
        elif magic in (b'\xa1\xb2\xc3\xd4', b'\xa1\xb2\x3c\x4d'):
            end, nano = '>', magic[3] == 0x4d
        else:
            raise SystemExit('%s: not a classic pcap' % path)
        while True:
            h = f.read(16)
            if len(h) < 16:
                return
            ts, tus, caplen, _ = struct.unpack(end + 'IIII', h)
            d = f.read(caplen)
            if len(d) < caplen:
                return
            if len(d) == FRAME_LEN and d[12:14] == ETH_SCA:
                yield ts + tus / (1e9 if nano else 1e6), d, None


def _jsonl_records(path):
    for line in open(path):
        line = line.strip()
        if not line:
            continue
        try:
            r = json.loads(line)
            d = bytes.fromhex(r['hex'])
        except Exception:
            continue
        if len(d) == FRAME_LEN:
            yield r['t'] / 1000.0, d, r.get('port')


def frames(path):
    """[(t, src_mac, label, category, opcode, body)] for the 204-byte class."""
    src = _jsonl_records(path) if path.endswith('.jsonl') else _pcap_records(path)
    out = []
    for t, d, label in src:
        body = d[BODY_OFF:]
        out.append((t, d[6:12], label or d[6:12].hex(':'), body[8], body[9],
                    body))
    return out


def is_ovmx(mac, label):
    """An OVMX node, by the demo's port label when there is one, else by the
    absence of a DEC OUI. Never a guess dressed as a fact: a pcap of real nodes
    only ever yields DEC OUIs."""
    if label and ':' not in label:
        return label.startswith('OVMX')
    return mac[0:3] not in REAL_OUIS


# --------------------------------------------------------------------------
# The transition window
# --------------------------------------------------------------------------
def windows(fs):
    """epoch -> (t_first, t_last) over the transition-open family's frames."""
    span = {}
    for t, _, _, cat, op, body in fs:
        if (cat & ~RESPONSE_BIT) != CM_CAT or op not in BARRIER_OPS:
            continue
        epoch = struct.unpack_from('<I', body, OFF_EPOCH)[0]
        if epoch == 0:
            continue
        lo, hi = span.get(epoch, (t, t))
        span[epoch] = (min(lo, t), max(hi, t))
    return span


def epoch_at(span, t):
    for epoch, (lo, hi) in span.items():
        if lo <= t <= hi:
            return epoch
    return None


def step_tracker(fs):
    """A generator-free walk helper: yields (t, mac, label, cat, op, body,
    epoch_in_force, step_in_force) in time order."""
    span = windows(fs)
    cur = {}
    for t, mac, label, cat, op, body in fs:
        if cat == CM_CAT and op in STEP_OPS:
            e = struct.unpack_from('<I', body, OFF_EPOCH)[0]
            s = struct.unpack_from('<I', body, OFF_STEP)[0]
            if s < STEP_SANE_MAX:
                cur[e] = s
        w = epoch_at(span, t)
        yield t, mac, label, cat, op, body, w, (cur.get(w, 0) if w else 0)


# --------------------------------------------------------------------------
# census
# --------------------------------------------------------------------------
def census_one(path, real_only):
    inw, outw = collections.Counter(), collections.Counter()
    fs = frames(path)
    span = windows(fs)
    for t, mac, label, cat, op, _ in fs:
        if (cat & ~RESPONSE_BIT) != DLM_CAT:
            continue
        if real_only and is_ovmx(mac, label):
            continue
        key = '%02x/%02x' % (cat, op)
        (inw if epoch_at(span, t) is not None else outw)[key] += 1
    return len(fs), len(span), inw, outw


def cmd_census(a):
    tin, tout = collections.Counter(), collections.Counter()
    for path in a.captures:
        n, nw, inw, outw = census_one(path, a.real_only)
        tin.update(inw)
        tout.update(outw)
        if a.detail:
            print('%s: %d frames, %d transition epoch(s)' % (path, n, nw))
            for key in sorted(set(inw) | set(outw)):
                print('    %s  inside %6d   outside %6d'
                      % (key, inw[key], outw[key]))
    print('\n=== %d capture(s)%s ==='
          % (len(a.captures), ', real senders only' if a.real_only else ''))
    print('cat/op   inside a transition   outside')
    for key in sorted(set(tin) | set(tout)):
        print('%s  %19d   %7d' % (key, tin[key], tout[key]))
    return 0


# --------------------------------------------------------------------------
# steps
# --------------------------------------------------------------------------
def steps_one(path):
    """[(sender_class, 'cat/op', step)] for non-0x0d DLM frames in a window."""
    rows = []
    for t, mac, label, cat, op, _, w, step in step_tracker(frames(path)):
        if (cat & ~RESPONSE_BIT) != DLM_CAT or op == REBUILD_OP or w is None:
            continue
        rows.append(('ovmx' if is_ovmx(mac, label) else 'real',
                     '%02x/%02x' % (cat, op), step))
    return rows


def cmd_steps(a):
    hist = collections.Counter()
    for path in a.captures:
        for row in steps_one(path):
            hist[row] += 1
    print('sender  cat/op   barrier step   count')
    for (who, key, step), n in sorted(hist.items()):
        print('%-6s  %s  %12d   %5d' % (who, key, step, n))
    return 0


# --------------------------------------------------------------------------
# emitter
# --------------------------------------------------------------------------
def bugchecked(path):
    c = os.path.join(os.path.dirname(path), 'VAXC.console.log')
    if not os.path.exists(c):
        return '?'
    return 'YES' if 'BUG CHECK' in open(c, errors='replace').read() else 'no'


def emitter_one(path):
    """(in_transition, outside, [detail]) for OVMX-ORIGINATED cat-0x02."""
    inn, out, detail = 0, 0, []
    for t, mac, label, cat, op, _, w, step in step_tracker(frames(path)):
        if cat != DLM_CAT or not is_ovmx(mac, label):
            continue
        if w is None:
            out += 1
        else:
            inn += 1
            detail.append('%02x/%02x@step%d' % (cat, op, step))
    return inn, out, detail


def cmd_emitter(a):
    runs = collections.Counter()
    print('%-44s %-6s %-7s %-24s %s'
          % ('capture', 'in-txn', 'outside', 'detail(in-txn)', 'bugcheck'))
    for path in a.captures:
        inn, out, detail = emitter_one(path)
        bc = bugchecked(path)
        runs[(bc, bool(inn))] += 1
        print('%-44s %-6d %-7d %-24s %s'
              % (os.path.basename(os.path.dirname(path))[-44:], inn, out,
                 ','.join(detail) or '-', bc))
    print('\nruns with an OVMX cat-0x02 request inside a transition: %d of %d'
          % (sum(v for (_, i), v in runs.items() if i), sum(runs.values())))
    for (bc, i), v in sorted(runs.items()):
        print('   bugcheck=%-4s in-transition=%-5s %d run(s)' % (bc, i, v))
    return 0


# --------------------------------------------------------------------------
# selftest -- every figure rd vms-c55 asserts, re-derived from the corpus
# --------------------------------------------------------------------------
def all_pcaps():
    out = []
    for root, _, files in os.walk(CAPTURES):
        for f in files:
            if f.endswith('.pcap'):
                out.append(os.path.join(root, f))
    return sorted(out)


def crash_captures():
    """The three frame-level LOCKMGRERR specimens, and what OVMX originated in
    each. The third carries a whole run, so its removal is the only in-window
    origination among many outside one."""
    return [(os.path.join(CAPTURES, 'vms-c55-lockmgrerr-20261014',
                          'crash-frames-204.jsonl'),
             ['02/04@step1', '02/01@step2']),
            (os.path.join(CAPTURES, 'vms-c55-lockmgrerr-20261015',
                          'crash-frames-204.jsonl'),
             ['02/04@step1', '02/01@step2']),
            (os.path.join(CAPTURES, 'vms-c55-transition-hold-20261016',
                          'crash-frames.jsonl'),
             ['02/04@step1'])]


def _check(ok, what, got=None):
    print('  %s %s%s' % ('ok  ' if ok else 'FAIL', what,
                         '' if got is None else '  (%s)' % (got,)))
    return 0 if ok else 1


def selftest_real_corpus():
    """A real VMS node sends NOTHING but the rebuild record early in a
    transition."""
    bad = 0
    hist = collections.Counter()
    for p in all_pcaps():
        for who, key, step in steps_one(p):
            if who == 'real':
                hist[step] += 1
    early = sum(n for s, n in hist.items() if 1 <= s <= 5)
    tail = sum(n for s, n in hist.items() if s >= 10)
    bad += _check(early == 0,
                  'no real VMS node put a non-0x0d lock message on the wire '
                  'at barrier steps 1-5', early)
    bad += _check(tail > 0,
                  '...while the tail of the barrier (step >= 10) carries them',
                  tail)
    return bad


def selftest_rebuild_window():
    """The one capture that holds a whole 12-step barrier."""
    p = os.path.join(CAPTURES, 'vms-20c-rebuild-1789489597',
                     'rejoin-rebuild.pcap')
    _, _, inw, _ = census_one(p, True)
    bad = _check(set(inw) == {'02/0d', '82/0d'},
                 'inside the 12-step rebuild barrier the ONLY DLM traffic is '
                 'the op-0x0d record and its echo', sorted(inw))
    bad += _check(inw.get('02/0d') == 307 and inw.get('82/0d') == 307,
                  '  307 records, 307 echoes',
                  (inw.get('02/0d'), inw.get('82/0d')))
    return bad


def selftest_crash_captures():
    """OVMX did the opposite, in all three frame-level crash specimens."""
    bad = 0
    for p, want in crash_captures():
        _, _, detail = emitter_one(p)
        bad += _check(detail == want,
                      '%s: OVMX originated %s'
                      % (os.path.basename(os.path.dirname(p)),
                         ' and '.join(want)),
                      detail)
    return bad


def cmd_selftest():
    print('transition_dlm_census selftest (rd vms-c55)')
    bad = selftest_real_corpus()
    bad += selftest_rebuild_window()
    bad += selftest_crash_captures()
    print('%d failure(s)' % bad)
    return 1 if bad else 0


def main(argv):
    if '--selftest' in argv:
        return cmd_selftest()
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    for name in ('census', 'steps', 'emitter'):
        s = sub.add_parser(name)
        s.add_argument('captures', nargs='+')
        if name == 'census':
            s.add_argument('--real-only', action='store_true')
            s.add_argument('--detail', action='store_true')
    a = ap.parse_args(argv)
    return {'census': cmd_census, 'steps': cmd_steps,
            'emitter': cmd_emitter}[a.cmd](a)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
