#!/usr/bin/env python3
"""scs_disc_measure.py - vms-591: re-derive the SCA DISCONNECT measurements
from the raw lab captures.

Every number quoted in src/vmsscs/include/scs_disc.h, in the shutdown-timeout
justification in src/vmsscs/scsd.c and in docs/cluster-protocol-spec.md sec
4(h)(1a)/4(O)/5 comes out of this script. It reads pcaps only; it imports
nothing from OVMX except the pure-stdlib pcap reader in dissect_sca.py.

    tools/cluster/scs_disc_measure.py           # re-measure and PASS/FAIL vs EXPECTED
    tools/cluster/scs_disc_measure.py --print   # just print what the captures say

Requires the lab captures, host-only and NOT in git (47 files, CLAUDE.md rule
8): /data/training/vax/cluster/captures/*.pcap. Override with --captures.

`ctest -R scs_disc_figures` does NOT need the captures: it asserts every figure
in EXPECTED still appears in the header, in scsd.c and in the spec, so the prose
cannot drift away from the measurement. Only this script, on a host with the
captures, re-derives EXPECTED itself.

----------------------------------------------------------------------------
WHAT IT MEASURES, AND WHY EACH PART EXISTS
----------------------------------------------------------------------------

(A) THE REQUEST/RESPONSE PAIRING -- THE PART THAT CORRECTS THE SPEC.

    docs/cluster-protocol-spec.md sec 4(h)(1a) said, and had said since
    vms-dd5: "`5` and `7` DO NOT EXIST ON OUR WIRE ... not once, in 18,541
    frames ... Do not build a `5` or `7` frame on this section."

    THAT WAS A SAMPLING ERROR. Every census of the [46:48] connection-control
    message type had been restricted to the SCA length classes {62, 66, 110},
    because those are the classes the four FORMATION messages occupy. Both
    RESPONSE messages are SHORTER than all three -- 58 bytes: the envelope, the
    message type and the Con.ID pair, and nothing else.

    This part re-measures with NO length restriction. For every
    connection-control frame it finds the FIRST frame in the reverse direction
    whose Con.ID pair is this frame's pair with the two handles swapped, and
    tabulates (request class, msgtype) -> (response class, msgtype). All four
    SCA request/response pairs come out, in figure order.

(B) THE PER-BYTE CENSUS of the two DISCONNECT frames over the VMS-origin
    population, which is what says which offsets a builder may replay and which
    it must substitute. Reported as constant-vs-varying per payload offset.

(C) THE [60:62] MATCHING FLAG. sec 4(h)(1a) recorded this as "a live field with
    an undecoded meaning", 0x0000 x131 / 0x0001 x89 on DISCONNECT_REQ. This
    part ranks each DISCONNECT_REQ within its Con.ID-pair dialogue and
    cross-tabulates rank against the value. The partition is exact.

(D) THE REQ->RSP LATENCY, which is the whole justification for the 500 ms
    shutdown wait in scsd.c. Reported as min/p50/p90/p99/max plus the count of
    requests that got no answer inside the capture window.

VMS-ORIGIN ONLY, and the rule is the same one scs_reason_measure.py uses: a
real lab VAX sources from 08:00:2b (DEC OUI) or aa:00:04 (DECnet logical). OVMX
sources from a locally-administered Linux tap MAC and is EXCLUDED, because
counting OVMX's own frames as evidence about VMS is circular -- and here it
would be doubly wrong, since a PRE-vms-591 OVMX emitted DISCONNECT frames of
its own (docs/HANDOFF-vms-2f3.md sec 4M.24) that are still in these captures.
The OVMX-sourced population is reported separately, never mixed in.

Everything here reads captured Ethernet frames only -- no VSI/HPE source or
binary is involved (CLAUDE.md rule 8).
"""
import argparse
import collections
import glob
import os
import sys

DEFAULT_CAPDIR = "/data/training/vax/cluster/captures"


