#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""segment_relay.py - the 2-node rig's L2 segment, with a PARTITION window
(rd vms-b6d, FC-P8.1) or a RECONNECT-TOLERANT window (rd vms-4838).

WHAT IT IS. The rig normally wires its two guests with a QEMU `socket` netdev
straight from B to A. This sits in the middle of that wire instead: it accepts
node B's connection, dials node A's listener, and pumps frames both ways. In
the default (qhang) mode, for a configured window it DROPS every frame instead
of forwarding it -- a segment that is electrically up and carrying nothing,
which is what a partition looks like to a connection manager. In
--reconnect-b mode (rd vms-4838, the EVACUATE->REJOIN rig) it never drops
anything, but tolerates node B's OWN QEMU PROCESS being killed and a fresh one
launched with the same SYSGEN identity: node A's single TCP leg to this relay
is dialled ONCE and stays up for the whole run, while the B-facing listener
goes back to accept() every time node B's leg drops.

WHY A DROP AND NOT A DISCONNECT, for the partition case. QEMU's socket netdev
never re-dials: tearing the TCP connection down would end the run, not
partition it. Dropping whole frames leaves both guests' NICs exactly as they
were and lets the segment HEAL, which is the half of that proof a disconnect
could never deliver.

WHY A REAL DISCONNECT IS RIGHT FOR THE REJOIN case, and why it is the OTHER
side of the same limit. rd vms-4838 needs node B to actually reboot -- a fresh
guest, from IDLE, going through its own join FSM's first entry -- not a guest
that stays up through a network blackout (that would be a channel-timeout
scenario, already covered by qhang). Since QEMU's own connecting side never
re-dials, node B's QEMU process is killed and a NEW one launched instead; this
relay is what lets that new TCP leg land on the SAME already-established
connection to node A, which a raw B-directly-to-A `socket,listen=` netdev
cannot do (a `listen=` netdev accepts once, and this rig does not rely on
whatever a given QEMU build does after that).

WHY WHOLE FRAMES. The netdev stream is length-prefixed (a 4-byte big-endian
length, then that many bytes of Ethernet frame). Dropping bytes would resync
the peers' parsers into garbage; dropping whole frames is a lossy segment, which
is a thing that really happens.

IT TELLS THE GUESTS NOTHING. There is no channel from this process into either
executive: each node discovers the partition or the reconnect the way a real
system does -- its channel stops hearing HELLOs and times out, or a fresh
connection lands. Every number the proof quotes is read out of an executive;
the only thing this file contributes to the record is its own RELAY lines
saying when it stopped/resumed forwarding or when node B's leg dropped and
came back, which are facts about the WIRE and are labelled as such.

Usage:
  segment_relay.py --listen-port P --connect-port Q --cut-at S --heal-at S
                   [--connect-host H]
  segment_relay.py --listen-port P --connect-port Q --reconnect-b
                   [--connect-host H] [--reconnect-timeout S]
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
    """The cut window, as one clock both directions read.

    cut_at/heal_at are None in --reconnect-b mode (rd vms-4838): that mode
    never drops a frame, it tolerates node B's TCP leg disappearing and
    reappearing, which is a different mechanism (see pump_round() below).
    """

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
        if self.cut_at is None or self.heal_at is None:
            return True
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


class BSlot:
    """The CURRENT node-B leg, if any (rd vms-4838).

    node A's own connection to this relay is dialled ONCE and is read by ONE
    thread for the WHOLE run (a_reader, below); node B's leg comes and goes
    (evacuation, then a fresh one on rejoin). This is the seam between them:
    a_reader always sends to "whichever B leg is current", and a round ending
    just clears it -- it never has to wait for a_reader to notice, which is
    the deadlock a straight two-threads-per-round design had (measured: with
    node A quiet during the evacuation window, as it is in a synthetic test
    with no live executive redialling it, the relay's own accept() of node
    B's NEXT connection sat blocked behind a join() on the thread reading
    node A, which had nothing to wake it -- the exact same shape a segment
    with a genuinely quiet node A could hit for real).
    """

    def __init__(self):
        self.sock = None
        self.lock = threading.Lock()

    def set(self, sock):
        with self.lock:
            self.sock = sock

    def send(self, data):
        with self.lock:
            sock = self.sock
        if sock is None:
            return False
        try:
            sock.sendall(data)
            return True
        except OSError:
            return False


def a_reader(a_side, bslot, seg, done):
    """Persistent for the WHOLE run (rd vms-4838): reads node A's ONE
    connection once and forwards each frame to whatever node-B leg is
    CURRENTLY live, via `bslot`. A frame with no live B leg to reach is
    DROPPED (counted, never queued) -- exactly what a real segment does
    while node B is powered off; it is not this relay's job to invent a
    buffer VMS's own wire never had. Ends (and sets `done`) only when node
    A's OWN connection ends -- node A's QEMU process is never killed by
    this rig, so that is the real end of the run.
    """
    try:
        while True:
            hdr = read_exactly(a_side, HDR)
            if hdr is None:
                break
            n = int.from_bytes(hdr, "big")
            if n <= 0 or n > 65536:
                log("A->B: implausible frame length %d -- closing" % n)
                break
            body = read_exactly(a_side, n)
            if body is None:
                break
            if seg.carrying() and bslot.send(hdr + body):
                seg.count(True)
            else:
                seg.count(False)
    except OSError as exc:
        log("A->B: %s" % exc)
    done.set()


