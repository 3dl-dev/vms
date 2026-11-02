#!/usr/bin/env python3
"""ci6_evac_grade.py [--share] <capture-dir> -- grade a ci.6 evacuation capture (rd vms-1e58),
or with --share a shared-volume capture (rd vms-5fa, `ci6_evac.sh share`).

The grading half of tests/lab/tools/ci6_evac.sh (`ci6_evac.sh grade`). It is
PURE: it reads only files in the capture directory and prints one PASS/FAIL
line per criterion; the exit status is 1 if any criterion FAILs, 2 if the
capture is unreadable. tests/lab/tests/test_ci6_evac_grade.sh proves it on
fixtures and on the 2026-10-10 run16 hand-run capture.

Capture directory:
  vax1.log, vax2.log      the real VAX consoles
  <OVMX>.console.log      the OVMX member's console, OVMXE by default
                          (CI6_OVMX_NODE); <ovmx>.log is accepted too
  timeline.txt            the fire's timeline (must exist; read for nothing else)
  evac_dat.txt            optional: EVAC.DAT as typed on VAX2. Without it, the
                          largest TYPE/SEARCH dump of EVAC.DAT in vax2.log is used
                          (how the run16 hand run read the seam back).

A console that contains the fire's begin marker (CI6EVAC-BEGIN) is graded
only from its LAST marker on: lab consoles are append-only for the life of the
pod, so an unwindowed grep would find another run's lines.

Criteria:
  1  EVACWL ran on VAX1 first (EVAC.DAT starts with VAX1 records) while the
     OVMX standby held EVAC$WORKLOAD NL, queued behind it.
  2  The workload was stopped on VAX1 and VAX1 then left the cluster:
     STOP, then SHUTDOWN with REMOVE_NODE, SHUTDOWN COMPLETE, and VAX2 saw
     VAX1 removed.
  3  OVMX was granted and continued at seq+1: its "took ... EX at seq S"
     banner has S = last VAX1 seq + 1, EVAC.DAT's next record is OVMX's at S,
     and OVMX kept writing (>= 2 records, none from VAX1 after the seam).
  4  A VAX2 $ENQW EX/NOQUEUE probe (EVPROBE) got SS$_NOTQUEUED (2488,
     %X000009B8) at a time inside OVMX's hold window (between its first and
     last EVAC.DAT record).
  5  Quorum never lost and no bugcheck on any console (grep
     Fatal|bugcheck|CNXMGRERR|LOCKMGRERR|INVLOCKID|quorum lost); VAX2's last
     F$GETSYI read shows CLUSTER_VOTES >= CLUSTER_QUORUM; EVAC.DAT runs
     1..N with no gap and no duplicate.

--share criteria (rd vms-5fa; EVAC.DAT is the largest TYPE of it in vax1.log):
  S1  One volume mounted by both: VAX1 MOUNTED it on $2$DUAn: and its SHOW
      DEVICE/FULL says mounted + "served to cluster via MSCP Server"; OVMX
      MOUNTED the same label on its MSCP unit <VAX1>$DUAn: and its SHOW
      DEVICE/FULL says mounted.
  S2  The Files-11 volume lock reaches one master: VAX1's SDA SHOW RESOURCE
      of F11B$v<label> has Lock Count >= 2 and a SHOW LOCK in the capture of a
      lock on that resource reads "Master copy of lock ... on system <csid>"
      (another member's lock, mastered on VAX1).
  S3  Both nodes wrote EVAC.DAT alternately: >= 4 runs, alternating VAX1 and
      OVMX, each node >= 2 runs.
  S4  Evacuation on the shared volume: "$ STOP <workload>" on VAX1, then OVMX's
      "took EVAC$WORKLOAD EX at seq S" with S = VAX1's last seq + 1, and
      EVAC.DAT ends with >= 2 OVMX records from S.
  S5  ANALYZE/DISK_STRUCTURE on VAX1 is clean: it ran, printed no
      %ANALDISK-W/-E/-F line, and the $STATUS read after it is odd.
  S6  0 bugcheck/quorum lines on any console; VAX2's last Q/V read has V >= Q;
      EVAC.DAT is 1..N with no gap and no duplicate.
"""
import os
import re
import sys
from datetime import datetime

