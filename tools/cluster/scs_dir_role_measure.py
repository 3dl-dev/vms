#!/usr/bin/env python3
"""scs_dir_role_measure.py -- vms-66f.

RE-DERIVES two censuses that docs/cluster-protocol-spec.md sec 4(h)(2a) and the
in-source comments in src/vmsscs/scs_dir.c depend on, and PASS/FAILs each
against a checked-in EXPECTED table:

  CENSUS A -- WHO IS THE ACTIVE HALF.  Which node sends the VMS$VAXcluster
  CONNECT_REQ during cluster formation, and which node sends the SCS$DIRECTORY
  CONNECT_REQ (i.e. which node RUNS a Process Poller).  An earlier revision of
  sec 4(h)(2a) asserted that the JOINER opens its own VMS$VAXcluster connection
  and never polls.  Both halves of that are refuted by this capture; see the
  EXPECTED table.

  CENSUS B -- WHAT [48:50] DOES ON A LOOKUP FRAME.  scs_dir.c once commented
  SCA [48:50] on the lookup REQUEST template as --
  REFUTED-QUOTE-BEGIN
    "flag = 0 (a request; the response has 1)"
  REFUTED-QUOTE-END
  This census is the refutation: it splits every lookup
  message on a SCS$DIRECTORY connection by the GROUNDED [58:62] request/response
  marker and reports the [48:50] histogram of each side.

CLEAN-ROOM PROVENANCE.  Everything below is derived from (a) raw bytes observed
on our own 2-node SIMH OpenVMS VAX 7.3 reference cluster and (b) public
OpenVMS documentation / documented tool output.  No VSI/HPE source or binary was
disassembled, decompiled or consulted.  CLAUDE.md rule 8.

NOT CLASS-RESTRICTED (vms-c11).  vms-c11 found that earlier SCA censuses in this
epic silently keyed on the length set {62, 66, 110} and missed the 58-byte class.
Neither census here filters on SCA length.  CENSUS A scans every SCA frame long
enough to hold the [62:94] name pair, whatever its length; CENSUS B selects on
CONNECTION IDENTITY (the Con.ID pair of an observed SCS$DIRECTORY connection),
never on length, and PRINTS the length histogram of what it selected so a reader
can see the classes were not assumed.

FRAME NUMBERING.  Raw pcap record index, 0-BASED, counting every record in the
file including non-SCA ones -- the convention docs/cluster-protocol-spec.md has
used since sec 4(h).  Wireshark/tcpdump frame numbers are these PLUS ONE.

Usage:
    scs_dir_role_measure.py [pcap]        # default: the golden join window
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from dissect_sca import read_pcap, ETHERTYPE_SCA, mac_str  # noqa: E402

DEFAULT_PCAP = "/data/training/vax/cluster/captures/formation-ci1-joinwindow.pcap"

MAC_NAME = {
    "aa:00:04:00:01:04": "VAX1",
    "aa:00:04:00:02:04": "VAX2",
    "08:00:2b:78:56:b9": "VAX2",   # VAX2's real HW MAC, before DECnet reprograms it
    "ab:00:04:01:01:01": "MCAST",
}

# SCA-content offsets (sca = frame[14:]), the convention of the spec and
# src/vmsscs/scs_dir.c.
OFF_MSGTYPE = 46      # [46:48] LE u16: 0=CONNECT_REQ 1=CONNECT_RSP 2=ACCEPT_REQ
                      #                 3=ACCEPT_RSP 0x0a=application message
OFF_CREDIT = 48       # [48:50] LE u16, the grounded SCA credit field (sec 4d)
OFF_REMOTE = 50       # [50:54] remote Con.ID
OFF_LOCAL = 54        # [54:58] local Con.ID
OFF_MARKER = 58       # [58:62] LE u32: 0 = lookup REQUEST, 1 = lookup RESPONSE
OFF_NAME1 = 62        # [62:78] destination SYSAP name
OFF_NAME2 = 78        # [78:94] source SYSAP name / result

# ---------------------------------------------------------------------------
# EXPECTED -- every figure sec 4(h)(2a) and scs_dir.c are allowed to state.
# Re-measured 2026-08-05 over formation-ci1-joinwindow.pcap (3000 records).
# ---------------------------------------------------------------------------
EXPECTED = {
    # CENSUS A
    "vaxcluster_connect_req_total": 1,
    "vaxcluster_connect_req_frames": [(47, "VAX1", "VAX2")],
    "vaxcluster_connect_req_from_joiner": 0,
    "vaxcluster_accept_req_total": 1,
    "vaxcluster_accept_req_frames": [(50, "VAX2", "VAX1")],
    "directory_connect_req_total": 2,
    "directory_connect_req_frames": [(29, "VAX1", "VAX2"), (1237, "VAX2", "VAX1")],
    # CENSUS B
    "lookup_frames_total": 12,
    "lookup_requests": 6,
    "lookup_responses": 6,
    "request_credit_hist": {0: 2, 1: 4},
    "response_credit_hist": {1: 6},
    "request_credit_zero_frames": [37, 1244],
    "lookup_length_hist": {94: 12},
}


def sca_frames(path):
    for i, (_ts, _us, _ol, fr) in enumerate(read_pcap(path)):
        if len(fr) < 14 or fr[12:14] != ETHERTYPE_SCA:
            continue
        sca = fr[14:]
        if len(sca) < 18:
            continue
        total = struct.unpack("<H", sca[0:2])[0] + 2
        src = MAC_NAME.get(mac_str(fr[6:12]), mac_str(fr[6:12]))
        dst = MAC_NAME.get(mac_str(fr[0:6]), mac_str(fr[0:6]))
        yield i, total, src, dst, sca


def u16(sca, off):
    return struct.unpack("<H", sca[off:off + 2])[0] if off + 2 <= len(sca) else None


def u32(sca, off):
    return struct.unpack("<I", sca[off:off + 4])[0] if off + 4 <= len(sca) else None


def name(sca, off):
    if off + 16 > len(sca):
        return None
    raw = sca[off:off + 16]
    if not all(32 <= b < 127 for b in raw):
        return None
    return raw.decode("ascii").rstrip()


def measure(path):
    """Re-derive every EXPECTED figure from one capture. Split out of main()
    for vms-371 so rederive() below -- and therefore the ctest gate -- reads
    the same packets by the same code."""
    got = {}

    # ---------------- CENSUS A: who opens what ----------------
    # No length filter. Every SCA frame that can hold the [62:94] name pair is
    # examined; selection is on the name pair and the [46:48] message type.
    vc_conn, vc_acc, dir_conn = [], [], []
    for i, total, src, dst, sca in sca_frames(path):
        n1, n2 = name(sca, OFF_NAME1), name(sca, OFF_NAME2)
        if n1 is None or n2 is None:
            continue
        mt = u16(sca, OFF_MSGTYPE)
        pair = (n1, n2)
        if pair == ("VMS$VAXcluster", "VMS$VAXcluster"):
            if mt == 0:
                vc_conn.append((i, src, dst, total))
            elif mt == 2:
                vc_acc.append((i, src, dst, total))
        if pair == ("SCS$DIRECTORY", "SCS$DIR_LOOKUP") and mt == 0:
            dir_conn.append((i, src, dst, total, u32(sca, OFF_LOCAL)))

    got["vaxcluster_connect_req_total"] = len(vc_conn)
    got["vaxcluster_connect_req_frames"] = [(i, s, d) for i, s, d, _ in vc_conn]
    got["vaxcluster_accept_req_total"] = len(vc_acc)
    got["vaxcluster_accept_req_frames"] = [(i, s, d) for i, s, d, _ in vc_acc]
    got["directory_connect_req_total"] = len(dir_conn)
    got["directory_connect_req_frames"] = [(i, s, d) for i, s, d, _, _ in dir_conn]

    # The joiner is the node that does NOT already hold the cluster: in this
    # capture VAX1 is the established member and VAX2 boots into it. Grounded by
    # the SCS$DIRECTORY exchange ordering, not assumed -- VAX1's directory
    # CONNECT_REQ precedes any VAX2-sourced SCS frame.
    got["vaxcluster_connect_req_from_joiner"] = sum(1 for _, s, _, _ in vc_conn if s == "VAX2")

    # ---------------- CENSUS B: [48:50] on lookup frames ----------------
    # Selection is by CONNECTION IDENTITY, never by SCA length. Each observed
    # SCS$DIRECTORY CONNECT_REQ offers a local Con.ID; every later frame whose
    # [50:54]/[54:58] pair contains that Con.ID is on that connection.
    dir_conids = {c for _, _, _, _, c in dir_conn if c}
    req_hist, rsp_hist, len_hist = {}, {}, {}
    req_zero, n_req, n_rsp = [], 0, 0
    for i, total, src, dst, sca in sca_frames(path):
        r, l = u32(sca, OFF_REMOTE), u32(sca, OFF_LOCAL)
        if r not in dir_conids and l not in dir_conids:
            continue
        if u16(sca, OFF_MSGTYPE) != 0x0a:
            continue           # not an application message on this connection
        if name(sca, OFF_NAME1) is None:
            continue           # no SYSAP name field => not a lookup
        marker = u32(sca, OFF_MARKER)
        credit = u16(sca, OFF_CREDIT)
        len_hist[total] = len_hist.get(total, 0) + 1
        if marker == 0:
            n_req += 1
            req_hist[credit] = req_hist.get(credit, 0) + 1
            if credit == 0:
                req_zero.append(i)
        elif marker == 1:
            n_rsp += 1
            rsp_hist[credit] = rsp_hist.get(credit, 0) + 1

    got["lookup_frames_total"] = n_req + n_rsp
    got["lookup_requests"] = n_req
    got["lookup_responses"] = n_rsp
    got["request_credit_hist"] = req_hist
    got["response_credit_hist"] = rsp_hist
    got["request_credit_zero_frames"] = req_zero
    got["lookup_length_hist"] = len_hist
    return got


# Every EXPECTED key here is a packet census; none is a declaration.
# tests/vmsscs/scs_wire.require_coverage() reds if that stops being true.
WIRE_KEYS = tuple(EXPECTED)
NON_WIRE_KEYS = ()

# The single capture this census is taken over. The ctest gate passes it to
# scs_wire.capture_dir() as the `need` set, so a directory without it counts as
# ABSENT and gets the banner instead of a silent skip.
CAPTURE_NAME = os.path.basename(DEFAULT_PCAP)
DEFAULT_CAPDIR = os.path.dirname(DEFAULT_PCAP)


def rederive(capdir, **_kw):
    """THE ctest GATE'S ENTRY POINT (vms-371).

    Returns (results, covered_keys) with `results` as [(ok, label), ...], so
    `ctest -R scs_dir_figures` reds on a lab host when the packets stop
    supporting the sec 4(h)(2a) census -- not only when the prose stops
    matching the (possibly stale) table.
    """
    got = measure(os.path.join(capdir, CAPTURE_NAME))
    return ([(got.get(k) == EXPECTED[k],
              "%s %r != %r" % (k, got.get(k), EXPECTED[k])) for k in EXPECTED],
            set(WIRE_KEYS))


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PCAP
    if not os.path.exists(path):
        print(f"SKIP: {path} not present (captures are host-only, not in git)")
        return 0

    got = measure(path)

    failures = 0
    print(f"{path}\n")
    for k in EXPECTED:
        ok = got.get(k) == EXPECTED[k]
        failures += 0 if ok else 1
        print(f"  [{'PASS' if ok else 'FAIL'}] {k}\n"
              f"          got      {got.get(k)}")
        if not ok:
            print(f"          expected {EXPECTED[k]}")
    print(f"\n{len(EXPECTED)} checks, {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
