#!/usr/bin/env python3
"""
scs_peer_silence_measure.py -- re-derive the two figures the vms-17f peer listen
timeout is bounded by, from the lab captures.

WHY THIS EXISTS. src/vmsscs/include/scs_depart.h picks a listen timeout: how long
a peer may be silent before OVMX declares it departed, tears its Path Block down
and releases its peer slot. That number decides whether a healthy node gets
thrown out of OVMX's configuration database, so it may not be a guess. The header
states two measurements:

  A. the LONGEST silence ever observed from a HEALTHY peer, and
  B. the silence of a peer that ACTUALLY departed and came back,

and asserts the default sits strictly between them. This script is how A and B
are re-derived. `ctest` cannot run it -- the captures are host-only and not in
git -- so it is run by hand on a lab host:

    tools/scs_peer_silence_measure.py            # re-measure and PASS/FAIL vs EXPECTED
    tools/scs_peer_silence_measure.py --print    # just print what the captures say

The C side is not left un-gated by that: tests/vmsscs/test_scs_config.c asserts
the ORDERING of the three constants (healthy max < default < observed departure),
so a change that moved the default into either measured population reds ctest
even with no captures present.

Everything here reads captured Ethernet frames only -- no VSI/HPE source or
binary is involved (CLAUDE.md rule 8).

METHOD, stated so it can be disagreed with:
  - Only 0x6007 (DEC LAVC/SCA) frames count, which is what SCSD's socket sees.
  - "Silence" is the gap between consecutive frames with the same SOURCE MAC,
    regardless of destination -- multicast beacons included. That matches what
    scsd.c's peer_touch() stamps, and it is the point: a node that is beaconing
    has not departed even if it is saying nothing to us in particular.
  - A capture's leading gap (before a source's first frame) and its trailing gap
    (after its last) are NOT silences: they are the capture window's edges. They
    are reported separately as `head`/`tail` and excluded from the maxima.
  - HEALTHY captures are ones in which no node left the cluster during the
    window. DEPARTURE captures are ones taken across a deliberate node
    drop-and-rejoin. The classification is per capture, listed below, and comes
    from what the run was FOR -- not from the numbers, which would be circular.
"""

import argparse
import os
import struct
import sys

DEFAULT_CAPDIR = "/data/training/vax/cluster/captures"
SCA_ETHERTYPE = b"\x60\x07"

# Captures in which every node stayed up for the whole window. The maximum
# per-source silence over this set is the "healthy" bound.
HEALTHY = (
    "ovmx-760-persist-10min-20260730.pcap",
    "cd0-baseline-current-20260728.pcap",
    "formation-ci1-joinwindow.pcap",
)

# The controlled drop-and-rejoin: VAX2 leaves while VAX1 stays up, then reboots
# and rejoins. docs/cluster-protocol-spec.md sec 4(i) is built on this capture.
DEPARTURE = ("af2-established-rejoin-20260728.pcap",)
DEPARTURE_NODE = "08:00:2b:78:56:b9"  # VAX2, the node that left

# ---------------------------------------------------------------------------
# The recorded measurement (2026-08-04). Every figure appears in
# src/vmsscs/include/scs_depart.h.
# ---------------------------------------------------------------------------
EXPECTED = {
    "healthy_max_ms": 3160,          # 3.153 s, rounded UP to the next 10 ms
    "departure_ms": 395950,          # 395.955 s, rounded DOWN to the next 10 ms
    "healthy_frames": 13392,
    "healthy_span_s": 747,           # sum of the three windows, truncated
}


def frames(path):
    """Yield (timestamp_seconds, frame_bytes) from a classic pcap."""
    with open(path, "rb") as f:
        gh = f.read(24)
        if len(gh) < 24:
            return
        magic = gh[:4]
        if magic == b"\xd4\xc3\xb2\xa1":
            end = "<"
        elif magic == b"\xa1\xb2\xc3\xd4":
            end = ">"
        else:
            raise SystemExit("%s: not a classic pcap (magic %r)" % (path, magic))
        while True:
            rh = f.read(16)
            if len(rh) < 16:
                return
            ts, us, incl, _orig = struct.unpack(end + "IIII", rh)
            data = f.read(incl)
            if len(data) < incl:
                return
            yield ts + us / 1e6, data