MARKER = 'CI6EVAC-BEGIN'
OVMX = os.environ.get('CI6_OVMX_NODE', 'OVMXE')
VAX1 = os.environ.get('CI6_VAX1_NODE', 'VAX1')
WLPROC = os.environ.get('CI6_VAX1_PROCESS', 'EVACVAX1')
SS_NOTQUEUED = 0x9B8

TIME_RE = re.compile(r'[ \d]\d-[A-Z]{3}-\d{4} \d\d:\d\d:\d\d\.\d\d')
BAD_RE = re.compile(r'(?i:fatal|bug ?check|quorum lost)|CNXMGRERR|LOCKMGRERR|INVLOCKID')


def vtime(s):
    return datetime.strptime(s.strip(), '%d-%b-%Y %H:%M:%S.%f')


def read(path):
    with open(path, 'rb') as f:
        txt = f.read().decode('latin-1').replace('\r', '').replace('\0', '')
    lines = txt.split('\n')
    last = max((i for i, l in enumerate(lines) if MARKER in l), default=None)
    return lines if last is None else lines[last:]


def record(line):
    """One EVAC.DAT record (EVACWL.MAR's fixed 64-byte layout), or None."""
    if len(line) < 59 or not TIME_RE.fullmatch(line[36:59]):
        return None
    node, pid, seq = line[0:16].strip(), line[16:26].strip(), line[26:36].strip()
    if not node or not pid.isdigit() or not seq.isdigit():
        return None
    return (node, int(seq), vtime(line[36:59]))


def dump_from_console(lines):
    """The largest TYPE/SEARCH of EVAC.DAT in a console, as records."""
    blocks, cur = [], None
    for l in lines:
        if re.match(r'^ ?\$ (TYPE|SEARCH)\b.*EVAC\.DAT', l):
            cur = []
            blocks.append(cur)
        elif cur is not None:
            if l.lstrip().startswith('$'):
                cur = None
            else:
                r = record(l)
                if r:
                    cur.append(r)
    return max(blocks, key=len, default=[])


