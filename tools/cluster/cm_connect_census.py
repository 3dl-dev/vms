#!/usr/bin/env python3
"""cm_connect_census.py -- census of the SCA connection-control dialogue
(ethertype 0x6007) in a pcap: who dialled whom, for which SYSAP, and how each
dialogue ended.

CLEAN-ROOM PROVENANCE. Every offset this script reads is one already grounded
and written up in docs/cluster-protocol-spec.md from observation of our own
SIMH OpenVMS VAX reference cluster plus public OpenVMS documentation:

  content[2:8]    destination logical LAVC address      (spec 4d)
  content[10:16]  source logical LAVC address           (spec 4d)
  content[46:48]  SCA connection-control message type   (spec 4h(1a), vms-dd5)
                  0 CONNECT_REQ  1 CONNECT_RSP  2 ACCEPT_REQ  3 ACCEPT_RSP
                  4 REJECT_REQ   5 REJECT_RSP   6 DISCONNECT_REQ
                  7 DISCONNECT_RSP  10 application data
  content[50:54]  remote Con.ID, content[54:58] local Con.ID (spec 4g/4h)
  content[62:78]  SYSAP name on CONNECT_REQ / ACCEPT_REQ   (spec 4h(2))

Nothing is disassembled, decompiled or taken from VSI/HPE material.

Usage:
    cm_connect_census.py <pcap> [--sysap VMS$VAXcluster] [--csv]
                         [--crossings] [--t0 <epoch>]

    --crossings   report only dialogues that CROSS: a pair of CONNECT_REQs for
                  the same SYSAP, in opposite directions between the same two
                  nodes, both outstanding at the same time -- and how each one
                  ended (which side yielded).
"""
import argparse
import struct
import sys

ETHERTYPE_SCA = b"\x60\x07"

CTRL = {0: "CONNECT_REQ", 1: "CONNECT_RSP", 2: "ACCEPT_REQ", 3: "ACCEPT_RSP",
        4: "REJECT_REQ", 5: "REJECT_RSP", 6: "DISCONNECT_REQ",
        7: "DISCONNECT_RSP", 10: "APPDATA"}

# Length classes the spec grounds the connection-control layout for.
CTRL_TOTALS = (110, 66, 62, 58)


def read_pcap(path):
    """Yield (index, epoch_seconds, frame_bytes). Classic pcap only."""
    with open(path, "rb") as fh:
        gh = fh.read(24)
        if len(gh) < 24:
            return
        magic = gh[:4]
        if magic == b"\xd4\xc3\xb2\xa1":
            endian, nano = "<", False
        elif magic == b"\xa1\xb2\xc3\xd4":
            endian, nano = ">", False
        elif magic == b"\x4d\x3c\xb2\xa1":
            endian, nano = "<", True
        elif magic == b"\xa1\xb2\x3c\x4d":
            endian, nano = ">", True
        else:
            raise SystemExit(f"{path}: not a classic pcap (magic {magic.hex()})")
        idx = 0
        while True:
            rh = fh.read(16)
            if len(rh) < 16:
                return
            ts_sec, ts_frac, caplen, _ = struct.unpack(endian + "IIII", rh)
            data = fh.read(caplen)
            if len(data) < caplen:
                return
            ts = ts_sec + ts_frac / (1e9 if nano else 1e6)
            yield idx, ts, data
            idx += 1


def mac(b):
    return ":".join("%02x" % x for x in b)


def sca_payload(frame):
    if len(frame) < 14 or frame[12:14] != ETHERTYPE_SCA:
        return None
    return frame[14:]


def decode_ctrl(pl):
    """Return a dict for a connection-control frame, or None."""
    if len(pl) < 58:
        return None
    total = struct.unpack("<H", pl[0:2])[0] + 2
    if total not in CTRL_TOTALS or len(pl) < min(total, 58):
        return None
    ctype = struct.unpack("<H", pl[46:48])[0]
    if ctype not in CTRL or ctype == 10:
        return None
    remote = struct.unpack("<I", pl[50:54])[0]
    local = struct.unpack("<I", pl[54:58])[0]
    sysap = ""
    if total >= 78 and len(pl) >= 78:
        sysap = pl[62:78].decode("latin-1").strip("\x00 ").strip()
    return {"total": total, "ctype": ctype, "name": CTRL[ctype],
            "remote": remote, "local": local, "sysap": sysap,
            "dst": mac(pl[2:8]), "src": mac(pl[10:16])}


