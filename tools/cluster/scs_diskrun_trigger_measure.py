#!/usr/bin/env python3
"""scs_diskrun_trigger_measure.py - vms-ebb: re-derive the disk-discovery
trigger measurements from the raw lab-2 captures.

Every figure quoted in docs/cluster-protocol-spec.md sec 4(O.4) and in
scsd_diskrun_ungate_tick()'s header in src/vmsscs/scsd.c comes out of this
script. It reads pcaps only and imports nothing from OVMX.

    tools/cluster/scs_diskrun_trigger_measure.py            # PASS/FAIL vs EXPECTED
    tools/cluster/scs_diskrun_trigger_measure.py --print    # just print the capture

Requires the lab captures, host-only and NOT in git (CLAUDE.md rule 8):
/data/training/vax/cluster/captures/vmsebb-*.pcap. Override with --captures.

`ctest -R scs_diskrun_figures` does NOT need the captures: it asserts every
figure in EXPECTED still appears in the spec and in scsd.c, so the ruling's
prose cannot drift away from the measurement. Only this script, on a host with
the captures, re-derives EXPECTED itself.

----------------------------------------------------------------------------
WHAT IT MEASURES, AND WHY EACH PART EXISTS
----------------------------------------------------------------------------

The question vms-096 left open was whether disk discovery should regain an
IMMEDIATE trigger beside the OVMX_DISKRUN_GATE_MS ungate. Answering it needs two
counts off the wire, and BOTH have to come off the wire rather than off SCSD's
log -- the log is the thing under test, and two arms of this very bracket ran a
FOREIGN daemon whose log lines were plausible and wrong (sec 4(O.4)).

(A) DOES THE SIGNAL AN IMMEDIATE TRIGGER WOULD FIRE ON EXIST? That signal is an
    SCA connection-control DISCONNECT_REQ -- message type 6, spec sec 4(h)(1a)
    -- addressed to OVMX's SCS$DIRECTORY *server* Con.ID, whose low 16 bits are
    slot 0x0007. Counting it needs the class filter at [30]: the 0xa0/0xb3/0xb4
    HELLO classes carry no message type there and read as a spurious type 0.

(B) WHEN DOES THE ONE TRIGGER ACTUALLY START THE RUN? That is OVMX's own
    SCS$DIRECTORY CONNECT_REQ on the PS disk-client handle, slot 0x000C. The
    difference between (B) and (A) is what an immediate trigger would save.

Offsets are scsd.c's, absolute from the start of the Ethernet frame:
  [30]    SCA class byte      (0x4b seq-app / 0x5b SCS$DIRECTORY / 0x7b retx)
  [60:62] connection-control message type       (SCA [46:48])
  [64:68] destination ("remote") Con.ID -- OURS when the peer sends
  [68:72] sender's own ("local") Con.ID

Con.IDs are per-boot (ovmx_conid_base()), so everything keys on the low-16 slot
index, never on a whole Con.ID value.
"""
import os
import struct
import sys

CAPTURE_DIR = "/data/training/vax/cluster/captures-lab2"

SLOT_DIR_SERVER = 0x0007   # SCS_DIR_OVMX_CONID
SLOT_PS_DIR = 0x000C       # OVMX_PS_DIR_CONID
SLOT_PS_MSCP = 0x000D      # OVMX_PS_MSCP_CONID

MSG_DISCONNECT_REQ = 6
MSG_DISCONNECT_RSP = 7

SCS_CLASSES = (0x4B, 0x5B, 0x7B)

# ---------------------------------------------------------------------------
# EXPECTED -- the checked-in record of what the vms-ebb bracket measured.
# lab-2 pod vaxlab-1, 2026-08-05, one binary md5 9fc8451f… verified in-pod
# before and after every arm, control BETWEEN the two test arms.
# ---------------------------------------------------------------------------
EXPECTED = {
    "vmsebb-E7-lab2-vaxlab1-20260805.pcap": {
        "identity": "OVMXE7",
        "ungate": True,
        "peer_disconnect_req_to_dir_server": 2,
        "ps_dir_connect_req": 2,
    },
    "vmsebb-E8-control-lab2-vaxlab1-20260805.pcap": {
        "identity": "OVMXE8",
        "ungate": False,                       # OVMX_NO_DISKRUN_UNGATE=1
        "peer_disconnect_req_to_dir_server": 2,
        "ps_dir_connect_req": 0,               # THE CONTROL: none on the wire
    },
    "vmsebb-E9-lab2-vaxlab1-20260805.pcap": {
        "identity": "OVMXE9",
        "ungate": True,
        "peer_disconnect_req_to_dir_server": 2,
        "ps_dir_connect_req": 2,
    },
}

# The gain an immediate trigger would buy, in seconds: PS dir CONNECT_REQ minus
# the peer DISCONNECT_REQ that preceded it, per peer, over the two test arms.
EXPECTED_LEAD_S = [2.175, 2.138, 2.838, 2.172]
EXPECTED_LEAD_RANGE = (2.1, 2.9)
EXPECTED_GATE_MS = 2000


def frames(path):
    with open(path, "rb") as f:
        gh = f.read(24)
        if len(gh) < 24:
            return
        magic = struct.unpack("<I", gh[:4])[0]
        if magic == 0xA1B2C3D4:
            end, nano = "<", False
        elif magic == 0xA1B23C4D:
            end, nano = "<", True
        elif magic == 0xD4C3B2A1:
            end, nano = ">", False
        elif magic == 0x4D3CB2A1:
            end, nano = ">", True
        else:
            raise SystemExit("not a pcap: %s (magic %08x)" % (path, magic))
        while True:
            hdr = f.read(16)
            if len(hdr) < 16:
                return
            _ts, tus, incl, _orig = struct.unpack(end + "IIII", hdr)
            data = f.read(incl)
            if len(data) < incl:
                return
            yield _ts + (tus / 1e9 if nano else tus / 1e6), data