def main():
    args = sys.argv[1:]
    share = bool(args) and args[0] == '--share'
    if share:
        args = args[1:]
    if len(args) != 1:
        print(__doc__.split('\n')[0], file=sys.stderr)
        return 2
    d = args[0]
    con = {}
    for name, cands in (('vax1', ['vax1.log']), ('vax2', ['vax2.log']),
                        ('ovmx', [OVMX + '.console.log', OVMX.lower() + '.log'])):
        p = next((os.path.join(d, c) for c in cands if os.path.isfile(os.path.join(d, c))), None)
        if not p:
            print('capture: FAIL -- %s console log missing (%s)' % (name, ' or '.join(cands)))
            return 2
        con[name] = read(p)
    if not os.path.isfile(os.path.join(d, 'timeline.txt')):
        print('capture: FAIL -- timeline.txt missing')
        return 2
    if share:
        return grade_share(d, con)
    dp = os.path.join(d, 'evac_dat.txt')
    if os.path.isfile(dp):
        recs = [r for r in map(record, read(dp)) if r]
        src = 'evac_dat.txt'
    else:
        recs = dump_from_console(con['vax2'])
        src = 'vax2.log dump'
    print('capture: %s; EVAC.DAT from %s, %d records' % (d, src, len(recs)))

    verdict = {}

    def say(n, ok, why):
        verdict[n] = ok
        print('C%d %s: %s' % (n, 'PASS' if ok else 'FAIL', why))

    vax1_recs = [r for r in recs if r[0] == VAX1]
    ovmx_recs = [r for r in recs if r[0] == OVMX]
    ovtext = '\n'.join(con['ovmx'])

    # 1
    standby = 'EVACWL: standby holds EVAC$WORKLOAD NL' in ovtext
    if not recs:
        say(1, False, 'no EVAC.DAT records in the capture')
    elif recs[0][0] != VAX1:
        say(1, False, 'EVAC.DAT starts with %s seq %d, not %s' % (recs[0][0], recs[0][1], VAX1))
    elif len(vax1_recs) < 2:
        say(1, False, '%s wrote %d record(s): the workload never ran' % (VAX1, len(vax1_recs)))
    elif not standby:
        say(1, False, 'no "standby holds EVAC$WORKLOAD NL" on the %s console' % OVMX)
    else:
        say(1, True, '%s wrote seq %d..%d; %s standby held NL behind it'
            % (VAX1, vax1_recs[0][1], vax1_recs[-1][1], OVMX))

    # 2
    v1 = con['vax1']
    stop = next((i for i, l in enumerate(v1) if re.match(r'^\$ STOP %s\b' % re.escape(WLPROC), l)), None)
    shut = next((i for i, l in enumerate(v1) if re.match(r'^\$ @SYS\$SYSTEM:SHUTDOWN\b', l)), None)
    remove = any(re.search(r'Shutdown options \[[^]]*\]: *.*REMOVE_NODE', l) for l in v1)
    complete = any('SYSTEM SHUTDOWN COMPLETE' in l for l in v1)
    gone = any(re.search(r'removed from VAXcluster system %s\b|Node %s \(csid [0-9A-F]+\) has been removed'
                         % (VAX1, VAX1), l) for l in con['vax2'])
    if stop is None:
        say(2, False, 'no "$ STOP %s" on the %s console' % (WLPROC, VAX1))
    elif shut is None or shut < stop:
        say(2, False, 'no SHUTDOWN on %s after the STOP' % VAX1)
    elif not remove:
        say(2, False, 'SHUTDOWN without the REMOVE_NODE option')
    elif not complete:
        say(2, False, '%s never reached SYSTEM SHUTDOWN COMPLETE' % VAX1)
    elif not gone:
        say(2, False, 'VAX2 never reported %s removed from the cluster' % VAX1)
    else:
        say(2, True, 'STOP %s, then SHUTDOWN REMOVE_NODE; VAX2 removed %s' % (WLPROC, VAX1))

    # 3
    took = re.findall(r'EVACWL: (\S+) took EVAC\$WORKLOAD EX at seq (\d+)', ovtext)
    took = [(n, int(s)) for n, s in took if n == OVMX]
    if not vax1_recs:
        say(3, False, 'no %s records to continue from' % VAX1)
    elif not took:
        say(3, False, 'no "%s took EVAC$WORKLOAD EX" banner: OVMX was never granted' % OVMX)
    else:
        last = vax1_recs[-1]
        at = recs.index(last)
        nxt = recs[at + 1] if at + 1 < len(recs) else None
        after = recs[at + 1:]
        if took[0][1] != last[1] + 1:
            say(3, False, '%s took EX at seq %d, %s last wrote %d' % (OVMX, took[0][1], VAX1, last[1]))
        elif nxt is None or nxt[0] != OVMX or nxt[1] != last[1] + 1:
            say(3, False, 'EVAC.DAT after %s seq %d is %s, not %s seq %d'
                % (VAX1, last[1], ('%s seq %d' % nxt[:2]) if nxt else 'nothing', OVMX, last[1] + 1))
        elif any(r[0] != OVMX for r in after):
            say(3, False, 'a non-%s record follows the seam' % OVMX)
        elif len(after) < 2:
            say(3, False, '%s wrote only %d record after the seam' % (OVMX, len(after)))
        elif nxt[2] <= last[2]:
            say(3, False, '%s seq %d is not later than %s seq %d' % (OVMX, nxt[1], VAX1, last[1]))
        else:
            say(3, True, '%s seq %d (%s) -> %s seq %d (%s) .. %d'
                % (VAX1, last[1], last[2].strftime('%H:%M:%S.%f')[:11], OVMX, nxt[1],
                   nxt[2].strftime('%H:%M:%S.%f')[:11], after[-1][1]))

    # 4
    probes = [re.search(r'EVPROBE: (%s) .*R0=([0-9A-Fa-f]{8})\s*(.*)' % TIME_RE.pattern, l) for l in con['vax2']]
    probes = [p for p in probes if p]
    if not probes:
        say(4, False, 'no EVPROBE line on VAX2: no $ENQ/NOQUEUE probe was run')
    else:
        p = probes[-1]
        st, msg, t = int(p.group(2), 16), p.group(3).strip(), vtime(p.group(1))
        if st != SS_NOTQUEUED and 'NOTQUEUED' not in msg:
            say(4, False, 'probe status %%X%08X (%s), not SS$_NOTQUEUED' % (st, msg or '?'))
        elif not ovmx_recs or not (ovmx_recs[0][2] <= t <= ovmx_recs[-1][2]):
            say(4, False, 'probe at %s is outside %s\'s hold window' % (p.group(1), OVMX))
        else:
            say(4, True, 'VAX2 EX/NOQUEUE at %s -> %%X%08X %s' % (p.group(1)[12:], st, msg))

    # 5
    hits = ['%s: %s' % (n, l.strip()) for n in ('vax1', 'vax2', 'ovmx') for l in con[n] if BAD_RE.search(l)]
    q = [re.search(r'\bQ=(\d+) V=(\d+)', l) for l in con['vax2']]
    q = [m for m in q if m]
    seqs = [r[1] for r in recs]
    seen, dup = set(), []
    for s in seqs:
        if s in seen:
            dup.append(s)
        seen.add(s)
    gaps = [s for s in range(1, max(seqs) + 1) if s not in seen] if seqs else []
    order = [b for a, b in zip(seqs, seqs[1:]) if b != a + 1]
    if hits:
        say(5, False, '%d console line(s) match the bugcheck/quorum grep, first: %s' % (len(hits), hits[0][:120]))
    elif not q:
        say(5, False, 'no CLUSTER_QUORUM/CLUSTER_VOTES read on VAX2')
    elif int(q[-1].group(2)) < int(q[-1].group(1)):
        say(5, False, 'VAX2 last read Q=%s V=%s: votes below quorum' % (q[-1].group(1), q[-1].group(2)))
    elif not seqs:
        say(5, False, 'EVAC.DAT is empty')
    elif dup:
        say(5, False, 'EVAC.DAT duplicate seq %s' % ','.join(map(str, dup[:5])))
    elif gaps:
        say(5, False, 'EVAC.DAT gap: seq %s missing' % ','.join(map(str, gaps[:5])))
    elif order:
        say(5, False, 'EVAC.DAT out of order at seq %d' % order[0])
    else:
        say(5, True, '0 bugcheck/quorum lines; VAX2 Q=%s V=%s; EVAC.DAT seq 1..%d contiguous'
            % (q[-1].group(1), q[-1].group(2), len(seqs)))

    bad = [n for n in sorted(verdict) if not verdict[n]]
    print('VERDICT: %s' % ('PASS' if not bad else 'FAIL (criteria %s)' % ','.join(map(str, bad))))
    return 1 if bad else 0


