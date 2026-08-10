#!/usr/bin/env python3
"""dlm_body.py -- clean-room decoder for the DLM (Distributed Lock Manager)
SYSAP message body carried on the 190-byte VMS$VAXcluster VC (SCA/LAVC,
ethertype 0x6007), category 0x02 / 0x82.

CLEAN-ROOM PROVENANCE (project Rule 8)
--------------------------------------
Every field below is grounded ONLY from:
  (a) our own captures of a real 2-node OpenVMS VAX 7.3 reference cluster
      driving KNOWN single lock operations ($ENQ / convert / $DEQ) at
      controlled resource names and modes, one variable at a time, and
  (b) public OpenVMS documentation + documented tool output:
        - Roy G. Davis, *VAXcluster Principles* (Digital Press, 1993),
          ch. 6 "The VMS Lock Manager", Table 6-1 (p. 6-2) and the lock-mode
          numeric encoding NL=0 CR=1 CW=2 PR=3 PW=4 EX=5 (p. 6-3);
        - SDA `ANALYZE/SYSTEM` `SHOW LOCKS` (the decoder ring: it prints each
          lock's resource name, granted mode, and the process-copy / master-copy
          lock IDs, which we correlate byte-for-byte to the wire).
No VSI/HPE VMS source or binary was disassembled, decompiled, or consulted.

The grounding captures + the exact one-variable byte-diffs that pin each field
are recorded in docs/cluster-protocol-spec.md 4(f). This decoder is validated
against those captures (`--verify`), never against itself.

Body-offset convention (matches 4(j)): body[0] = payload[58] = ABS frame
offset 72 (first byte after the Local Con.ID). The 190-byte class is a 204-byte
frame; the 132-byte SYSAP body spans abs [72..203].

Usage:
    dlm_body.py <pcap> [--all]      decode DLM (cat 02/82) frames in a pcap
    dlm_body.py --verify <dir>      assert the grounded field map against the
                                    ac4-*.pcap captures in <dir>
"""
import struct
import sys
import argparse

ETHERTYPE_SCA = b"\x60\x07"
V1_LOGICAL = b"\xaa\x00\x04\x00\x01\x04"   # VAX1 cluster-logical LAVC addr
V2_LOGICAL = b"\xaa\x00\x04\x00\x02\x04"   # VAX2 cluster-logical LAVC addr

# Public: VAXcluster Principles Table 6-1 (p. 6-2) + numeric encoding (p. 6-3);
# identical to src/libvms/include/lckdef.h LCK$K_*.
LOCK_MODES = {0: "NL", 1: "CR", 2: "CW", 3: "PR", 4: "PW", 5: "EX"}

# cat 0x02 = steady-state DLM (4(j)); opcode within the category (this file):
DLM_OPS = {0x01: "ENQ (new lock request)", 0x07: "CONVERT (mode change)"}


def read_pcap(path):
    d = open(path, "rb").read()
    i, recs = 24, []
    while i + 16 <= len(d):
        _s, _u, cap, _o = struct.unpack("<IIII", d[i:i + 16])
        recs.append(d[i + 16:i + 16 + cap])
        i += 16 + cap
    return recs


def who(mac):
    return {V1_LOGICAL: "VAX1", V2_LOGICAL: "VAX2"}.get(mac, mac.hex())


def is_dlm(f):
    # 190-byte SCS fixed class = 204-byte frame; cat 0x02 (req) / 0x82 (resp)
    return (f[12:14] == ETHERTYPE_SCA and len(f) == 204 and (f[80] & 0x7f) == 0x02)


def decode(f):
    """Return a dict of the grounded DLM-body fields for one 204-byte frame."""
    cat, op = f[80], f[81]
    d = {
        "eth_src": who(f[6:12]), "eth_dst": who(f[0:6]),
        # --- envelope, from 4(j) (body[0..9]) ---
        "send_msg": struct.unpack("<H", f[72:74])[0],
        "ack_msg": struct.unpack("<H", f[74:76])[0],
        "txn": struct.unpack("<H", f[76:78])[0],
        "cksum": struct.unpack("<H", f[78:80])[0],
        "category": cat, "is_response": bool(cat & 0x80),
        "opcode": op, "op_name": DLM_OPS.get(op, "?"),
        # --- DLM body fields grounded by vms-ac4 (body offsets) ---
        # body[20:24] abs[92:96]: request -> requesting PID; response -> the
        #   requester's assigned LOCAL lock id (SDA "Process copy of lock ...").
        "b20_reqid_or_pid": struct.unpack("<I", f[92:96])[0],
        # body[24:28] abs[96:100]: MASTER lock id (SDA "Master copy of lock ...").
        "b24_master_lkid": struct.unpack("<I", f[96:100])[0],
        # body[30] abs[102]: requested lock mode (ENQ + CONVERT requests).
        "b30_lockmode": f[102],
        "b30_lockmode_name": LOCK_MODES.get(f[102], "-"),
        # body[47] abs[119]: resource-name length; body[46] abs[118] const 0x03.
        "b46_const": f[118],
        "b47_resname_len": f[119],
    }
    rl = f[119]
    d["b48_resname"] = f[120:120 + rl] if 1 <= rl <= 31 else b""
    return d


def cmd_decode(path, show_all):
    for idx, f in enumerate(read_pcap(path)):
        if not is_dlm(f):
            continue
        d = decode(f)
        if not show_all and not (d["b48_resname"].startswith(b"OVMX") or
                                 d["opcode"] in DLM_OPS):
            continue
        arrow = "resp" if d["is_response"] else "req "
        print(f"f{idx:<3d} {arrow} cat={d['category']:02x} op={d['opcode']:02x}"
              f" {d['op_name']:<24s} {d['eth_src']}->{d['eth_dst']}"
              f" mode={d['b30_lockmode']:#04x}({d['b30_lockmode_name']})"
              f" reqid/pid={d['b20_reqid_or_pid']:#010x}"
              f" master_lkid={d['b24_master_lkid']:#010x}"
              f" reslen={d['b47_resname_len']}"
              f" res={d['b48_resname'].decode('latin1')!r}")


