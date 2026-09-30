#!/usr/bin/env python3
"""echocheck.py <run-dir> -- one rig arm's re-formation, after the stall wakes.

For every OVMXB -> VAX 0x41 frame after the wake: does its abs-36 echo equal the
incarnation the VAX's LATEST directed discovery frame advertised for OVMXB
(abs 92)? A frame stamped with any other number is STALE -- the dead circuit
generation's echo -- and a real OpenVMS VAX V7.3 discards it (rd vms-1f40).

Also reports who STARTed first after the wake: 'peer-first' is the race
h_vc_rx_start() must win on its own (the peer's START reaches the circuit
before this node's channel raised CHANNEL_UP), 'own-first' is CHANNEL_UP's.

Inputs: <run-dir>/fault.out (the injector's "STALL off ... at HH:MM:SS.fff")
and <run-dir>/s8.pcap.gz (the rig's bridge capture, never committed).

CLEAN-ROOM: offsets only from docs/cluster-protocol-spec.md -- abs 30 message
type, abs 36 the sec 4(i).B echo (sec 4(h)(4c)), abs 58 config round, abs 92 the
directed-discovery advertisement (sec 4(i).B).
"""
import datetime
import gzip
import re
import struct
import sys

VAX = bytes.fromhex("08002bfb9186")     # the rig's VAXC
OVMXB = bytes.fromhex("52540000df0b")   # the rig's stalled node
UTC = datetime.timezone.utc


def frames(raw):
    nano = raw[:4] == b"\x4d\x3c\xb2\xa1"
    off = 24
    while off + 16 <= len(raw):
        ts, tu, cl, _ = struct.unpack("<IIII", raw[off:off + 16])
        off += 16
        yield ts + tu / (1e9 if nano else 1e6), raw[off:off + cl]
        off += cl


def wake_epoch(run_dir, first_ts):
    m = re.search(r"STALL off .*? at (\d\d:\d\d:[\d.]+)",
                  open(run_dir + "/fault.out").read(), re.S)
    if not m:
        return None
    day = datetime.datetime.fromtimestamp(first_ts, UTC).strftime("%Y-%m-%d")
    t = datetime.datetime.strptime(day + " " + m.group(1),
                                   "%Y-%m-%d %H:%M:%S.%f")
    return t.replace(tzinfo=UTC).timestamp()


def u16(f, off):
    return struct.unpack("<H", f[off:off + 2])[0]


def main():
    run_dir = sys.argv[1].rstrip("/")
    fr = list(frames(gzip.open(run_dir + "/s8.pcap.gz").read()))
    wake = wake_epoch(run_dir, fr[0][0]) if fr else None
    adv, n, stale, first, shown = None, 0, 0, None, []
    for ts, f in fr:
        if f[12:14] != b"\x60\x07" or len(f) < 40:
            continue
        mt, src, dst = f[30], f[6:12], f[0:6]
        if src == VAX and dst == OVMXB and mt in (0xb2, 0xb3, 0xb4) \
                and len(f) >= 94:
            adv = u16(f, 92)
        if wake is None or ts < wake or mt != 0x41 or len(f) < 60:
            continue
        if first is None and u16(f, 58) == 0 and {src, dst} == {VAX, OVMXB}:
            first = "peer-first" if src == VAX else "own-first"
        if src == OVMXB and dst == VAX:
            n += 1
            if adv is not None and u16(f, 36) != adv:
                stale += 1
            if len(shown) < 4:
                shown.append("%+.3f a36=%d adv=%s rnd=%d"
                             % (ts - wake, u16(f, 36), adv, u16(f, 58)))
    print("%s B->VAX 0x41 after wake: %d stale-echo: %d start: %s | %s"
          % (run_dir.split("/")[-1], n, stale, first, "; ".join(shown)))


if __name__ == "__main__":
    main()