def b_round(b_side, a_side, seg, round_num):
    """One node-B leg's own lifetime (rd vms-4838): forwards B->A until this
    leg ends, then tears down `b_side` (never `a_side` -- that outlives every
    round). Returns once this leg is done; the CALLER decides what that
    means (an evacuation to wait out, or the whole run ending)."""
    try:
        while True:
            hdr = read_exactly(b_side, HDR)
            if hdr is None:
                break
            n = int.from_bytes(hdr, "big")
            if n <= 0 or n > 65536:
                log("round%d B->A: implausible frame length %d -- closing"
                    % (round_num, n))
                break
            body = read_exactly(b_side, n)
            if body is None:
                break
            if seg.carrying():
                try:
                    a_side.sendall(hdr + body)
                except OSError as exc:
                    log("round%d B->A: %s" % (round_num, exc))
                    break
                seg.count(True)
            else:
                seg.count(False)
    except OSError as exc:
        log("round%d B->A: %s" % (round_num, exc))
    try:
        b_side.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    b_side.close()


def dial_a(connect_host, connect_port):
    a_side = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    for attempt in range(60):
        try:
            a_side.connect((connect_host, connect_port))
            return a_side
        except OSError:
            if attempt == 59:
                return None
            time.sleep(0.5)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen-port", type=int, required=True)
    ap.add_argument("--connect-port", type=int, required=True)
    ap.add_argument("--connect-host", default="127.0.0.1")
    ap.add_argument("--cut-at", type=float, default=None)
    ap.add_argument("--heal-at", type=float, default=None)
    ap.add_argument("--reconnect-b", action="store_true",
                     help="tolerate node B's own QEMU process being killed "
                          "and a fresh one reconnecting (rd vms-4838)")
    ap.add_argument("--reconnect-timeout", type=float, default=90.0,
                     help="seconds to wait for node B's NEXT connection "
                          "before giving up (--reconnect-b only)")
    args = ap.parse_args()

    if (args.cut_at is None) != (args.heal_at is None):
        log("--cut-at and --heal-at must be given together")
        return 2

    seg = Segment(args.cut_at, args.heal_at)
    if args.cut_at is not None:
        log("up: listening on %d, dialing %s:%d; cut at t=%.0fs, heal at "
            "t=%.0fs" % (args.listen_port, args.connect_host,
                         args.connect_port, args.cut_at, args.heal_at))
    else:
        log("up: listening on %d, dialing %s:%d; reconnect-b=%s"
            % (args.listen_port, args.connect_host, args.connect_port,
               args.reconnect_b))

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.listen_port))
    srv.listen(5 if args.reconnect_b else 1)

    log("waiting for node B's first connection")
    b_side, _ = srv.accept()
    log("node B connected")
    b_side.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    a_side = dial_a(args.connect_host, args.connect_port)
    if a_side is None:
        log("could not reach node A's listener")
        return 1
    a_side.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    log("node A connected -- the segment is up")

    if not args.reconnect_b:
        # ORIGINAL BEHAVIOUR (rd vms-b6d qhang mode), UNCHANGED: one B leg,
        # the run ends when either side does.
        t1 = threading.Thread(target=pump, args=(b_side, a_side, seg, "B->A"))
        t2 = threading.Thread(target=pump, args=(a_side, b_side, seg, "A->B"))
        t1.start()
        t2.start()
        t1.join()
        t2.join()
        log("down: forwarded=%d dropped=%d" % (seg.forwarded, seg.dropped))
        return 0

    # RECONNECT-TOLERANT MODE (rd vms-4838): node B's leg may drop
    # (evacuation) and a fresh one may arrive (rejoin) any number of times;
    # node A's single leg persists across every round, read by ONE
    # persistent thread (a_reader) so accept()ing the NEXT node-B leg never
    # waits on node A noticing anything.
    bslot = BSlot()
    bslot.set(b_side)
    a_done = threading.Event()
    a_thread = threading.Thread(target=a_reader, args=(a_side, bslot, seg, a_done),
                                 daemon=True)
    a_thread.start()

    round_num = 1
    while True:
        rt = threading.Thread(target=b_round, args=(b_side, a_side, seg, round_num))
        rt.start()
        # Whichever ends first: this round's own B leg, or node A's
        # connection (the real end of the run). A short poll, not a second
        # blocking join, is what lets this notice a_done promptly without a
        # third synchronisation primitive.
        while rt.is_alive() and not a_done.is_set():
            rt.join(timeout=0.2)
        if a_done.is_set():
            bslot.set(None)
            rt.join()
            log("round %d: node A's own connection ended -- run over"
                % round_num)
            break
        bslot.set(None)
        log("round %d ended (node B's leg closed)" % round_num)
        log("waiting up to %.0fs for node B to reconnect (round %d)"
            % (args.reconnect_timeout, round_num + 1))
        srv.settimeout(args.reconnect_timeout)
        try:
            b_side, _ = srv.accept()
        except socket.timeout:
            log("node B did not reconnect within %.0fs -- ending"
                % args.reconnect_timeout)
            break
        b_side.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        bslot.set(b_side)
        log("node B reconnected (round %d)" % (round_num + 1))
        round_num += 1

    log("down: forwarded=%d dropped=%d" % (seg.forwarded, seg.dropped))
    return 0


if __name__ == "__main__":
    sys.exit(main())
