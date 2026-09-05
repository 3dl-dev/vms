#!/usr/bin/env python3
"""cm_dialogue_audit.py -- what a lab capture says about the VMS$VAXcluster
CM dialogues on it, and about which stations left.

WHY THIS EXISTS (integration note E76). An OVMX lab re-fire looked like a
send-credit regression from the OVMX side alone: eight connection attempts,
each one cdt-closing seconds after a burst, and no COMMIT. The capture said
something else entirely -- both real VAXes emitted the SS4(O.30) departure
marker within 2 ms of an OVMX CM burst and their consoles carried a fatal
CNXMGRERR bugcheck -- and the only thing that distinguished the two fatal
bursts from the two harmless ones earlier in the SAME run was the sec-4(j)
transaction envelope: a dialogue OPENED at send-msg# 8 (and 13), acking a peer
send-msg# on a connection the peer had never sent on.

This script is that comparison, mechanised. It answers three questions off the
wire and nothing else:

  1. WHO WAS ON THE LAN, AND WHEN. First/last frame per source MAC, so a lab
     substrate that changed under the run (a node rebooting into a different
     hardware address, a second instance appearing) is visible instead of being
     read as protocol churn.

  2. WHO ANNOUNCED A DEPARTURE. Every abs-30 == 0xb1 frame with its wall-clock
     time (spec sec 4(O.30)). VMS emits it on a clean leave AND on the way out
     of a cluster bugcheck, so a 0xb1 next to an OVMX transmission is the
     question "did we do that?", not an answer -- correlate with the node's
     console log.

  3. HOW EACH CM DIALOGUE OPENED. For the fixed 190-content class (the only
     class with a grounded Con.ID location, spec sec 4(d)), group frames by the
     (local Con.ID, remote Con.ID) pair -- one SCS connection, one CM dialogue
     -- and print the FIRST envelope each side put on it. Spec sec 4(j) grounds
     send-msg# as "strictly monotonic per sender ... starts at 1 on the first VC
     message" and ack-msg# as an acknowledgement of the peer's highest send-msg#
     seen. So a dialogue whose first frame carries send_msg != 1, or an ack_msg
     above anything the peer has sent on it, is FLAGGED: those are assertions
     about a conversation that did not happen.

CLEAN-ROOM (rule 8): every offset and every label here is either an on-wire
observation or a citation of docs/cluster-protocol-spec.md. Nothing is computed
from an unpublished VMS algorithm; nothing is inferred from VMS sources.

Dependency-free (stdlib only). Classic libpcap files, LE or BE, us or ns.

Usage:
    tools/cluster/cm_dialogue_audit.py <pcap> [<pcap> ...]
"""
import collections
import datetime
import struct
import sys

ETHERTYPE_SCA = b"\x60\x07"

OFF_SCA_LEN = 14        # abs 14, the LE16 "remaining bytes - 2" (sec 4(a))
OFF_MSGTYPE = 30        # abs 30, sec 4(g)
OFF_CONID_REMOTE = 64   # abs 64 == SCA content 50, sec 4(d), 190-class only
OFF_CONID_LOCAL = 68    # abs 68 == SCA content 54, sec 4(d), 190-class only
OFF_CM_BODY = 72        # abs 72 == SCA content 58: the 132-byte SYSAP body

MT_LAST_GASP = 0xb1     # sec 4(O.30), the departure marker
CM_TOTAL = 190          # sec 4(d): the fixed VMS$VAXcluster message class


# --------------------------------------------------------------------------
# pcap
# --------------------------------------------------------------------------
def read_pcap(path):
    """Yield (unix_ts, raw Ethernet frame) from a classic libpcap file."""
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) < 24:
        raise ValueError("%s: too short to be a pcap file" % path)
    magic = data[0:4]
    if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
        endian = "<"
    elif magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
        endian = ">"
    else:
        raise ValueError("%s: unrecognized pcap magic %r" % (path, magic))
    nsec = magic in (b"\x4d\x3c\xb2\xa1", b"\xa1\xb2\x3c\x4d")
    off, end = 24, len(data)
    while off + 16 <= end:
        sec, frac, incl, _orig = struct.unpack(endian + "IIII",
                                               data[off:off + 16])
        off += 16
        yield sec + (frac / 1e9 if nsec else frac / 1e6), data[off:off + incl]
        off += incl


def mac(raw):
    return ":".join("%02x" % b for b in raw)


def le16(frame, off):
    return struct.unpack("<H", frame[off:off + 2])[0]


def le32(frame, off):
    return struct.unpack("<I", frame[off:off + 4])[0]


def clock(ts):
    return datetime.datetime.fromtimestamp(ts, datetime.timezone.utc).strftime(
        "%H:%M:%S.%f")[:-3]


def is_sca(frame):
    return len(frame) > OFF_MSGTYPE and frame[12:14] == ETHERTYPE_SCA


def is_cm(frame):
    """The fixed 190-content class -- the only one whose Con.ID pair and SYSAP
    body offsets sec 4(d) grounds."""
    return (is_sca(frame) and len(frame) >= OFF_CM_BODY + 10 and
            le16(frame, OFF_SCA_LEN) + 2 == CM_TOTAL)


# --------------------------------------------------------------------------
# 1. who was on the LAN
# --------------------------------------------------------------------------
def stations(frames):
    """First and last time each source MAC was seen, and how many frames."""
    seen = collections.OrderedDict()
    for ts, frame in frames:
        if not is_sca(frame):
            continue
        src = mac(frame[6:12])
        rec = seen.setdefault(src, [ts, ts, 0])
        rec[1] = ts
        rec[2] += 1
    return seen