def events(path, sysap_filter=None):
    """Every connection-control event in the pcap, in order.

    The SYSAP name rides only on CONNECT_REQ/ACCEPT_REQ, so a dialogue's SYSAP
    is learned from its CONNECT_REQ and carried forward on the Con.ID pair --
    never guessed for a frame that does not carry it.
    """
    by_conid = {}
    out = []
    for idx, ts, frame in read_pcap(path):
        pl = sca_payload(frame)
        if pl is None:
            continue
        ev = decode_ctrl(pl)
        if ev is None:
            continue
        ev["idx"], ev["ts"] = idx, ts
        key = tuple(sorted((ev["remote"], ev["local"])))
        if ev["sysap"]:
            by_conid[key] = ev["sysap"]
            if ev["ctype"] == 0:
                by_conid[(0, ev["local"])] = ev["sysap"]
        else:
            ev["sysap"] = by_conid.get(key, "")
            if not ev["sysap"]:
                for k in ((0, ev["local"]), (0, ev["remote"])):
                    if k in by_conid:
                        ev["sysap"] = by_conid[k]
                        break
        if sysap_filter and ev["sysap"] != sysap_filter:
            continue
        out.append(ev)
    return out


def find_crossings(evs):
    """A CROSSING is two CONNECT_REQs for the same SYSAP between the same two
    nodes, in opposite directions, with the second sent before the first was
    answered (ACCEPT/REJECT/DISCONNECT for its Con.ID).

    Returns a list of (req_a, req_b, outcome_a, outcome_b).
    """
    reqs = [e for e in evs if e["ctype"] == 0]
    out = []
    for i, a in enumerate(reqs):
        for b in reqs[i + 1:]:
            if b["src"] != a["dst"] or b["dst"] != a["src"]:
                continue
            if a["sysap"] and b["sysap"] and a["sysap"] != b["sysap"]:
                continue
            if outcome_time(evs, a) is not None and b["ts"] > outcome_time(evs, a):
                continue
            out.append((a, b, outcome_of(evs, a), outcome_of(evs, b)))
            break
    return out


def outcome_of(evs, req):
    """The terminating event of the dialogue this CONNECT_REQ opened."""
    for e in evs:
        if e["ts"] <= req["ts"]:
            continue
        if req["local"] not in (e["remote"], e["local"]):
            continue
        if e["ctype"] in (2, 4, 6):
            return e
    return None


def outcome_time(evs, req):
    o = outcome_of(evs, req)
    return o["ts"] if o else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--sysap", default=None)
    ap.add_argument("--crossings", action="store_true")
    ap.add_argument("--t0", type=float, default=None)
    args = ap.parse_args()

    evs = events(args.pcap, args.sysap)
    if not evs:
        print("no connection-control frames matched")
        return
    t0 = args.t0 if args.t0 is not None else evs[0]["ts"]

    if args.crossings:
        xs = find_crossings(evs)
        print(f"{len(xs)} crossing CONNECT_REQ pair(s)")
        for a, b, oa, ob in xs:
            print(f"\n--- crossing: {a['sysap'] or '?'} ---")
            for tag, r, o in (("A", a, oa), ("B", b, ob)):
                print(f"  {tag}  t={r['ts']-t0:8.3f}  {r['src']} -> {r['dst']}"
                      f"  local=0x{r['local']:08x}")
                if o:
                    print(f"      ends {o['name']:<15} t={o['ts']-t0:8.3f}"
                          f"  {o['src']} -> {o['dst']}")
                else:
                    print("      ends (no terminating event in capture)")
            print(f"  gap between the two CONNECT_REQs: {abs(b['ts']-a['ts'])*1000:.1f} ms")
        return

    for e in evs:
        print(f"[{e['idx']:6d}] t={e['ts']-t0:9.3f}  {e['src']} -> {e['dst']}"
              f"  {e['name']:<15} rem=0x{e['remote']:08x} loc=0x{e['local']:08x}"
              f"  len={e['total']}  {e['sysap']}")


if __name__ == "__main__":
    main()