def block(lines, start_re, stop_re=r'^ ?(\$|SDA>) '):
    """The lines after the LAST line matching start_re, up to the next prompt."""
    at = [i for i, l in enumerate(lines) if re.search(start_re, l)]
    if not at:
        return None
    out = []
    for l in lines[at[-1] + 1:]:
        if re.match(stop_re, l):
            break
        out.append(l)
    return out


def grade_share(d, con):
    v1, v2, ov = con['vax1'], con['vax2'], con['ovmx']
    recs = dump_from_console(v1)
    print('capture: %s; EVAC.DAT from vax1.log dump, %d records' % (d, len(recs)))
    verdict = {}

    def say(n, ok, why):
        verdict[n] = ok
        print('S%d %s: %s' % (n, 'PASS' if ok else 'FAIL', why))

    # S1
    vm = [re.search(r'%MOUNT-I-MOUNTED, (\S+) mounted on _(\$\d+\$DUA\d+:)', l) for l in v1]
    vm = [m for m in vm if m]
    label = vm[-1].group(1) if vm else None
    om = [re.search(r'%%MOUNT-I-MOUNTED, (\S+) mounted on _(%s\$DUA\d+:)' % re.escape(VAX1), l) for l in ov]
    om = [m for m in om if m and m.group(1) == label]
    vfull = block(v1, r'^ ?\$ SHOW DEVICE/FULL \$\d+\$DUA') or []
    vfull = ' '.join(x.strip() for x in vfull)
    ofull = block(ov, r'^ ?\$ SHOW DEVICE/FULL %s\$DUA' % re.escape(VAX1)) or []
    ofull = ' '.join(x.strip() for x in ofull)
    if not vm:
        say(1, False, 'no %%MOUNT-I-MOUNTED on %s' % VAX1)
    elif not om:
        say(1, False, '%s never mounted %s on an MSCP unit of %s' % (OVMX, label, VAX1))
    elif 'mounted' not in vfull or 'served to cluster via MSCP Server' not in vfull:
        say(1, False, '%s SHOW DEVICE/FULL does not show %s mounted and MSCP-served' % (VAX1, vm[-1].group(2)))
    elif 'is online, mounted' not in ofull:
        say(1, False, '%s SHOW DEVICE/FULL does not show %s mounted' % (OVMX, om[-1].group(2)))
    else:
        say(1, True, '%s on %s (MSCP-served) and %s (%s)'
            % (label, vm[-1].group(2), om[-1].group(2), OVMX))

    # S2
    pad = (label or '').ljust(12)
    rsb = block(v1, r'SHOW RESOURCE/NAME="F11B\$v%s"' % re.escape(pad), r'^ ?(\$|SDA>) ')
    rsb_t = '\n'.join(rsb or [])
    cnt = re.search(r'Lock Count:\s+(\d+)', rsb_t)
    master = re.search(r'CSID:\s+([0-9A-F]{8})', rsb_t)
    remote = []
    cur = None
    for l in v1:
        m = re.match(r'Resource:\s+\S+ \S+\s+(\S+)', l)
        if m:
            cur = m.group(1)
        m = re.search(r'Master copy of lock ([0-9A-F]{8}) on system ([0-9A-F]{8})', l)
        if m and cur == 'F11B$v' + (label or '')[:2]:
            remote.append(m.groups())
    if not rsb:
        say(2, False, 'no SDA SHOW RESOURCE of F11B$v%s on %s' % (label, VAX1))
    elif not cnt or int(cnt.group(1)) < 2:
        say(2, False, '%s masters F11B$v%s (CSID %s) with Lock Count %s: no other member\'s lock reached it'
            % (VAX1, label, master.group(1) if master else '?', cnt.group(1) if cnt else '?'))
    elif not remote:
        say(2, False, 'F11B$v%s has %s locks on %s but none is a master copy of another member\'s'
            % (label, cnt.group(1), VAX1))
    else:
        say(2, True, 'F11B$v%s mastered on %s, Lock Count %s, master copy of lock %s on system %s'
            % (label, VAX1, cnt.group(1), remote[0][0], remote[0][1]))

    # S3
    runs = []
    for r in recs:
        if not runs or runs[-1][0] != r[0]:
            runs.append([r[0], 0])
        runs[-1][1] += 1
    shape = ' '.join('%s:%d' % (n, c) for n, c in runs)
    nv = sum(1 for n, _ in runs if n == VAX1)
    no = sum(1 for n, _ in runs if n == OVMX)
    if not recs:
        say(3, False, 'no EVAC.DAT records typed on %s' % VAX1)
    elif any(n not in (VAX1, OVMX) for n, _ in runs):
        say(3, False, 'EVAC.DAT has a writer other than %s/%s: %s' % (VAX1, OVMX, shape))
    elif len(runs) < 4 or nv < 2 or no < 2:
        say(3, False, 'EVAC.DAT runs %s: not alternating (want >= 4 runs, >= 2 per node)' % shape)
    else:
        say(3, True, 'EVAC.DAT runs %s' % shape)

    # S4
    stop = any(re.match(r'^ ?\$ STOP %s\b' % re.escape(WLPROC), l) for l in v1)
    took = [int(s) for n, s in re.findall(r'EVACWL: (\S+) took EVAC\$WORKLOAD EX at seq (\d+)', '\n'.join(ov))
            if n == OVMX]
    tail = runs[-1] if runs else None
    if not stop:
        say(4, False, 'no "$ STOP %s" on %s' % (WLPROC, VAX1))
    elif not took:
        say(4, False, 'no "%s took EVAC$WORKLOAD EX" banner: the workload never evacuated' % OVMX)
    elif not tail or tail[0] != OVMX or tail[1] < 2 or len(runs) < 2:
        say(4, False, 'EVAC.DAT does not end with >= 2 %s records after a %s run' % (OVMX, VAX1))
    else:
        start = recs[-tail[1]][1]
        if took[-1] != start or recs[-tail[1] - 1][0] != VAX1 or start != recs[-tail[1] - 1][1] + 1:
            say(4, False, '%s took EX at seq %d; its final run starts at seq %d' % (OVMX, took[-1], start))
        else:
            say(4, True, '%s seq %d -> %s seq %d..%d' % (VAX1, start - 1, OVMX, start, recs[-1][1]))

    # S5
    an = block(v1, r'^ ?\$ ANALYZE/DISK_STRUCTURE')
    st = [re.search(r'ANALDISK STATUS=%X([0-9A-F]{8})', l) for l in v1]
    st = [m for m in st if m]
    bad = [l for l in (an or []) if re.search(r'%ANALDISK-[WEF]-', l)]
    if an is None:
        say(5, False, 'no ANALYZE/DISK_STRUCTURE on %s' % VAX1)
    elif bad:
        say(5, False, '%d ANALDISK warning/error line(s), first: %s' % (len(bad), bad[0].strip()))
    elif not st or not int(st[-1].group(1), 16) & 1:
        say(5, False, 'ANALYZE/DISK_STRUCTURE $STATUS %s' % (st[-1].group(1) if st else 'not read'))
    else:
        say(5, True, 'no %%ANALDISK-W/-E/-F; $STATUS %%X%s' % st[-1].group(1))

    # S6
    hits = ['%s: %s' % (n, l.strip()) for n in ('vax1', 'vax2', 'ovmx') for l in con[n] if BAD_RE.search(l)]
    q = [m for m in (re.search(r'\bQ=(\d+) V=(\d+)', l) for l in v2) if m]
    seqs = [r[1] for r in recs]
    contiguous = seqs == list(range(1, len(seqs) + 1))
    if hits:
        say(6, False, '%d console line(s) match the bugcheck/quorum grep, first: %s' % (len(hits), hits[0][:120]))
    elif not q or int(q[-1].group(2)) < int(q[-1].group(1)):
        say(6, False, 'VAX2 last Q/V read %s' % (q[-1].group(0) if q else 'missing'))
    elif not seqs or not contiguous:
        say(6, False, 'EVAC.DAT seq is not 1..N contiguous')
    else:
        say(6, True, '0 bugcheck/quorum lines; VAX2 %s; EVAC.DAT seq 1..%d contiguous'
            % (q[-1].group(0), len(seqs)))

    bad = [n for n in sorted(verdict) if not verdict[n]]
    print('VERDICT: %s' % ('PASS' if not bad else 'FAIL (criteria %s)' % ','.join('S%d' % n for n in bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