def _read_pcap():
    """The pcap reader, imported LAZILY.

    tests/vmsscs/test_scs_disc_figures.py imports EXPECTED out of this file and
    must not need dissect_sca.py -- it reads no capture at all, and its mutation
    harness copies this file alone into a scratch tree. A module-level import
    would make the gate depend on a sibling it never uses.
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from dissect_sca import read_pcap
    return read_pcap

SCA_ETHERTYPE = b"\x60\x07"
# The SCS envelope opcodes a connection-control frame can carry (spec sec 4h).
CONNCTL_OPCODES = (0x4B, 0x5B, 0x7B)
# The four REQUEST classes and the two RESPONSE classes this item adds.
CONNCTL_CLASSES = (58, 62, 66, 110)
MSGTYPE_PAYLOAD_OFF = 46
CONID_PAYLOAD_OFF = 50          # [50:54] remote, [54:58] local
MATCH_PAYLOAD_OFF = 60          # the matching flag (REQUEST only)
FORMAT_PAYLOAD_OFF = 17         # 0x13
MSGTYPE_REQ = 6
MSGTYPE_RSP = 7

MSGTYPE_NAMES = {0: "CONNECT_REQ", 1: "CONNECT_RSP", 2: "ACCEPT_REQ",
                 3: "ACCEPT_RSP", 4: "REJECT_REQ", 5: "REJECT_RSP",
                 6: "DISCONNECT_REQ", 7: "DISCONNECT_RSP", 10: "APPLICATION"}

# ---------------------------------------------------------------------------
# The recorded measurement. Every figure here is quoted in scs_disc.h, in
# scsd.c and in docs/cluster-protocol-spec.md, and
# tests/vmsscs/test_scs_disc_figures.py asserts it is still there.
# Last re-derived 2026-08-05 on workshop against the 47-capture library.
# ---------------------------------------------------------------------------
EXPECTED = {
    "n_captures": 47,

    # (A) request class -> response class. The four SCA pairs, in figure order.
    #     Keyed (req_len, req_msgtype) -> {rsp_len, rsp_msgtype, frames, pcaps}.
    #     Measured over ALL origins, because the pairing is a property of the
    #     dialogue and both ends of every dialogue must be in the sample for a
    #     pairing count to mean anything.
    "pairs": {
        (110, 0): {"rsp": (66, 1), "frames": 1115, "pcaps": 34},
        (110, 2): {"rsp": (62, 3), "frames": 381, "pcaps": 33},
        (62, 4): {"rsp": (58, 5), "frames": 696, "pcaps": 26},
        (62, 6): {"rsp": (58, 7), "frames": 262, "pcaps": 25},
    },

    # (B) the VMS-origin populations of the two DISCONNECT frames.
    "populations": {
        (62, 6): {"name": "DISCONNECT_REQ", "frames": 220, "pcaps": 25},
        (58, 7): {"name": "DISCONNECT_RSP", "frames": 223, "pcaps": 25},
    },

    # (C) the matching flag: rank within the Con.ID-pair dialogue -> value.
    #     ZERO RESIDUALS -- no frame of either rank carries the other value and
    #     no pair carries a third DISCONNECT_REQ.
    "match_flag": {
        (0, 0x0000): {"frames": 131, "pcaps": 25},
        (1, 0x0001): {"frames": 89, "pcaps": 18},
    },
    "match_flag_residuals": 0,

    # (D) REQ -> RSP latency, VMS-origin requests. Seconds, 6 decimal places.
    "latency": {
        "requests": 220,
        "answered": 220,
        "unanswered": 0,
        "min": 0.000006,
        "p50": 0.000286,
        "p90": 0.001041,
        "p99": 0.003459,
        "max": 0.006919,
        "over_10ms": 0,
    },

    # The always-constant payload offsets of each frame over its VMS-origin
    # population -- i.e. exactly what a template may replay. Sorted tuples.
    "const_offsets": {
        (62, 6): (0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 15, 17, 23, 24, 25,
                  28, 29, 32, 33, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
                  48, 49, 51, 55, 58, 59, 61),
        (58, 7): (0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 15, 17, 23, 24, 25,
                  28, 29, 32, 33, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
                  48, 49, 51, 55),
    },
}


def is_vms_origin(frame):
    mac = frame[6:12].hex()
    return mac.startswith("08002b") or mac.startswith("aa000400")


def load_connctl(path, read_pcap=None):
    """Every connection-control-class frame in one pcap, in wire order."""
    if read_pcap is None:
        read_pcap = _read_pcap()
    out = []
    for ts, tu, _o, frame in read_pcap(path):
        if len(frame) < 62 or frame[12:14] != SCA_ETHERTYPE:
            continue
        pl = frame[14:]
        if len(pl) < 58:
            continue
        scalen = (pl[0] | (pl[1] << 8)) + 2
        if scalen not in CONNCTL_CLASSES or len(pl) < scalen:
            continue
        if pl[16] not in CONNCTL_OPCODES or pl[FORMAT_PAYLOAD_OFF] != 0x13:
            continue
        mt = pl[MSGTYPE_PAYLOAD_OFF] | (pl[MSGTYPE_PAYLOAD_OFF + 1] << 8)
        remote = int.from_bytes(pl[CONID_PAYLOAD_OFF:CONID_PAYLOAD_OFF + 4], "little")
        local = int.from_bytes(pl[CONID_PAYLOAD_OFF + 4:CONID_PAYLOAD_OFF + 8], "little")
        out.append({"t": ts + tu / 1e6, "frame": frame, "len": scalen, "mt": mt,
                    "remote": remote, "local": local, "pl": bytes(pl[:scalen]),
                    "vms": is_vms_origin(frame)})
    return out


def measure(capdir):
    read_pcap = _read_pcap()
    files = sorted(glob.glob(os.path.join(capdir, "*.pcap")))
    skipped = []

    pairs = collections.Counter()
    pair_pcaps = collections.defaultdict(set)
    pops = collections.defaultdict(list)
    match = collections.Counter()
    match_pcaps = collections.defaultdict(set)
    match_residual = 0
    lat = []
    lat_unanswered = 0
    lat_requests = 0
    ovmx_pop = collections.Counter()

    for path in files:
        try:
            fr = load_connctl(path, read_pcap)
        except Exception as exc:                       # a truncated file must
            skipped.append((os.path.basename(path), str(exc)))  # not hide the rest
            continue
        base = os.path.basename(path)
        n = len(fr)

        # (A) pairing, and (D) latency for the disconnect pair.
        for i, f in enumerate(fr):
            if f["local"] == 0:
                continue
            dst = f["frame"][0:6]
            rsp = None
            for j in range(i + 1, min(i + 200, n)):
                g = fr[j]
                if g["frame"][6:12] != dst:
                    continue
                if g["remote"] == f["local"] and g["local"] == f["remote"]:
                    rsp = g
                    break
            key = ((f["len"], f["mt"]), (rsp["len"], rsp["mt"]) if rsp else None)
            pairs[key] += 1
            pair_pcaps[key].add(base)
            if f["mt"] == MSGTYPE_REQ and f["len"] == 62 and f["vms"]:
                lat_requests += 1
                if rsp is not None and rsp["mt"] == MSGTYPE_RSP:
                    lat.append(rsp["t"] - f["t"])
                else:
                    lat_unanswered += 1

        # (B) the two populations.
        for f in fr:
            k = (f["len"], f["mt"])
            if k not in ((62, MSGTYPE_REQ), (58, MSGTYPE_RSP)):
                continue
            if f["vms"]:
                pops[k].append((base, f["pl"]))
            else:
                ovmx_pop[k] += 1

        # (C) the matching flag, ranked within its Con.ID-pair dialogue.
        seen = {}
        for f in fr:
            if f["len"] != 62 or f["mt"] != MSGTYPE_REQ or not f["vms"]:
                continue
            key = tuple(sorted((f["remote"], f["local"])))
            rank = seen.get(key, 0)
            seen[key] = rank + 1
            v = f["pl"][MATCH_PAYLOAD_OFF] | (f["pl"][MATCH_PAYLOAD_OFF + 1] << 8)
            if rank > 1 or v not in (0, 1) or (rank == 0) != (v == 0):
                match_residual += 1
            match[(rank, v)] += 1
            match_pcaps[(rank, v)].add(base)

    lat.sort()

    def pct(q):
        return round(lat[min(len(lat) - 1, int(q * len(lat)))], 6) if lat else 0.0

    out = {
        "n_captures": len(files),
        "skipped": skipped,
        "pairs": {},
        "pairs_raw": {k: (pairs[k], len(pair_pcaps[k])) for k in pairs},
        "populations": {},
        "const_offsets": {},
        "match_flag": {k: {"frames": match[k], "pcaps": len(match_pcaps[k])}
                       for k in match},
        "match_flag_residuals": match_residual,
        "ovmx_sourced": dict(ovmx_pop),
        "byte_census": {},
        "latency": {
            "requests": lat_requests,
            "answered": len(lat),
            "unanswered": lat_unanswered,
            "min": round(lat[0], 6) if lat else 0.0,
            "p50": pct(0.5), "p90": pct(0.9), "p99": pct(0.99),
            "max": round(lat[-1], 6) if lat else 0.0,
            "over_10ms": sum(1 for x in lat if x > 0.010),
        },
    }

    # The DOMINANT response for each request class -- the pairing claim.
    byreq = collections.defaultdict(list)
    for (req, rsp), n in pairs.items():
        byreq[req].append((n, rsp, len(pair_pcaps[(req, rsp)])))
    for req, rows in byreq.items():
        rows = [r for r in rows if r[1] is not None]
        if not rows:
            continue
        rows.sort(reverse=True)
        n, rsp, pc = rows[0]
        out["pairs"][req] = {"rsp": rsp, "frames": n, "pcaps": pc}

    for key, rs in pops.items():
        out["populations"][key] = {
            "name": MSGTYPE_NAMES.get(key[1], "?"),
            "frames": len(rs),
            "pcaps": len(set(r[0] for r in rs)),
        }
        consts = []
        census = []
        for off in range(key[0]):
            vals = collections.Counter(r[1][off] for r in rs)
            if len(vals) == 1:
                consts.append(off)
            else:
                census.append((off, len(vals), vals.most_common(4)))
        out["const_offsets"][key] = tuple(consts)
        out["byte_census"][key] = census
    return out


def fmt_key(k):
    return f"({k[0]}, {k[1]})"


def report(m):
    print(f"captures: {m['n_captures']}")
    for base, err in m["skipped"]:
        print(f"  SKIPPED {base}: {err}")
    print("\n(A) request -> response pairing (first reverse frame, mirrored Con.ID pair)")
    for req in sorted(m["pairs"]):
        r = m["pairs"][req]
        print(f"   {req[0]:>3} B mt={req[1]:<2} {MSGTYPE_NAMES.get(req[1],'?'):<15}"
              f" -> {r['rsp'][0]:>3} B mt={r['rsp'][1]:<2}"
              f" {MSGTYPE_NAMES.get(r['rsp'][1],'?'):<15}"
              f" n={r['frames']:<6} pcaps={r['pcaps']}")
    print("\n(B) VMS-origin populations of the two DISCONNECT frames")
    for k in sorted(m["populations"]):
        p = m["populations"][k]
        print(f"   {fmt_key(k)} {p['name']:<15} frames={p['frames']:<5} pcaps={p['pcaps']}")
        print(f"      constant payload offsets ({len(m['const_offsets'][k])}): "
              f"{list(m['const_offsets'][k])}")
    if m["ovmx_sourced"]:
        print(f"   EXCLUDED, OVMX-sourced (reported, never counted): {m['ovmx_sourced']}")
    print("\n(C) the [60:62] matching flag, by rank within the Con.ID-pair dialogue")
    for k in sorted(m["match_flag"]):
        v = m["match_flag"][k]
        print(f"   rank={k[0]} value=0x{k[1]:04x} frames={v['frames']:<5} pcaps={v['pcaps']}")
    print(f"   residuals (any other rank/value combination): {m['match_flag_residuals']}")
    print("\n(D) DISCONNECT_REQ -> DISCONNECT_RSP latency, seconds")
    L = m["latency"]
    print(f"   requests={L['requests']} answered={L['answered']} unanswered={L['unanswered']}")
    print(f"   min={L['min']:.6f} p50={L['p50']:.6f} p90={L['p90']:.6f}"
          f" p99={L['p99']:.6f} max={L['max']:.6f} over-10ms={L['over_10ms']}")


def compare(m):
    fails = 0
    checks = 0

    def ck(cond, what):
        nonlocal fails, checks
        checks += 1
        if not cond:
            fails += 1
            print(f"  FAIL {what}")

    ck(m["n_captures"] == EXPECTED["n_captures"],
       f"n_captures {m['n_captures']} != {EXPECTED['n_captures']}")
    for req, want in EXPECTED["pairs"].items():
        got = m["pairs"].get(req)
        ck(got is not None and tuple(got["rsp"]) == tuple(want["rsp"]),
           f"pair {fmt_key(req)} -> {got['rsp'] if got else None}, expected {want['rsp']}")
        if got:
            ck(got["frames"] == want["frames"],
               f"pair {fmt_key(req)} frames {got['frames']} != {want['frames']}")
            ck(got["pcaps"] == want["pcaps"],
               f"pair {fmt_key(req)} pcaps {got['pcaps']} != {want['pcaps']}")
    for k, want in EXPECTED["populations"].items():
        got = m["populations"].get(k, {})
        ck(got.get("frames") == want["frames"],
           f"population {fmt_key(k)} frames {got.get('frames')} != {want['frames']}")
        ck(got.get("pcaps") == want["pcaps"],
           f"population {fmt_key(k)} pcaps {got.get('pcaps')} != {want['pcaps']}")
    for k, want in EXPECTED["const_offsets"].items():
        ck(tuple(m["const_offsets"].get(k, ())) == tuple(want),
           f"constant offsets for {fmt_key(k)} changed: got "
           f"{list(m['const_offsets'].get(k, ()))}")
    for k, want in EXPECTED["match_flag"].items():
        got = m["match_flag"].get(k, {})
        ck(got.get("frames") == want["frames"],
           f"match flag rank={k[0]} 0x{k[1]:04x} frames {got.get('frames')} != {want['frames']}")
        ck(got.get("pcaps") == want["pcaps"],
           f"match flag rank={k[0]} 0x{k[1]:04x} pcaps {got.get('pcaps')} != {want['pcaps']}")
    ck(set(m["match_flag"]) == set(EXPECTED["match_flag"]),
       f"match flag combinations changed: {sorted(m['match_flag'])}")
    ck(m["match_flag_residuals"] == EXPECTED["match_flag_residuals"],
       f"match flag residuals {m['match_flag_residuals']} != "
       f"{EXPECTED['match_flag_residuals']}")
    for field, want in EXPECTED["latency"].items():
        got = m["latency"][field]
        ck(got == want, f"latency {field} {got} != {want}")

    print(f"{'FAIL' if fails else 'PASS'}: {checks} checks, {fails} failure(s)")
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--captures", default=DEFAULT_CAPDIR)
    ap.add_argument("--print", dest="just_print", action="store_true")
    args = ap.parse_args()

    if not os.path.isdir(args.captures):
        print(f"no capture directory at {args.captures} -- this script needs the "
              f"host-only lab captures (CLAUDE.md rule 8). Use --captures.",
              file=sys.stderr)
        return 2
    m = measure(args.captures)
    report(m)
    if args.just_print:
        return 0
    print()
    return compare(m)


if __name__ == "__main__":
    sys.exit(main())