def measure(path):
    rows = list(frames(path))
    if not rows:
        raise SystemExit("empty capture " + path)
    t0 = rows[0][0]

    # OVMX's source MAC is the one that is not a DEC node address. The two VAXes
    # in a lab-2 pod are aa:00:04:… and 08:00:2b:…; OVMX's is the pod tap's.
    ovmx_mac = None
    for _t, d in rows:
        if len(d) >= 12:
            m = ":".join("%02x" % b for b in d[6:12])
            if not (m.startswith("aa:00:04") or m.startswith("08:00:2b")):
                ovmx_mac = m
                break

    idents = set()
    disc_req = []      # (t, peer) -- peer DISCONNECT_REQ to our dir-server slot
    ps_connect = []    # (t, slot) -- OVMX's own PS disk-client CONNECT_REQ
    teardown = []      # the full slot-0x0007 dialogue, in wire order

    for t, d in rows:
        i = d.find(b"OVMX")
        while i != -1:
            s = d[i:i + 6]
            if len(s) == 6 and all(48 <= c <= 90 for c in s):
                idents.add(s.decode())
            i = d.find(b"OVMX", i + 1)
        if len(d) < 72 or d[30] not in SCS_CLASSES:
            continue
        src = ":".join("%02x" % b for b in d[6:12])
        msg = d[60] | (d[61] << 8)
        rcon = int.from_bytes(d[64:68], "little")
        lcon = int.from_bytes(d[68:72], "little")
        dt = t - t0
        from_peer = src != ovmx_mac
        if msg in (MSG_DISCONNECT_REQ, MSG_DISCONNECT_RSP) and \
                (SLOT_DIR_SERVER in ((rcon & 0xFFFF), (lcon & 0xFFFF))):
            teardown.append((dt, "peer->OVMX" if from_peer else "OVMX->peer",
                             msg, lcon, rcon))
        dst = ":".join("%02x" % b for b in d[0:6])
        if from_peer and msg == MSG_DISCONNECT_REQ and \
                (rcon & 0xFFFF) == SLOT_DIR_SERVER:
            disc_req.append((dt, src))
        if not from_peer and msg == 0 and (lcon & 0xFFFF) == SLOT_PS_DIR:
            ps_connect.append((dt, dst))

    # The lead an immediate trigger would buy. PAIRED PER PEER, by MAC: a
    # lab-2 pod has TWO VAXes, each sending its own DISCONNECT_REQ and each
    # getting its own PS dir CONNECT_REQ, and pairing a connect with whichever
    # DISCONNECT_REQ happened to be most recent mixes the two nodes' timelines
    # and understates the lead.
    leads = []
    for t, peer in ps_connect:
        prior = [x for x, p in disc_req if p == peer and x <= t]
        if prior:
            leads.append(round(t - max(prior), 3))

    return {
        "ovmx_mac": ovmx_mac,
        "frames": len(rows),
        "identities": sorted(idents),
        "peer_disconnect_req_to_dir_server": len(disc_req),
        "disconnect_req_times": [round(x, 3) for x, _p in disc_req],
        "ps_dir_connect_req": len(ps_connect),
        "ps_dir_connect_times": [round(x, 3) for x, _s in ps_connect],
        "leads": leads,
        "teardown": teardown,
    }


def main():
    args = sys.argv[1:]
    cap_dir = CAPTURE_DIR
    if "--captures" in args:
        cap_dir = args[args.index("--captures") + 1]
    printing = "--print" in args

    failures = 0
    for name, exp in EXPECTED.items():
        path = os.path.join(cap_dir, name)
        if not os.path.isfile(path):
            print("MISSING  %s" % path)
            failures += 1
            continue
        got = measure(path)
        print("== %s" % name)
        print("   identities %s  frames %d  OVMX MAC %s"
              % (" ".join(got["identities"]), got["frames"], got["ovmx_mac"]))
        print("   peer DISCONNECT_REQ -> dir-server slot 0x%04x: %d at %s"
              % (SLOT_DIR_SERVER, got["peer_disconnect_req_to_dir_server"],
                 got["disconnect_req_times"]))
        print("   OVMX PS dir CONNECT_REQ (slot 0x%04x): %d at %s"
              % (SLOT_PS_DIR, got["ps_dir_connect_req"],
                 got["ps_dir_connect_times"]))
        print("   lead an immediate trigger would buy (s): %s" % got["leads"])
        if printing:
            for dt, who, msg, lcon, rcon in got["teardown"]:
                print("     t+%10.6f  %s  msgtype %d  local 0x%08X remote 0x%08X"
                      % (dt, who, msg, lcon, rcon))
            continue
        for key in ("peer_disconnect_req_to_dir_server", "ps_dir_connect_req"):
            if got[key] != exp[key]:
                print("   FAIL %s: measured %d, EXPECTED %d"
                      % (key, got[key], exp[key]))
                failures += 1
        if exp["identity"] not in got["identities"]:
            print("   FAIL identity: %s not on the wire" % exp["identity"])
            failures += 1
        lo, hi = EXPECTED_LEAD_RANGE
        for lead in got["leads"]:
            if not (lo <= lead <= hi):
                print("   FAIL lead %.3f s outside the recorded %.1f-%.1f s"
                      % (lead, lo, hi))
                failures += 1

    if printing:
        return 0
    print("scs_diskrun_trigger_measure: %d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