def cmd_verify(d):
    """Assert the grounded field map against the vms-ac4 captures."""
    import os
    P = lambda t: os.path.join(d, f"ac4-{t}.pcap")

    def req(tag, res=b"OVMXAAAA"):
        for f in read_pcap(P(tag)):
            if is_dlm(f) and f[80] == 0x02 and f[81] == 0x01 and res in f:
                return decode(f)
        return None

    def resp(path, res=None):
        for f in read_pcap(path):
            if is_dlm(f) and f[80] == 0x82 and f[81] == 0x01:
                if res is None or res in f or res == b"":
                    yield decode(f)

    ok = True

    def check(name, cond):
        nonlocal ok
        ok = ok and cond
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}")

    print("Lock-mode byte body[30]/abs102 (one-variable diff, resource OVMXAAAA):")
    for tag, val in [("MCR", 1), ("MCW", 2), ("MPR", 3), ("MPW2", 4), ("MEX", 5)]:
        r = req(tag)
        check(f"{tag}: mode byte == {val} ({LOCK_MODES[val]})",
              r is not None and r["b30_lockmode"] == val)

    print("Resource name body[48]/abs120 (same mode PW, resource diff):")
    a, b = req("MPW2", b"OVMXAAAA"), req("RBBBB", b"OVMXBBBB")
    check("MPW2 res == OVMXAAAA", a and a["b48_resname"] == b"OVMXAAAA")
    check("RBBBB res == OVMXBBBB", b and b["b48_resname"] == b"OVMXBBBB")
    check("mode byte identical (both PW=4) across the resource diff",
          a and b and a["b30_lockmode"] == 4 == b["b30_lockmode"])

    print("Resource length body[47]/abs119 (8-byte vs 12-byte name):")
    ln = None
    for f in read_pcap(P("LEN")):
        if is_dlm(f) and f[80] == 0x02 and f[81] == 0x01 and b"OVMXLONGNAME" in f:
            ln = decode(f)
    check("8-byte OVMXAAAA -> len 0x08", a and a["b47_resname_len"] == 0x08)
    check("12-byte OVMXLONGNAME -> len 0x0c",
          ln and ln["b47_resname_len"] == 0x0c and ln["b48_resname"] == b"OVMXLONGNAME")

    print("Lock-id fields in grant response body[20]/[24] (byte-exact vs SDA):")
    # LKID2: SDA reported VAX1 local 0x310000AB, VAX2 master 0x520006AF
    got = [r for r in resp(P("LKID2")) if r["b20_reqid_or_pid"] == 0x310000AB]
    check("LKID2 grant resp body[20] == VAX1 local lkid 0x310000AB (SDA)",
          any(r["b20_reqid_or_pid"] == 0x310000AB for r in got))
    check("LKID2 grant resp body[24] == VAX2 master lkid 0x520006AF (SDA)",
          any(r["b24_master_lkid"] == 0x520006AF for r in got))
    # MPW: SDA reported VAX1 local 0x0D000380, VAX2 master 0x4A0006AC
    mpw = list(resp(P("MPW")))
    check("MPW grant resp body[20] == VAX1 local lkid 0x0D000380 (SDA)",
          any(r["b20_reqid_or_pid"] == 0x0D000380 for r in mpw))
    check("MPW grant resp body[24] == VAX2 master lkid 0x4A0006AC (SDA)",
          any(r["b24_master_lkid"] == 0x4A0006AC for r in mpw))

    print("CONVERT opcode 0x07 carries the new mode (NL->EX):")
    cvt = None
    for f in read_pcap(P("CVT")):
        if is_dlm(f) and f[80] == 0x02 and f[81] == 0x07:
            cvt = decode(f)
    check("CVT request op==0x07 and mode byte == 5 (EX)",
          cvt and cvt["opcode"] == 0x07 and cvt["b30_lockmode"] == 5)

    print("Completion status = grant/deny discriminator (PW request, one-variable):")
    # grant (OVMXCCCC, NL holder): reply assigns requester lkid at body[20], no name echo
    grant = [r for r in resp(P("LKID2")) if r["b20_reqid_or_pid"] == 0x310000AB]
    # deny  (OVMXDDDD, EX holder, NOQUEUE): reply keeps request placeholder, echoes name
    deny = [r for r in resp(P("DENY")) if r["b48_resname"] == b"OVMXDDDD"]
    check("grant reply assigns a real requester lock-id (!= PID form 0x2020xxxx)",
          any((r["b20_reqid_or_pid"] >> 16) != 0x2020 for r in grant))
    check("deny reply echoes the resource name OVMXDDDD (grant reply does not)",
          any(r["b48_resname"] == b"OVMXDDDD" for r in deny) and
          all(not g["b48_resname"].startswith(b"OVMX") for g in grant))

    print("\n" + ("ALL DLM-BODY GROUNDING CHECKS PASS" if ok else "*** FAILURES ***"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("target", help="pcap to decode, or --verify dir")
    ap.add_argument("--all", action="store_true", help="show every DLM frame")
    ap.add_argument("--verify", action="store_true",
                    help="target is a directory of ac4-*.pcap captures to validate against")
    a = ap.parse_args()
    if a.verify:
        sys.exit(cmd_verify(a.target))
    cmd_decode(a.target, a.all)


if __name__ == "__main__":
    main()
