#!/usr/bin/env python3
"""scs_reason_measure.py - vms-6b3: re-derive the REJECT/DISCONNECT reason-code
measurement from the raw lab captures.

Every number quoted in src/vmsscs/include/scs_reason.h and in
docs/cluster-protocol-spec.md sec 5 (entry "SCA REJECT/DISCONNECT reason-code
offset") comes out of this script. It reads pcaps only; it imports nothing from
OVMX except the pure-stdlib pcap reader in dissect_sca.py.

WHAT IT DOES. Selects every SCA (0x6007) frame whose total SCA length is 62 --
the connection-control class of spec sec 4(h)(1a) -- and whose message type at
payload [46:48] is 4 (REJECT_REQ) or 6 (DISCONNECT_REQ). Restricts to
VMS-origin source MACs (DEC OUI 08-00-2b, or the LAVC logical aa-00-04-00-xx-04)
so no OVMX-emitted frame can be counted as a VMS observation. Then prints a
per-offset value census of the whole payload, which is what shows that the four
bytes after the Con.ID pair never vary.

USAGE: tools/cluster/scs_reason_measure.py [capture-dir]
       (default capture-dir: /data/training/vax/cluster/captures)
"""
import collections
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dissect_sca import read_pcap  # noqa: E402

SCA_ETHERTYPE = b"\x60\x07"
CONNCTL_SCA_LEN = 62
MSGTYPE_ABS = 60          # payload [46:48] -> absolute 60
REASON_ABS = 72           # payload [58:60] -> absolute 72 (the OVMX placement)
TYPES = {4: "REJECT_REQ", 6: "DISCONNECT_REQ"}


def is_vms_origin(frame):
    mac = frame[6:12].hex()
    return mac.startswith("08002b") or mac.startswith("aa000400")


def main():
    capdir = sys.argv[1] if len(sys.argv) > 1 else "/data/training/vax/cluster/captures"
    rows = collections.defaultdict(list)
    files = sorted(glob.glob(os.path.join(capdir, "*.pcap")))
    if not files:
        print("no pcaps under %s" % capdir)
        return 1
    for path in files:
        try:
            frames = read_pcap(path)
        except Exception as exc:  # a non-pcap or truncated file must not hide the rest
            print("SKIP %s: %s" % (os.path.basename(path), exc))
            continue
        for idx, (_s, _u, _o, frame) in enumerate(frames):
            if len(frame) < 76 or frame[12:14] != SCA_ETHERTYPE:
                continue
            if (frame[14] | (frame[15] << 8)) + 2 != CONNCTL_SCA_LEN:
                continue
            mtype = frame[MSGTYPE_ABS] | (frame[MSGTYPE_ABS + 1] << 8)
            if mtype not in TYPES or not is_vms_origin(frame):
                continue
            rows[mtype].append((os.path.basename(path), idx, bytes(frame[:76])))

    print("capture dir: %s  (%d pcaps)" % (capdir, len(files)))
    for mtype in sorted(TYPES):
        rs = rows.get(mtype, [])
        pcaps = len(set(r[0] for r in rs))
        print("\n=== msgtype %d = %s: %d VMS-origin frames across %d pcaps"
              % (mtype, TYPES[mtype], len(rs), pcaps))
        if not rs:
            continue
        print("  per-offset census, payload bytes that take more than one value:")
        for off in range(14, 76):
            vals = collections.Counter(r[2][off] for r in rs)
            if len(vals) > 1:
                print("    payload[%2d] (abs %2d): %d distinct %s"
                      % (off - 14, off, len(vals), vals.most_common(4)))
        for off, label in ((72, "payload[58:60]  <- THE OVMX REASON-CODE SLOT"),
                           (74, "payload[60:62]  (observed constant on REJECT_REQ)")):
            vals = collections.Counter(r[2][off] | (r[2][off + 1] << 8) for r in rs)
            print("  %s: %s"
                  % (label, ", ".join("0x%04x x%d" % (v, c) for v, c in vals.most_common())))
        nonzero = [r for r in rs if (r[2][72] | (r[2][73] << 8)) != 0]
        print("  VMS frames with a NONZERO value at payload[58:60]: %d" % len(nonzero))
    return 0


if __name__ == "__main__":
    sys.exit(main())
