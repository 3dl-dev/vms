#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""segment_relay.py - the 2-node rig's L2 segment, with a PARTITION window
(rd vms-b6d, FC-P8.1).

WHAT IT IS. The rig normally wires its two guests with a QEMU `socket` netdev
straight from B to A. This sits in the middle of that wire instead: it accepts
node B's connection, dials node A's listener, and pumps frames both ways. For a
configured window it DROPS every frame instead of forwarding it -- a segment
that is electrically up and carrying nothing, which is what a partition looks
like to a connection manager.

WHY A DROP AND NOT A DISCONNECT. QEMU's socket netdev never re-dials: tearing
the TCP connection down would end the run, not partition it. Dropping whole
frames leaves both guests' NICs exactly as they were and lets the segment HEAL,
which is the half of this proof that a disconnect could never deliver.

WHY WHOLE FRAMES. The netdev stream is length-prefixed (a 4-byte big-endian
length, then that many bytes of Ethernet frame). Dropping bytes would resync
the peers' parsers into garbage; dropping whole frames is a lossy segment, which
is a thing that really happens.

IT TELLS THE GUESTS NOTHING. There is no channel from this process into either
executive: each node discovers the partition the way a real system does -- its
channel stops hearing HELLOs and times out. Every number the proof quotes is
read out of an executive; the only thing this file contributes to the record is
its own RELAY lines saying when it stopped and resumed forwarding, which are
facts about the WIRE and are labelled as such.

Usage:
  segment_relay.py --listen-port P --connect-port Q --cut-at S --heal-at S
                   [--connect-host H]
"""
import argparse
import socket
import sys
import threading
import time

HDR = 4


def log(msg):
    print("RELAY %s" % msg, flush=True)


class Segment:
    """The cut window, as one clock both directions read."""

    def __init__(self, cut_at, heal_at):
        self.t0 = time.monotonic()
        self.cut_at = cut_at
        self.heal_at = heal_at
        self.announced_cut = False
        self.announced_heal = False
        self.dropped = 0
        self.forwarded = 0
        self.lock = threading.Lock()

    def elapsed(self):
        return time.monotonic() - self.t0

    def carrying(self):
        """Is the segment carrying frames right now? Announces each edge once."""
        t = self.elapsed()
        cut = self.cut_at <= t < self.heal_at
        with self.lock:
            if cut and not self.announced_cut:
                self.announced_cut = True
                log("CUT at t=%.1fs (frames are dropped from here; the guests "
                    "are told nothing)" % t)
            if not cut and self.announced_cut and not self.announced_heal:
                self.announced_heal = True
                log("HEAL at t=%.1fs (forwarded=%d dropped=%d during the cut)"
                    % (t, self.forwarded, self.dropped))
        return not cut

    def count(self, forwarded):
        with self.lock:
            if forwarded:
                self.forwarded += 1
            else:
                self.dropped += 1


def read_exactly(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def pump(src, dst, seg, name):
    """One direction: read a length-prefixed frame, forward it or drop it."""
    try:
        while True:
            hdr = read_exactly(src, HDR)
            if hdr is None:
                break
            n = int.from_bytes(hdr, "big")
            if n <= 0 or n > 65536:
                log("%s: implausible frame length %d -- closing" % (name, n))
                break
            body = read_exactly(src, n)
            if body is None:
                break
            if seg.carrying():
                dst.sendall(hdr + body)
                seg.count(True)
            else:
                seg.count(False)
    except OSError as exc:
        log("%s: %s" % (name, exc))
    finally:
        try:
            dst.shutdown(socket.SHUT_WR)
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen-port", type=int, required=True)
    ap.add_argument("--connect-port", type=int, required=True)
    ap.add_argument("--connect-host", default="127.0.0.1")
    ap.add_argument("--cut-at", type=float, required=True)
    ap.add_argument("--heal-at", type=float, required=True)
    args = ap.parse_args()

    seg = Segment(args.cut_at, args.heal_at)
    log("up: listening on %d, dialing %s:%d; cut at t=%.0fs, heal at t=%.0fs"
        % (args.listen_port, args.connect_host, args.connect_port,
           args.cut_at, args.heal_at))

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.listen_port))
    srv.listen(1)
    b_side, _ = srv.accept()
    log("node B connected")

    a_side = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    for attempt in range(60):
        try:
            a_side.connect((args.connect_host, args.connect_port))
            break
        except OSError:
            if attempt == 59:
                log("could not reach node A's listener")
                return 1
            time.sleep(0.5)
    log("node A connected -- the segment is up")

    for s in (a_side, b_side):
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    t1 = threading.Thread(target=pump, args=(b_side, a_side, seg, "B->A"))
    t2 = threading.Thread(target=pump, args=(a_side, b_side, seg, "A->B"))
    t1.start()
    t2.start()
    t1.join()
    t2.join()
    log("down: forwarded=%d dropped=%d" % (seg.forwarded, seg.dropped))
    return 0


if __name__ == "__main__":
    sys.exit(main())