def report_stations(frames):
    print("  -- stations (SCA sources) --")
    for src, (first, last, n) in stations(frames).items():
        print("     %-18s %6d frames  %s .. %s" %
              (src, n, clock(first), clock(last)))


# --------------------------------------------------------------------------
# 2. who announced a departure
# --------------------------------------------------------------------------
def report_departures(frames):
    gasps = [(ts, frame) for ts, frame in frames
             if is_sca(frame) and frame[OFF_MSGTYPE] == MT_LAST_GASP]
    print("  -- departure markers (abs 30 == 0xb1, sec 4(O.30)) --")
    if not gasps:
        print("     none")
        return
    for ts, frame in gasps:
        print("     %s  %s announced departure" % (clock(ts), mac(frame[6:12])))


# --------------------------------------------------------------------------
# 3. how each CM dialogue opened
# --------------------------------------------------------------------------
class Envelope(object):
    """The sec-4(j) transaction envelope at the head of the 132-byte body."""

    __slots__ = ("send_msg", "ack_msg", "txn", "token", "category", "opcode")

    def __init__(self, frame):
        body = OFF_CM_BODY
        self.send_msg = le16(frame, body + 0)
        self.ack_msg = le16(frame, body + 2)
        self.txn = le16(frame, body + 4)
        self.token = le16(frame, body + 6)
        self.category = frame[body + 8]
        self.opcode = frame[body + 9]


def dialogue_key(frame):
    """One SCS connection: the unordered Con.ID pair, so both directions of the
    same dialogue land on one key."""
    a = le32(frame, OFF_CONID_LOCAL)
    b = le32(frame, OFF_CONID_REMOTE)
    return (a, b) if a <= b else (b, a)


def station_pair(frame):
    """The unordered MAC pair the dialogue rides, so a SECOND dialogue between
    the same two stations is recognisable as a re-open."""
    a, b = mac(frame[0:6]), mac(frame[6:12])
    return (a, b) if a <= b else (b, a)


def collect_dialogues(frames):
    """key -> (station pair, ordered list of (ts, src, Envelope))."""
    out = collections.OrderedDict()
    for ts, frame in frames:
        if not is_cm(frame):
            continue
        key = dialogue_key(frame)
        if key not in out:
            out[key] = (station_pair(frame), [])
        out[key][1].append((ts, mac(frame[6:12]), Envelope(frame)))
    return out


def _opened_at_one(ts, src, env):
    """Sec 4(j): "starts at 1 on the first VC message". A first frame carrying
    anything else asserts a conversation the peer never had."""
    if env.send_msg == 1:
        return None
    return ("%s %s OPENED the dialogue at send_msg=%d (sec 4(j): starts at 1 "
            "on the first VC message)" % (clock(ts), src, env.send_msg))


def _ack_is_covered(ts, src, env, high):
    """Sec 4(j): ack-msg# "acknowledges the peer's highest send-msg# seen". An
    ack above anything the peer has put on THIS dialogue is an assertion about
    messages it never sent here."""
    if env.ack_msg <= high:
        return None
    return ("%s %s acked send_msg=%d but the peer has sent %d on this dialogue"
            % (clock(ts), src, env.ack_msg, high))


def audit_dialogue(msgs):
    """Both sec-4(j) checks over one dialogue, in capture order. Returns the
    findings; empty means it opened the way the golden wire opens one."""
    findings = []
    opened = set()
    high = collections.Counter()   # source MAC -> its own highest send_msg here
    for ts, src, env in msgs:
        if src not in opened:
            opened.add(src)
            findings.append(_opened_at_one(ts, src, env))
        peer_high = max([n for who, n in high.items() if who != src] or [0])
        findings.append(_ack_is_covered(ts, src, env, peer_high))
        high[src] = max(high[src], env.send_msg)
    return [f for f in findings if f is not None]


def report_dialogues(frames):
    """
    The sec-4(j) checks are applied ONLY to a RE-OPENED dialogue: a second
    Con.ID pair between two stations this capture has already watched hold one.
    That restriction is the honest half of the measurement -- a capture, like a
    node attaching to a circuit already carrying traffic, cannot know what
    preceded the first frame it sees, so the first dialogue between any pair is
    reported and NOT judged (the E74 control capture opens on a VAX1<->VAX2
    dialogue already at send_msg 14283, which says nothing about either node).
    A RE-open is different: this capture holds the whole life of the second
    dialogue, so "it opened at 8" is a fact about the sender, not the capture.
    """
    print("  -- VMS$VAXcluster CM dialogues (190-content class, sec 4(d)) --")
    dialogues = collect_dialogues(frames)
    if not dialogues:
        print("     none")
        return 0
    flagged = 0
    seen_pairs = set()
    for key, (pair, msgs) in dialogues.items():
        ts, src, env = msgs[0]
        reopen = pair in seen_pairs
        seen_pairs.add(pair)
        print("     con.id pair %08x/%08x  %d messages, opened %s by %s "
              "(cat=%02x op=%02x)%s" %
              (key[0], key[1], len(msgs), clock(ts), src,
               env.category, env.opcode, "  [RE-OPEN]" if reopen else ""))
        if not reopen:
            print("        (first dialogue seen for this station pair -- "
                  "reported, not judged: the capture may have begun mid-stream)")
            continue
        for finding in audit_dialogue(msgs):
            flagged += 1
            print("        FLAG %s" % finding)
    return flagged


def report(path):
    frames = [(ts, fr) for ts, fr in read_pcap(path)]
    print("== %s (%d frames)" % (path, len(frames)))
    report_stations(frames)
    report_departures(frames)
    flagged = report_dialogues(frames)
    print("  -- %d envelope finding(s) --" % flagged)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    for path in argv[1:]:
        report(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