def mac(b):
    return ":".join("%02x" % x for x in b)


def silences(path):
    """{src_mac: (max_gap_s, n_frames, head_s, tail_s)} plus the window span."""
    last = {}
    first = {}
    mx = {}
    n = {}
    t0 = t1 = None
    for ts, d in frames(path):
        if len(d) < 14 or d[12:14] != SCA_ETHERTYPE:
            continue
        s = mac(d[6:12])
        if t0 is None:
            t0 = ts
        t1 = ts
        if s in last:
            gap = ts - last[s]
            if gap > mx.get(s, 0.0):
                mx[s] = gap
        else:
            first[s] = ts
            mx.setdefault(s, 0.0)
        last[s] = ts
        n[s] = n.get(s, 0) + 1
    out = {}
    for s in n:
        out[s] = (mx[s], n[s], first[s] - t0, t1 - last[s])
    return out, (0.0 if t0 is None else t1 - t0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--captures", default=DEFAULT_CAPDIR)
    ap.add_argument("--print", dest="just_print", action="store_true")
    args = ap.parse_args()

    healthy_max = 0.0
    healthy_where = ""
    healthy_frames = 0
    healthy_span = 0.0
    missing = []

    print("HEALTHY captures (no node left the cluster during the window):")
    for name in HEALTHY:
        p = os.path.join(args.captures, name)
        if not os.path.exists(p):
            missing.append(name)
            continue
        per, span = silences(p)
        healthy_span += span
        print("  %s  span=%.1fs" % (name, span))
        for s, (gap, cnt, head, tail) in sorted(per.items(), key=lambda kv: -kv[1][1]):
            healthy_frames += cnt
            print("      %s  n=%-6d max_silence=%6.2fs  head=%5.2fs tail=%5.2fs"
                  % (s, cnt, gap, head, tail))
            if gap > healthy_max:
                healthy_max = gap
                healthy_where = "%s / %s" % (name, s)

    departure = 0.0
    departure_where = ""
    print("\nDEPARTURE captures (a node deliberately left and came back):")
    for name in DEPARTURE:
        p = os.path.join(args.captures, name)
        if not os.path.exists(p):
            missing.append(name)
            continue
        per, span = silences(p)
        print("  %s  span=%.1fs" % (name, span))
        for s, (gap, cnt, head, tail) in sorted(per.items(), key=lambda kv: -kv[1][1]):
            tag = "  <-- the node that left" if s == DEPARTURE_NODE else ""
            print("      %s  n=%-6d max_silence=%6.2fs  head=%5.2fs tail=%5.2fs%s"
                  % (s, cnt, gap, head, tail, tag))
            if s == DEPARTURE_NODE and gap > departure:
                departure = gap
                departure_where = "%s / %s" % (name, s)

    if missing:
        print("\nMISSING CAPTURES (host-only, not in git): %s" % ", ".join(missing),
              file=sys.stderr)
        print("Run this on a lab host, or pass --captures DIR.", file=sys.stderr)
        return 2

    healthy_ms = int(healthy_max * 1000 + 9.999) // 10 * 10
    departure_ms = int(departure * 1000) // 10 * 10

    print("\nRESULT")
    print("  longest HEALTHY peer silence : %8.3f s  -> %7d ms   (%s)"
          % (healthy_max, healthy_ms, healthy_where))
    print("  observed DEPARTURE silence   : %8.3f s  -> %7d ms   (%s)"
          % (departure, departure_ms, departure_where))
    print("  healthy population           : %d frames over %.0f s"
          % (healthy_frames, healthy_span))

    if args.just_print:
        return 0

    fails = 0
    for key, got in (("healthy_max_ms", healthy_ms),
                     ("departure_ms", departure_ms),
                     ("healthy_frames", healthy_frames),
                     ("healthy_span_s", int(healthy_span))):
        want = EXPECTED[key]
        ok = got == want
        if not ok:
            fails += 1
        print("  %-16s expected %-8s measured %-8s %s"
              % (key, want, got, "PASS" if ok else "FAIL"))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
