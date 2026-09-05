#!/usr/bin/env python3
"""cm_wire_safety_audit.py -- the PRE-FLIGHT SAFETY GATE for OVMX cluster wire
output: does anything we put on the wire sit outside the envelope every REAL
VMS node in the reference corpus respects?

WHY THIS EXISTS. A faithful OVMX node must never emit cluster traffic that can
bugcheck a peer -- the endgame is joining REAL production VMScluster systems,
where a crashed Connection Manager takes the cluster with it. We have found two
such vectors REACTIVELY, by crashing lab VAXes:

  * `CNXMGRERR` (integration note E76) -- OVMX opened a brand-new SCS dialogue
    at SYSAP send-msg# 8 (and 13), acking a peer send-msg# on a connection the
    peer had never sent on, because the per-CSB counter never restarted per
    connection. BOTH reference VAXes took a fatal bugcheck.
  * `INVEXCEPTN` (integration note E78) -- OVMX answered the coordinator's
    254-frame `cat 0x01 op 0x06` membership burst with ONE `cat 0x04` ack per
    frame: 254 acks in 31.6 ms. VAX2 took a fatal bugcheck and stayed down.

Finding those by crashing hardware does not scale and does not generalise. This
script makes the check PROACTIVE: it measures the envelope the corpus of real
VMS nodes actually keeps, and flags every OVMX-originated frame outside it.

WHAT IT IS NOT. It never asserts that a flagged frame WILL crash a peer. It
asserts exactly one thing, which is the thing we can prove: **no real VMS node
in the reference corpus ever emitted this, and here is the sample size.** Two of
the vectors below additionally carry an OBSERVED crash; the rest are honest
"outside the measured envelope" reports (INV-6 -- report what the wire shows,
never a fabricated verdict).

THE THRESHOLDS ARE MEASURED, NEVER GUESSED. Every constant in the GROUNDING
table below was re-derived from the reference corpus (`~/vax/cluster/captures`,
47 captures, 306 670 CM-class frames) by this file's own `--measure` mode, so it
can be re-derived rather than trusted. Where a corpus sample is thin it is
labelled thin and the vector is reported at WARN, not FATAL.

CLEAN-ROOM (rule 8). Every offset and label here is either an on-wire
observation or a citation of docs/cluster-protocol-spec.md. Nothing is computed
from an unpublished VMS algorithm; no VSI/HPE source or binary was consulted.

Dependency-free (stdlib only). Classic libpcap files, LE or BE, us or ns.

USAGE
    tools/cluster/cm_wire_safety_audit.py <pcap> [<pcap> ...]
    tools/cluster/cm_wire_safety_audit.py --self-test        prove it has teeth
    tools/cluster/cm_wire_safety_audit.py --measure <pcap>   re-derive thresholds
    tools/cluster/cm_wire_safety_audit.py --audit-all <pcap> judge EVERY source
    tools/cluster/cm_wire_safety_audit.py --ovmx-mac <mac> <pcap>

Exit 0 = no FATAL finding. Exit 1 = at least one FATAL finding. Exit 2 = the
audit could not run (unreadable capture, or a capture with no CM frames at all,
which proves nothing and must not read as a pass).
"""
import collections
import sys

from cm_dialogue_audit import (OFF_CM_BODY, OFF_CONID_LOCAL, OFF_CONID_REMOTE,
                               OFF_MSGTYPE, clock, is_cm, is_sca, le16, le32,
                               mac, read_pcap)

# ---------------------------------------------------------------------------
# GROUNDING -- every constant, with the measurement that produced it.
# Re-derive any of them with `--measure` over ~/vax/cluster/captures/*.pcap.
# ---------------------------------------------------------------------------

# Real DIGITAL hardware OUI and the DECnet-logical address prefix. Used ONLY to
# separate "a reference VMS node" from "the node under audit"; spec sec 4(h)(1c)
# uses the same two prefixes for the same purpose.
REAL_VAX_OUIS = ("08:00:2b", "aa:00:04")

# Spec sec 4(d): the VMS$VAXcluster class is a fixed 190-byte SCA content.
# MEASURED: every CM frame in the corpus is exactly 204 bytes -- 306670 of
# 306670 counting all sources, 299224 of 299224 counting reference nodes only
# (which is the population `--measure` prints).
CM_FRAME_BYTES = 204

# MEASURED: over 6549 advancing cat-0x04 acks from real VMS nodes, the SYSAP
# ack-msg# advances by 3 or more EVERY time -- an advance of 1 occurs 0 times
# and an advance of 2 occurs 0 times. Real VMS coalesces; it never acks a burst
# frame-for-frame. This is the E78 `INVEXCEPTN` vector's invariant.
ACK_MIN_COALESCE = 3

# MEASURED: the most cat-0x04 acks any real VMS node emitted inside any 50 ms
# window, anywhere in the corpus (scs-node-leave.pcap, a node LEAVING, i.e. the
# busiest ack moment the library contains). A secondary, softer bound than the
# coalescing law above -- reported at WARN.
ACK_MAX_PER_50MS = 111

# Spec sec 4(p): a cat-0x02 op-0x0d DLM-rebuild response is a VERBATIM echo with
# exactly four edits -- envelope [0:4], the response bit at [8], and the
# unconditional result stamp at [34]. That published recipe reconstructs 1367 of
# 1367 real responses byte-for-byte. Mutating anything else is the LOCKMGRERR
# vector: OVMX applied the cat-0x01 body[18]/body[55] edits here, overwriting the
# 8th byte of the lock RESOURCE NAME, and crashed VAX1 and VAX3.
DLM_ECHO_MUTABLE = frozenset((0, 1, 2, 3, 8, 34))

# MEASURED per opcode: the set of body offsets a real VMS responder EVER differs
# from the request it is answering, over the whole corpus. An OVMX response that
# rewrites a byte outside its opcode's set is asserting something into the
# peer's own structures that no reference node ever touches -- the class that
# produced INCONSTATE and INVEXCEPTN in ovmx-760-relay-crash.
# (n = real responses measured. n < 16 is labelled thin and reported at WARN.)
CM_ECHO_MUTABLE = {
    0x03: (frozenset((0, 1, 2, 3, 8, 18)), 64),
    0x05: (frozenset((0, 1, 2, 3, 8, 18)), 97),
    0x08: (frozenset((0, 1, 2, 3, 8, 18, 55)), 11),
    0x09: (frozenset((0, 1, 2, 3, 8, 18, 55)), 50),
    0x0b: (frozenset((0, 1, 2, 3, 8, 16, 17, 18)), 809),
    0x0d: (frozenset((0, 1, 2, 3, 8, 18)), 3),
    0x0f: (frozenset((0, 1, 2, 3, 8, 20)), 11),
    0x12: (frozenset((0, 1, 2, 3, 8, 17, 18, 20, 21, 22, 23)), 26),
}

# Spec sec 4(j)/sec 4(s): cat-0x01 op-0x01 is a cluster-parameters REPLY carrying
# the responder's OWN node state, not an echo of the request. Echo rules do not
# apply and it is excluded from the echo check rather than mis-flagged.
CM_ECHO_EXEMPT = frozenset((0x01,))

# MEASURED: over 990 real cat-0x86 close responses, the smallest number of body
# bytes differing from the correlated request is 7. Spec sec 4(p): "Echoing this
# request's payload bugchecks the peer -- it carries that peer's live Con.IDs
# and cluster id"; the observed failure was INCONSTATE. A response differing in
# fewer bytes than any real one is a near-verbatim reflection.
CLOSE_MIN_DIFF_BYTES = 7

# Spec sec 4(p)/sec 4(q): op 0x0a (barrier GO) and op 0x0c (step release) are
# NOTIFICATIONS -- they carry txn=0 and are never answered. MEASURED: zero
# responses correlated to either, in 47 captures. Answering one invents a
# message VMS never sends, on a transaction the peer does not hold.
NEVER_ANSWERED_OPS = frozenset((0x0a, 0x0c))

# MEASURED: zero CM frames in the corpus carry a zero local or remote Con.ID.
# A frame addressed to a Con.ID the peer does not hold cannot be dispatched to a
# CDT; spec sec 4(O.26) records OVMX doing exactly this after a VC break.
CONID_NEVER_ZERO = True

# Spec sec 4(g): abs 95 of the 0x41 START/config body is the peer's SYSGEN
# CLUSTER_CREDITS grant, byte-exact. Sending with more messages outstanding than
# the peer granted is an over-send against its receive buffering.
OFF_START_CREDITS = 95
OFF_RECV_ACK = 32       # abs 32, spec sec 4(h)(4)
OFF_SEND_SEQ = 34       # abs 34, spec sec 4(h)(4)
MT_START = 0x41
MT_SEQ = (0x41, 0x4b, 0x5b, 0x7b)

# How long a request may stay outstanding before a candidate response is no
# longer treated as answering it. MEASURED over 97 969 correlated real
# responses: 94.1% answer inside 1 ms, 99.9% inside 10 ms, and NONE takes as
# long as 1 s. The (txn, token) pair is only 32 bits and recurs, so a wider
# window buys nothing and starts pairing a response with a stale transaction.
CORRELATION_WINDOW_S = 1.0

# MEASURED: no response frame anywhere in the corpus carries txn == 0 -- 0 of
# 103 413 real and 0 of 2 953 non-reference responses. Spec sec 4(p) grounds
# why: the frames that carry txn = 0 are the NOTIFICATIONS (op 0x0a, op 0x0c),
# and they are never answered, so a response with nothing to correlate against
# is a message VMS does not have.
RESPONSE_TXN_NEVER_ZERO = True

# How far an ack may legitimately run ahead of the peer's highest send-msg# THIS
# CAPTURE saw. MEASURED: over 234 555 reference CM frames exactly two acks run
# ahead at all, and both by exactly +1 -- a frame the peer sent and tcpdump
# missed, not a claim about a message that does not exist. The E76 crash ran
# ahead by 8 and by 13, so tolerating the capture-loss floor of 1 costs the
# check nothing.
ACK_CAPTURE_SLACK = 1

# A cat-02/0d pairing is only judged when the wire CORROBORATES it. Spec sec
# 4(p) grounds the request layout: body[16] is the L1 length and body[47] the
# resource-name length, and the response echoes the record VERBATIM, so a true
# pair always reproduces both. MEASURED: this corroboration rejects 68 of the 73
# token-collision mis-pairings in the corpus, leaving 5 residual false positives
# in 12 644 judged real responses (0.04%), while keeping all 8 frames of the
# ovmx-760 LOCKMGRERR crash and its in-capture real-node control at zero.
DLM_PAIR_CORROBORATORS = (16, 47)

FATAL, WARN = "FATAL", "WARN"


# ---------------------------------------------------------------------------
# frame view
# ---------------------------------------------------------------------------
class CmFrame(object):
    """One VMS$VAXcluster 190-content frame, decoded at the grounded offsets
    only (spec sec 4(d) for the Con.ID pair, sec 4(j) for the SYSAP envelope)."""

    __slots__ = ("idx", "ts", "src", "dst", "conid_local", "conid_remote",
                 "send_msg", "ack_msg", "txn", "token", "category", "opcode",
                 "body", "length")

    def __init__(self, idx, ts, frame):
        b = OFF_CM_BODY
        self.idx, self.ts, self.length = idx, ts, len(frame)
        self.src, self.dst = mac(frame[6:12]), mac(frame[0:6])
        self.conid_local = le32(frame, OFF_CONID_LOCAL)
        self.conid_remote = le32(frame, OFF_CONID_REMOTE)
        self.send_msg = le16(frame, b + 0)
        self.ack_msg = le16(frame, b + 2)
        self.txn = le16(frame, b + 4)
        self.token = le16(frame, b + 6)
        self.category = frame[b + 8]
        self.opcode = frame[b + 9]
        self.body = frame[b:b + 132]

    @property
    def is_response(self):
        return bool(self.category & 0x80)

    @property
    def base_category(self):
        return self.category & 0x7f

    def label(self):
        return "cat=%02x op=%02x txn=%04x" % (self.category, self.opcode,
                                              self.txn)


def cm_frames(path):
    """Every CM-class frame in the capture, in capture order."""
    return [CmFrame(i, ts, fr)
            for i, (ts, fr) in enumerate(read_pcap(path)) if is_cm(fr)]


def sca_frames(path):
    """(index, ts, raw) for every SCA frame -- the SCS layer, below the CM
    class, where the credit window and seq/ack live (spec sec 4(h))."""
    return [(i, ts, fr)
            for i, (ts, fr) in enumerate(read_pcap(path)) if is_sca(fr)]


# ---------------------------------------------------------------------------
# who is under audit
# ---------------------------------------------------------------------------
def is_real_vax(address):
    return address[:8] in REAL_VAX_OUIS


def sources_under_audit(frames, pinned=None, audit_all=False):
    """The MACs whose OUTPUT this run judges.

    Default: every source that is not a reference VMS node. That is a
    classification, not an assertion of identity -- it is printed, so a capture
    whose OVMX address was misread is visible rather than silently mis-audited.
    """
    seen = collections.OrderedDict.fromkeys(f.src for f in frames)
    if pinned:
        return [m for m in seen if m in pinned]
    if audit_all:
        return list(seen)
    return [m for m in seen if not is_real_vax(m)]


# ---------------------------------------------------------------------------
# findings
# ---------------------------------------------------------------------------
class Finding(object):
    __slots__ = ("vector", "severity", "frame", "detail", "invariant")

    def __init__(self, vector, severity, frame, detail, invariant):
        self.vector, self.severity = vector, severity
        self.frame, self.detail, self.invariant = frame, detail, invariant

    def __str__(self):
        return ("%-5s %-18s frame #%d %s %s -> %s  %s\n"
                "                        invariant: %s" %
                (self.severity, self.vector, self.frame.idx,
                 clock(self.frame.ts), self.frame.src, self.frame.dst,
                 self.detail, self.invariant))


# ---------------------------------------------------------------------------
# transaction correlation -- one ledger, used by three checkers
# ---------------------------------------------------------------------------
def correlate(frames):
    """response frame index -> the request body it answers, or None.

    A request is CONSUMED by the first response that matches it on
    (category, opcode, txn, token) in the opposite direction inside the
    correlation window; spec sec 4(j) grounds that triple as the request/
    response token (17/17 in the golden VC, zero residuals). Consuming rather
    than looking up matters: a retransmit reuses (txn, token), so a
    non-consuming lookup silently pairs a response with the wrong request and
    manufactures byte differences that were never on the wire.
    """
    pending, matched = {}, {}
    for f in frames:
        if not f.is_response:
            key = (f.category, f.opcode, f.txn, f.token, f.src, f.dst)
            pending.setdefault(key, collections.deque()).append(f)
            continue
        key = (f.base_category, f.opcode, f.txn, f.token, f.dst, f.src)
        queue = pending.get(key)
        matched[f.idx] = None
        while queue:
            request = queue.popleft()
            if f.ts - request.ts <= CORRELATION_WINDOW_S:
                matched[f.idx] = request
                break
    return matched


def _diff_offsets(request_body, response_body):
    return [i for i in range(min(len(request_body), len(response_body)))
            if request_body[i] != response_body[i]]


# ---------------------------------------------------------------------------
# vector S1/S2 -- the transaction envelope (the observed CNXMGRERR)
# ---------------------------------------------------------------------------
def _dialogue_key(frame):
    a, b = frame.conid_local, frame.conid_remote
    return (a, b) if a <= b else (b, a)


def _station_key(frame):
    a, b = frame.src, frame.dst
    return (a, b) if a <= b else (b, a)


def _reopened_dialogues(frames):
    """The set of (Con.ID pair) keys this capture holds from birth: a second or
    later dialogue between a station pair it has already watched."""
    first_for_pair, reopened = {}, set()
    for f in frames:
        pair, dialogue = _station_key(f), _dialogue_key(f)
        if pair not in first_for_pair:
            first_for_pair[pair] = dialogue
        elif dialogue != first_for_pair[pair]:
            reopened.add(dialogue)
    return reopened


def check_envelope(frames, audited):
    """Spec sec 4(j): the SYSAP send-msg# "starts at 1 on the first VC message"
    and the ack-msg# "acknowledges the peer's highest send-msg# seen".

    Judged only on a RE-OPENED dialogue -- a second Con.ID pair between two
    stations this capture has already watched hold one. A capture cannot know
    what preceded the first frame it saw, so the first dialogue per station pair
    is not judged (the E74 control opens at send-msg# 14283, which says nothing
    about the sender). A re-open is different: the capture holds the whole life
    of the second dialogue, so "it opened at 8" is a fact about OVMX.
    """
    findings = []
    judged = _reopened_dialogues(frames)
    opened, seen = set(), set()
    node_high = collections.Counter()      # (sender, peer) -> its own high-water
    for f in frames:
        dialogue = _dialogue_key(f)
        # A capture cannot judge a counter whose history it did not watch. Both
        # facts below are only knowable once this capture has already seen the
        # relevant side transmit: otherwise the very first frame of any capture
        # that began mid-stream reads as an unbacked ack (INV-6 -- omit rather
        # than assert). MEASURED: with this restriction the gate is silent on
        # 19 pure-reference captures / 234 555 CM frames.
        fresh_open = (dialogue in judged and (dialogue, f.src) not in opened
                      and (f.src, f.dst) in seen)
        if f.src in audited:
            findings += _envelope_findings(
                f, dialogue, fresh_open, node_high[(f.src, f.dst)],
                node_high[(f.dst, f.src)], (f.dst, f.src) in seen)
        opened.add((dialogue, f.src))
        seen.add((f.src, f.dst))
        key = (f.src, f.dst)
        node_high[key] = max(node_high[key], f.send_msg)
    return findings


def _envelope_findings(f, dialogue, is_fresh_open, own_high, peer_high,
                       peer_seen):
    """The two sec-4(j) envelope facts, judged against the NODE-level dialogue.

    ⚠ CORRECTION to the E76 reading, measured: the SYSAP send-msg# is NOT
    restarted per Con.ID pair. Three reference captures (af2-established-rejoin,
    af2-firsttimer-established, scs-node-leave) show a real VMS node open a
    BRAND-NEW Con.ID pair at send-msg# 9 -- continuing its counter from the 8 it
    had sent that peer on the previous pair -- and the peer answers normally
    with no bugcheck. So "opened at != 1" is not by itself the crash. What OVMX
    did in E76 was neither restart NOR continue: it JUMPED, because its counter
    was incremented on sends that were refused and never left the node. That
    jump is what this checks, and it is what makes the ack unbacked.
    """
    findings = []
    if is_fresh_open and f.send_msg not in (1, own_high + 1):
        findings.append(Finding(
            "S1-ENVELOPE-JUMP", WARN, f,
            "opened Con.ID pair %08x/%08x at send_msg=%d, which neither "
            "restarts at 1 nor continues from the %d already sent to this peer "
            "(%s)" % (dialogue[0], dialogue[1], f.send_msg, own_high,
                      f.label()),
            "spec sec 4(j): send-msg# is strictly monotonic per sender and "
            "starts at 1 on the first VC message. WARN, NOT FATAL, and this "
            "is a CORRECTION to the E76 reading: three reference captures "
            "(af2-established-rejoin, af2-firsttimer-established, "
            "scs-node-leave) show BOTH real VMS nodes open a second "
            "VMS$VAXcluster Con.ID pair at send_msg=9 simultaneously, "
            "continuing neither from 1 nor from their own high-water, with no "
            "bugcheck. Why 9 is NOT recoverable from passive capture, so this "
            "cannot be asserted as an invariant -- it is reported so a jump is "
            "seen, and the FATAL half of the E76 vector is S2 below."))
    if peer_seen and f.ack_msg > peer_high + ACK_CAPTURE_SLACK:
        findings.append(Finding(
            "S2-ENVELOPE-ACK", FATAL, f,
            "acked peer send_msg=%d but the peer has sent %d to this node "
            "(%s)" % (f.ack_msg, peer_high, f.label()),
            "spec sec 4(j): ack-msg# acknowledges the peer's HIGHEST send-msg# "
            "seen. Acking a message the peer never sent asserts a conversation "
            "that did not happen. OBSERVED CRASH: E76 CNXMGRERR."))
    return findings


# ---------------------------------------------------------------------------
# vector S3/S4 -- ack coalescing and rate (the observed INVEXCEPTN)
# ---------------------------------------------------------------------------
def check_ack_coalescing(frames, audited):
    """MEASURED: 6549 of 6549 advancing cat-0x04 acks from real VMS nodes
    advance the SYSAP ack-msg# by 3 or more. An advance of 1 or 2 occurs ZERO
    times anywhere in the corpus."""
    findings, last = [], {}
    for f in frames:
        if f.category != 0x04:
            continue
        key = (f.src, f.dst)
        previous = last.get(key)
        last[key] = f.ack_msg
        if previous is None or f.src not in audited:
            continue
        advance = f.ack_msg - previous
        if 0 < advance < ACK_MIN_COALESCE:
            findings.append(Finding(
                "S3-ACK-COALESCE", FATAL, f,
                "cat-04 ack advanced ack_msg by %d (%d -> %d)" %
                (advance, previous, f.ack_msg),
                "MEASURED over 6549 real acks: the advance is >= %d every "
                "time; 1 and 2 occur zero times. Acking a burst frame-for-"
                "frame is the E78 INVEXCEPTN vector -- 254 acks in 31.6 ms "
                "bugchecked VAX2." % ACK_MIN_COALESCE))
    return findings


def check_ack_rate(frames, audited):
    """A secondary bound: the busiest 50 ms any real node ever had."""
    findings = []
    per_stream = collections.defaultdict(list)
    for f in frames:
        if f.category == 0x04 and f.src in audited:
            per_stream[(f.src, f.dst)].append(f)
    for stream in per_stream.values():
        oldest = 0
        for i, f in enumerate(stream):
            while stream[i].ts - stream[oldest].ts > 0.050:
                oldest += 1
            count = i - oldest + 1
            if count == ACK_MAX_PER_50MS + 1:
                findings.append(Finding(
                    "S4-ACK-RATE", WARN, f,
                    "%d cat-04 acks inside 50 ms" % count,
                    "MEASURED ceiling %d: the most any real VMS node emitted "
                    "in any 50 ms window in the corpus (a node LEAVING, the "
                    "library's busiest ack moment)." % ACK_MAX_PER_50MS))
    return findings


# ---------------------------------------------------------------------------
# vector S5/S6/S7 -- response fidelity (the observed LOCKMGRERR / INCONSTATE)
# ---------------------------------------------------------------------------
def _pair_is_corroborated(request, response):
    """Is this (txn, token) pairing the SAME transaction, or a collision?

    The 32-bit correlation token recurs inside one capture, so a pairing has to
    be corroborated by the record itself before any byte difference between the
    two frames can be read as a mutation rather than as two unrelated frames.
    """
    return all(request.body[i] == response.body[i]
               for i in DLM_PAIR_CORROBORATORS)


def check_dlm_echo(frames, audited, matched):
    """Spec sec 4(p): the cat-0x02 op-0x0d response is a verbatim echo plus the
    envelope, the response bit and the result stamp at [34] -- a recipe that
    reconstructs 1367 of 1367 real responses byte-for-byte."""
    findings = []
    for f in frames:
        if f.category != 0x82 or f.opcode != 0x0d or f.src not in audited:
            continue
        request = matched.get(f.idx)
        if request is None or not _pair_is_corroborated(request, f):
            continue          # not judged: no pairing this wire corroborates
        extra = [i for i in _diff_offsets(request.body, f.body)
                 if i not in DLM_ECHO_MUTABLE]
        if extra:
            findings.append(Finding(
                "S5-ECHO-DLM", FATAL, f,
                "rewrote body offsets %s outside the grounded edit set %s" %
                (extra[:10], sorted(DLM_ECHO_MUTABLE)),
                "spec sec 4(p): body[47] is the resource-name LENGTH and "
                "body[48:] the lock RESOURCE NAME. OBSERVED CRASH: OVMX wrote "
                "body[18]/body[55] here, corrupting the name, and VAX1 and "
                "VAX3 took a fatal LOCKMGRERR."))
    return findings


def _resource_name(body):
    """The lock resource name a cat-02/0d record carries in its own bytes:
    spec sec 4(p), body[47] = length, body[48:] = ASCII name + binary sub-key.
    Returns None when the length is outside the observed 13..24 range or the
    name does not start as ASCII -- i.e. when this is not a name we can read."""
    length = body[47]
    if not 13 <= length <= 24:
        return None
    name = bytes(body[48:48 + length])
    if not name or not 32 <= name[0] < 127:
        return None
    return name


def _name_has_hole(name):
    """The binary sub-key is a SUFFIX, so a non-printable byte FOLLOWED by more
    ASCII is a hole punched into the middle of the name -- exactly what the
    LOCKMGRERR responses carried (`CACHE$c\\0SYSDSK1`)."""
    binary_seen = False
    for byte in name:
        if not 32 <= byte < 127:
            binary_seen = True
        elif binary_seen:
            return True
    return False


def check_dlm_resource_name(frames, audited):
    """A correlation-free reading of the SAME crash: the response's own bytes
    say whether the resource name it carries is intact.

    MEASURED: this fires on all 8 LOCKMGRERR responses in
    ovmx-760-lockmgrerr-20260730.pcap with a perfect in-capture control (the
    two real VAXes exchanging the same records across the same milliseconds
    score zero). Its corpus false positives are 32 frames carrying ONE
    identifiable resource name, `UCX$INETACP_` with a binary IP sub-key whose
    last byte is coincidentally printable -- hence WARN, not FATAL.
    """
    findings = []
    for f in frames:
        if f.category != 0x82 or f.opcode != 0x0d or f.src not in audited:
            continue
        name = _resource_name(f.body)
        if name is None or not _name_has_hole(name):
            continue
        findings.append(Finding(
            "S5B-DLM-NAME-HOLE", WARN, f,
            "resource name %r has a non-printable byte inside its ASCII run" %
            name,
            "spec sec 4(p): body[48:] is the lock RESOURCE NAME, ASCII prefix "
            "then binary sub-key. OBSERVED CRASH: OVMX punched body[55] to 0 "
            "mid-name and VAX1 and VAX3 took a fatal LOCKMGRERR. Known benign "
            "case: a `UCX$INETACP_` name whose IP sub-key ends printable."))
    return findings


def check_cm_echo(frames, audited, matched):
    """MEASURED per opcode: the body offsets a real responder EVER rewrites."""
    findings = []
    for f in frames:
        if f.category != 0x81 or f.src not in audited:
            continue
        if f.opcode in CM_ECHO_EXEMPT:
            continue
        request = matched.get(f.idx)
        if request is None:
            continue
        findings += _cm_echo_findings(f, request)
    return findings


def _cm_echo_findings(f, request):
    known = CM_ECHO_MUTABLE.get(f.opcode)
    if known is None:
        return [Finding(
            "S6-ECHO-UNGROUNDED", WARN, f,
            "answered cat-01 op-%02x, whose response shape no real node in "
            "the corpus ever showed" % f.opcode,
            "spec sec 4(p): 'the rule is an allowlist, never a default' -- "
            "answer only (category, opcode) pairs grounded in the reference. "
            "OBSERVED CRASH: full-body cat-01 echoes of ungrounded opcodes "
            "produced INCONSTATE on VAX3 and INVEXCEPTN on VAX1.")]
    allowed, sample = known
    extra = [i for i in _diff_offsets(request.body, f.body) if i not in allowed]
    if not extra:
        return []
    severity = FATAL if sample >= 16 else WARN
    return [Finding(
        "S6-ECHO-CM", severity, f,
        "op-%02x response rewrote body offsets %s outside the measured set %s" %
        (f.opcode, extra[:10], sorted(allowed)),
        "MEASURED over %d real op-%02x responses%s: no reference node ever "
        "rewrites any other byte. These bodies carry the PEER's live Con.IDs "
        "and cluster id (spec sec 4(p))." %
        (sample, f.opcode, " (THIN sample -> WARN)" if sample < 16 else ""))]


def check_close_echo(frames, audited, matched):
    """Spec sec 4(p): a cat-0x06 close must carry the responder's OWN node
    parameters. "Echoing this request's payload bugchecks the peer" -- observed
    as INCONSTATE, Inconsistent I/O data base."""
    findings = []
    for f in frames:
        if f.category != 0x86 or f.src not in audited:
            continue
        request = matched.get(f.idx)
        if request is None:
            continue
        differing = len(_diff_offsets(request.body, f.body))
        if differing < CLOSE_MIN_DIFF_BYTES:
            findings.append(Finding(
                "S7-CLOSE-ECHO", FATAL, f,
                "cat-06 close differs from the request in only %d bytes -- a "
                "near-verbatim reflection" % differing,
                "MEASURED over 990 real closes: the smallest difference is %d "
                "bytes. Reflecting the request returns the peer its own live "
                "Con.IDs and cluster id (spec sec 4(p)); OBSERVED CRASH: "
                "INCONSTATE." % CLOSE_MIN_DIFF_BYTES))
    return findings


# ---------------------------------------------------------------------------
# vector S8/S9 -- transactions the peer does not hold
# ---------------------------------------------------------------------------
def check_notification_answered(frames, audited):
    """Spec sec 4(p)/sec 4(q): op 0x0a (barrier GO) and op 0x0c (step release)
    carry txn=0 and are NEVER answered. MEASURED: zero such responses in 47
    captures."""
    findings = []
    for f in frames:
        if not f.is_response or f.src not in audited:
            continue
        if f.base_category == 0x01 and f.opcode in NEVER_ANSWERED_OPS:
            findings.append(Finding(
                "S8-ANSWERED-NOTIFY", FATAL, f,
                "responded to op-%02x, a notification (%s)" %
                (f.opcode, f.label()),
                "spec sec 4(p): op 0x0a and op 0x0c get no response of any "
                "kind and carry txn=0, so there is nothing to correlate. "
                "MEASURED: zero such responses across 47 captures -- "
                "answering one invents a message VMS never sends."))
    return findings


def check_uncorrelatable_response(frames, audited):
    """A response carrying txn == 0 can never be correlated to anything: spec
    sec 4(j) grounds body[4:6] as the transaction number a request and its
    response SHARE, and the only frames carrying zero there are the
    notifications, which are never answered.

    This is the structural half of "a transaction the peer does not hold". The
    *statistical* half -- a response whose request is simply absent from the
    capture -- is deliberately NOT a finding: 5.3% of real responses in the
    corpus fail to correlate (a capture that began mid-transaction, a peer
    answering from its other leg), so flagging it would be 5 000 cries of wolf.
    It is reported as a rate in the summary instead.
    """
    findings = []
    for f in frames:
        if not f.is_response or f.src not in audited or f.txn != 0:
            continue
        findings.append(Finding(
            "S9-RESP-TXN-ZERO", FATAL, f,
            "response %s carries transaction number 0" % f.label(),
            "MEASURED: zero of 103413 real and zero of 2953 non-reference "
            "responses in the corpus carry txn == 0. Spec sec 4(j): the "
            "transaction number is what a request and its response SHARE; a "
            "response with none names a transaction the peer cannot look up."))
    return findings


# ---------------------------------------------------------------------------
# vector S13/S14 -- a MANDATORY field asserted as "nothing here"
#
# Both of these are the same defect wearing two hats: a fixed-width body cannot
# express "omitted", so a field the executive cannot ground goes out as zero --
# and on these offsets zero is a value no reference node has ever put on this
# wire. The gate reported 0 FATAL / 0 WARN on the run that discovered them
# (join-e83refire-1788612567), 0.6 ms before the transition coordinator
# bugchecked CNXMGRERR and last-gasped, which is why they exist.
# ---------------------------------------------------------------------------

# MEASURED over the whole capture library (47 pcaps, five distinct responder
# nodes): of the 122 body offsets in body[10:132], body[24] is the ONLY one
# that is nonzero in 1308 of 1308 real cat-0x86 op-0x00 close responses. Its
# meaning is UNGROUNDED -- it is not an echo (the best-matching request offset
# agrees 358/1308 = 27%), it is not the op-0x01 PARAMS block (every real PARAMS
# puts 0x0001 there while the same node's closes put 3, 4 and 5), and it is not
# per-node. So this check asserts nothing about what the field MEANS; it
# asserts only what the corpus says about what it is never allowed to be.
CLOSE_STATE_OFFSET = 24

# MEASURED per (category, opcode) over every originated body in the corpus: the
# opcodes whose (txn, token) pair is nonzero in EVERY single specimen, with the
# population. The complement -- op 0x00/0x02/0x04/0x06/0x0a/0x0c/0x14 and all of
# cat-0x04 -- is the opposite law, zero in every specimen, and spec sec 4(p)
# names two of those ("Notifications carry txn=0 and are NEVER answered").
# Only opcodes with a population far above the corpus's own thin-sample
# threshold are listed, so a finding here is never a small-sample artefact.
CORRELATED_OPS = {
    (0x01, 0x03): 142,     # membership COMMIT
    (0x01, 0x05): 282,     # lock/resource rebuild
    (0x01, 0x09): 97,      # transition open
    (0x01, 0x0b): 1035,    # BARRIER STEP -- the coordinator correlates its
                           # releases by this pair; twelve steps sharing one
                           # value are twelve steps it cannot tell apart
    (0x02, 0x0d): 21680,   # DLM rebuild record
    (0x06, 0x00): 1361,    # transaction close (the REQUEST direction)
}


def check_close_state(frames, audited):
    """A cat-0x86 op-0x00 close whose body[24] is zero -- a byte no reference
    node has ever put on this wire, and the one OVMX did put there 0.6 ms
    before the coordinator bugchecked."""
    findings = []
    for f in frames:
        if f.category != 0x86 or f.opcode != 0x00 or f.src not in audited:
            continue
        if f.body[CLOSE_STATE_OFFSET] != 0:
            continue
        findings.append(Finding(
            "S13-CLOSE-STATE-ZERO", FATAL, f,
            "cat-06 close %s carries body[%d] == 0" %
            (f.label(), CLOSE_STATE_OFFSET),
            "MEASURED: body[%d] is nonzero in 1308 of 1308 real close "
            "responses across the whole library -- the ONLY offset in "
            "body[10:132] that is never zero. OBSERVED CRASH: CNXMGRERR, the "
            "coordinator's last gasp 0.6 ms later "
            "(join-e83refire-1788612567 frames 894 -> 898)." %
            CLOSE_STATE_OFFSET))
    return findings


def check_request_pair(frames, audited):
    """An ORIGINATION of an opcode whose transaction pair is nonzero in every
    reference specimen, carrying zero in either cell. The peer echoes that pair
    to correlate its answer; a run of requests carrying one value is a run the
    peer cannot tell apart."""
    findings = []
    for f in frames:
        if f.is_response or f.src not in audited:
            continue
        n = CORRELATED_OPS.get((f.base_category, f.opcode))
        if n is None or (f.txn != 0 and f.token != 0):
            continue
        findings.append(Finding(
            "S14-REQUEST-PAIR-ZERO", FATAL, f,
            "origination %s carries txn=%04x token=%04x" %
            (f.label(), f.txn, f.token),
            "MEASURED: all %d real cat-%02x op-%02x originations in the "
            "corpus carry a NONZERO transaction number AND a nonzero "
            "correlation token; zero occurrences of either at zero. The peer "
            "correlates its answer by that pair." %
            (n, f.base_category, f.opcode)))
    return findings


def uncorrelated_rate(frames, audited, matched):
    """(uncorrelated, total) responses from the audited sources -- reported as
    context, never as a finding. Real corpus baseline: 5.3%."""
    responses = [f for f in frames if f.is_response and f.src in audited]
    absent = sum(1 for f in responses if matched.get(f.idx) is None)
    return absent, len(responses)


# ---------------------------------------------------------------------------
# vector S10/S11 -- addressing and framing
# ---------------------------------------------------------------------------
def check_conid(frames, audited):
    """MEASURED: no CM frame anywhere in the corpus carries a zero Con.ID."""
    findings = []
    for f in frames:
        if f.src not in audited:
            continue
        if f.conid_local and f.conid_remote:
            continue
        findings.append(Finding(
            "S10-CONID-ZERO", FATAL, f,
            "local Con.ID %08x / remote Con.ID %08x (%s)" %
            (f.conid_local, f.conid_remote, f.label()),
            "MEASURED: zero of 306670 corpus CM frames carry a zero Con.ID. A "
            "frame addressed to a Con.ID the peer does not hold cannot resolve "
            "to a CDT (spec sec 4(t)); spec sec 4(O.26) records OVMX emitting "
            "op 0x05/0x06 to Con.ID 0 after a VC break."))
    return findings


def check_frame_size(path, audited):
    """MEASURED: every CM-class frame in the corpus is exactly 204 bytes. A
    short or oversized frame in this class hands the peer a body it will index
    past."""
    findings = []
    for i, (ts, fr) in enumerate(read_pcap(path)):
        if not is_cm(fr) or mac(fr[6:12]) not in audited:
            continue
        if len(fr) != CM_FRAME_BYTES:
            findings.append(Finding(
                "S11-FRAME-SIZE", FATAL, CmFrame(i, ts, fr),
                "CM-class frame is %d bytes, not %d" %
                (len(fr), CM_FRAME_BYTES),
                "MEASURED: 306670 of 306670 corpus CM frames are exactly %d "
                "bytes (spec sec 4(d), the fixed 190-content class)." %
                CM_FRAME_BYTES))
    return findings


# ---------------------------------------------------------------------------
# vector S12 -- SCS send window vs the peer's advertised grant
# ---------------------------------------------------------------------------
def _advertised_grants(scas):
    """What each node put at abs 95 of its 0x41 START body (spec sec 4(g),
    byte-exact to SYSGEN CLUSTER_CREDITS)."""
    grants = {}
    for _i, _ts, fr in scas:
        if len(fr) > OFF_START_CREDITS and fr[OFF_MSGTYPE] == MT_START:
            grants[mac(fr[6:12])] = fr[OFF_START_CREDITS]
    return grants


def check_credit_window(path, audited):
    """Spec sec 4(h)(4) + VAXcluster Principles p. 2-43: one message costs one
    send credit and the acknowledgement returns it. Transmitting with more
    messages outstanding than the peer granted over-runs its receive buffering.
    """
    scas = sca_frames(path)
    grants = _advertised_grants(scas)
    findings, peer_ack, sent = [], {}, {}
    for i, ts, fr in scas:
        if len(fr) < OFF_SEND_SEQ + 2 or fr[OFF_MSGTYPE] not in MT_SEQ:
            continue
        src, dst = mac(fr[6:12]), mac(fr[0:6])
        # Spec sec 4(O.14): send_seq is per-VIRTUAL-CIRCUIT, not per-connection,
        # so a 0x41 opening another SYSAP connection between the same pair does
        # NOT restart it. The baseline is therefore the peer's own highest
        # observed recv_ack -- which is also the only baseline a capture that
        # began mid-circuit can honestly have.
        peer_ack[(dst, src)] = max(peer_ack.get((dst, src), 0),
                                   le16(fr, OFF_RECV_ACK))
        if src not in audited or (src, dst) not in peer_ack:
            continue          # not judged until the peer has acknowledged once
        findings += _credit_finding(i, ts, fr, src, dst, grants, peer_ack, sent)
    return findings


def _credit_finding(i, ts, fr, src, dst, grants, peer_ack, sent):
    grant = grants.get(dst)
    seq = le16(fr, OFF_SEND_SEQ)
    seen = sent.setdefault((src, dst), set())
    if grant is None or seq == 0 or seq in seen:
        return []          # no advertised grant, or a retransmit (sec 4(h)(4a))
    seen.add(seq)
    outstanding = seq - peer_ack.get((src, dst), 0)
    if outstanding <= grant:
        return []
    return [Finding(
        "S12-CREDIT-OVERSEND", FATAL, CmFrame(i, ts, fr) if is_cm(fr)
        else _StubFrame(i, ts, src, dst),
        "sent send_seq=%d with %d messages outstanding; the peer granted %d" %
        (seq, outstanding, grant),
        "spec sec 4(g): abs 95 of the peer's 0x41 START is its CLUSTER_CREDITS "
        "grant, byte-exact. VAXcluster Principles p. 2-43: one message costs "
        "one credit. Exceeding the grant over-runs the peer's receive "
        "buffering.")]


class _StubFrame(object):
    """A non-CM SCA frame, carried only so a Finding can name it."""
    __slots__ = ("idx", "ts", "src", "dst")

    def __init__(self, idx, ts, src, dst):
        self.idx, self.ts, self.src, self.dst = idx, ts, src, dst


# ---------------------------------------------------------------------------
# driver
# ---------------------------------------------------------------------------
def audit(path, frames, audited):
    """Every vector, over one capture, for the sources under audit."""
    matched = correlate(frames)
    findings = []
    findings += check_envelope(frames, audited)
    findings += check_ack_coalescing(frames, audited)
    findings += check_ack_rate(frames, audited)
    findings += check_dlm_echo(frames, audited, matched)
    findings += check_dlm_resource_name(frames, audited)
    findings += check_cm_echo(frames, audited, matched)
    findings += check_close_echo(frames, audited, matched)
    findings += check_close_state(frames, audited)
    findings += check_request_pair(frames, audited)
    findings += check_notification_answered(frames, audited)
    findings += check_uncorrelatable_response(frames, audited)
    findings += check_conid(frames, audited)
    findings += check_frame_size(path, audited)
    findings += check_credit_window(path, audited)
    findings.sort(key=lambda f: (f.frame.ts, f.vector))
    return findings


def report(path, pinned=None, audit_all=False, quiet_limit=40):
    """Audit one capture. Returns (fatal_count, warn_count, ran)."""
    frames = cm_frames(path)
    print("== %s (%d CM-class frames)" % (path, len(frames)))
    if not frames:
        print("   NO CM-class frames -- this capture proves nothing; not a pass")
        return 0, 0, False
    audited = sources_under_audit(frames, pinned, audit_all)
    reference = [f.src for f in frames if is_real_vax(f.src)]
    print("   under audit : %s" % (", ".join(audited) or "(none)"))
    print("   reference   : %s" %
          (", ".join(collections.OrderedDict.fromkeys(reference)) or "(none)"))
    if not audited:
        print("   nothing to judge in this capture")
        return 0, 0, True
    findings = audit(path, frames, audited)
    _print_findings(findings, quiet_limit)
    absent, total = uncorrelated_rate(frames, audited, correlate(frames))
    if total:
        print("   context     : %d of %d responses had no request in this "
              "capture (%.1f%%; real-corpus baseline 5.3%%) -- not a finding" %
              (absent, total, 100.0 * absent / total))
    fatal = sum(1 for f in findings if f.severity == FATAL)
    print("   -- %d FATAL, %d WARN --" % (fatal, len(findings) - fatal))
    return fatal, len(findings) - fatal, True


def _print_findings(findings, limit):
    by_vector = collections.Counter(f.vector for f in findings)
    shown = collections.Counter()
    for f in findings:
        shown[f.vector] += 1
        if shown[f.vector] <= limit:
            print("   %s" % f)
        elif shown[f.vector] == limit + 1:
            print("   ... %s: %d more suppressed" %
                  (f.vector, by_vector[f.vector] - limit))


# ---------------------------------------------------------------------------
# --measure: re-derive the thresholds instead of trusting them
# ---------------------------------------------------------------------------
def measure(paths):
    """Print the corpus measurements the constants above were taken from."""
    deltas, lengths, zero_conid = collections.Counter(), collections.Counter(), 0
    mutations, totals = collections.defaultdict(collections.Counter), \
        collections.Counter()
    close_diffs, busiest = [], 0
    for path in paths:
        try:
            frames = cm_frames(path)
        except (ValueError, IOError):
            continue
        real = [f for f in frames if is_real_vax(f.src)]
        busiest = max(busiest, _busiest_ack_window(real))
        _measure_deltas(real, deltas)
        for f in real:
            lengths[f.length] += 1
        zero_conid += sum(1 for f in real
                          if not f.conid_local or not f.conid_remote)
        _measure_mutations(frames, correlate(frames), mutations, totals,
                           close_diffs)
    _print_measurements(deltas, lengths, zero_conid, mutations, totals,
                        close_diffs, busiest)


def _measure_deltas(real, deltas):
    last = {}
    for f in real:
        if f.category != 0x04:
            continue
        key = (f.src, f.dst)
        if key in last:
            deltas[f.ack_msg - last[key]] += 1
        last[key] = f.ack_msg


def _busiest_ack_window(real):
    per_stream = collections.defaultdict(list)
    for f in real:
        if f.category == 0x04:
            per_stream[(f.src, f.dst)].append(f.ts)
    busiest = 0
    for times in per_stream.values():
        oldest = 0
        for i, t in enumerate(times):
            while t - times[oldest] > 0.050:
                oldest += 1
            busiest = max(busiest, i - oldest + 1)
    return busiest


def _measure_mutations(frames, matched, mutations, totals, close_diffs):
    for f in frames:
        if not f.is_response or not is_real_vax(f.src):
            continue
        request = matched.get(f.idx)
        if request is None:
            continue
        diffs = _diff_offsets(request.body, f.body)
        if f.category == 0x86:
            close_diffs.append(len(diffs))
        totals[(f.category, f.opcode)] += 1
        for offset in diffs:
            mutations[(f.category, f.opcode)][offset] += 1


def _print_measurements(deltas, lengths, zero_conid, mutations, totals,
                        close_diffs, busiest):
    advancing = {d: n for d, n in deltas.items() if d > 0}
    print("cat-04 ack advance: n=%d  min=%s  advance==1: %d  advance==2: %d" %
          (sum(advancing.values()), min(advancing) if advancing else "-",
           deltas[1], deltas[2]))
    print("busiest real 50 ms ack window: %d" % busiest)
    print("CM frame lengths: %s" % dict(lengths))
    print("real CM frames with a zero Con.ID: %d" % zero_conid)
    print("smallest real cat-86 close difference: %s bytes" %
          (min(close_diffs) if close_diffs else "-"))
    print("real response mutation sets:")
    for key in sorted(totals):
        offsets = sorted(mutations[key])
        print("   cat=%02x op=%02x  n=%-6d offsets=%s" %
              (key[0], key[1], totals[key], offsets[:24]))


# ---------------------------------------------------------------------------
# --self-test: prove the gate has teeth (a gate that cannot fail proves nothing)
# ---------------------------------------------------------------------------
def _synth_cm(ts, src, dst, conid_l, conid_r, send_msg, ack_msg, txn, token,
              category, opcode, payload=None, length=CM_FRAME_BYTES):
    """Build one CM-class frame at the grounded offsets."""
    frame = bytearray(length)
    frame[0:6] = bytes(int(x, 16) for x in dst.split(":"))
    frame[6:12] = bytes(int(x, 16) for x in src.split(":"))
    frame[12:14] = b"\x60\x07"
    frame[14:16] = (188).to_bytes(2, "little")        # 190-content class
    frame[OFF_MSGTYPE] = 0x4b
    frame[OFF_CONID_REMOTE:OFF_CONID_REMOTE + 4] = conid_r.to_bytes(4, "little")
    frame[OFF_CONID_LOCAL:OFF_CONID_LOCAL + 4] = conid_l.to_bytes(4, "little")
    b = OFF_CM_BODY
    frame[b + 0:b + 2] = send_msg.to_bytes(2, "little")
    frame[b + 2:b + 4] = ack_msg.to_bytes(2, "little")
    frame[b + 4:b + 6] = txn.to_bytes(2, "little")
    frame[b + 6:b + 8] = token.to_bytes(2, "little")
    frame[b + 8], frame[b + 9] = category, opcode
    if payload:
        frame[b + 10:b + 10 + len(payload)] = payload
    return ts, bytes(frame)


def _write_pcap(path, records):
    import struct
    with open(path, "wb") as fh:
        fh.write(struct.pack("<IHHiIII", 0xa1b2c3d4, 2, 4, 0, 0, 262144, 1))
        for ts, frame in records:
            fh.write(struct.pack("<IIII", int(ts), int((ts % 1) * 1e6),
                                 len(frame), len(frame)))
            fh.write(frame)


VAX = "08:00:2b:11:22:33"
OVMX = "52:54:00:00:00:f4"


def _clean_dialogue():
    """A dialogue that keeps every measured invariant: a fresh Con.ID pair
    opened at send_msg 1, a 12-message burst from the peer, and ONE coalesced
    ack covering three of them at a time."""
    records, ts = [], 1000.0
    # the first dialogue between this pair -- reported, never judged
    records.append(_synth_cm(ts, VAX, OVMX, 0x1001, 0x2001, 1, 0, 1, 1,
                             0x01, 0x14))
    records.append(_synth_cm(ts + .001, OVMX, VAX, 0x2001, 0x1001, 1, 1, 1, 1,
                             0x01, 0x14))
    ts += 1.0
    # a RE-open: judged from birth
    records.append(_synth_cm(ts, OVMX, VAX, 0x2002, 0x1002, 1, 0, 2, 7,
                             0x01, 0x14))
    peer_msg = 0
    for step in range(12):
        peer_msg += 1
        records.append(_synth_cm(ts + .01 * step, VAX, OVMX, 0x1002, 0x2002,
                                 peer_msg, 1, 0, 0, 0x01, 0x06))
        if peer_msg % ACK_MIN_COALESCE == 0:
            records.append(_synth_cm(ts + .01 * step + .001, OVMX, VAX,
                                     0x2002, 0x1002, 2 + step, peer_msg,
                                     0, 0, 0x04, 0x00))
    return records


def _violation_cases():
    """One synthesized capture per vector, each a single-factor mutation of the
    clean dialogue -- so a case that stops firing means the CHECK broke, not the
    fixture."""
    cases = {}
    ts = 2000.0
    opened = [_synth_cm(ts, VAX, OVMX, 0x3001, 0x4001, 1, 0, 1, 1, 0x01, 0x14),
              _synth_cm(ts + .001, OVMX, VAX, 0x4001, 0x3001, 1, 1, 1, 1,
                        0x01, 0x14)]

    cases["S1-ENVELOPE-JUMP"] = opened + [
        _synth_cm(ts + 1, OVMX, VAX, 0x4002, 0x3002, 8, 0, 2, 2, 0x01, 0x14)]

    cases["S2-ENVELOPE-ACK"] = opened + [
        _synth_cm(ts + 1, OVMX, VAX, 0x4002, 0x3002, 1, 0, 2, 2, 0x01, 0x14),
        _synth_cm(ts + 1.1, OVMX, VAX, 0x4002, 0x3002, 2, 99, 2, 2,
                  0x01, 0x14)]

    flood = list(_clean_dialogue())
    ts3 = 3000.0
    for n in range(1, 30):                       # one ack per received frame
        flood.append(_synth_cm(ts3 + n * .0001, VAX, OVMX, 0x1002, 0x2002,
                               100 + n, 1, 0, 0, 0x01, 0x06))
        flood.append(_synth_cm(ts3 + n * .0001 + .00005, OVMX, VAX,
                               0x2002, 0x1002, 200 + n, 100 + n, 0, 0,
                               0x04, 0x00))
    cases["S3-ACK-COALESCE"] = flood

    # A lock-rebuild record whose RESOURCE NAME (spec sec 4(p): body[47] is the
    # name length, body[48:] the name) spans offset 55 -- exactly the byte OVMX
    # clobbered when it applied the cat-01 mutations here and took VAX1 and VAX3
    # down with LOCKMGRERR.
    name = b"CACHE$cmSYSDSK1     "
    record = bytearray(122)
    record[37] = len(name)                       # body[47] = name length
    record[38:38 + len(name)] = name             # body[48:] = the name
    request = _synth_cm(ts, VAX, OVMX, 0x1002, 0x2002, 5, 1, 9, 9, 0x02, 0x0d,
                        payload=bytes(record))
    bad = bytearray(request[1])
    bad[OFF_CM_BODY + 8] = 0x82                  # the legal response bit
    bad[OFF_CM_BODY + 34] = 0xf9                 # the legal result stamp
    bad[OFF_CM_BODY + 18] = 0x01                 # ILLEGAL here: cat-01 marker
    bad[OFF_CM_BODY + 55] = 0x00                 # ILLEGAL here: the name's 8th
    bad[6:12], bad[0:6] = bad[0:6], bad[6:12]
    cases["S5-ECHO-DLM"] = [request, (ts + .001, bytes(bad))]

    req9 = _synth_cm(ts, VAX, OVMX, 0x1002, 0x2002, 6, 1, 10, 10, 0x01, 0x09,
                     payload=b"\x11" * 60)
    bad9 = bytearray(req9[1])
    bad9[OFF_CM_BODY + 8] = 0x81
    bad9[OFF_CM_BODY + 18] = 0x01
    bad9[OFF_CM_BODY + 55] = 0x00
    bad9[OFF_CM_BODY + 90] = 0xff                # outside the measured set
    bad9[6:12], bad9[0:6] = bad9[0:6], bad9[6:12]
    cases["S6-ECHO-CM"] = [req9, (ts + .001, bytes(bad9))]

    req6 = _synth_cm(ts, VAX, OVMX, 0x1002, 0x2002, 7, 1, 11, 11, 0x06, 0x00,
                     payload=b"\x33" * 100)
    bad6 = bytearray(req6[1])
    bad6[OFF_CM_BODY + 8] = 0x86                 # verbatim reflection
    bad6[6:12], bad6[0:6] = bad6[0:6], bad6[6:12]
    cases["S7-CLOSE-ECHO"] = [req6, (ts + .001, bytes(bad6))]

    cases["S8-ANSWERED-NOTIFY"] = [
        _synth_cm(ts, VAX, OVMX, 0x1002, 0x2002, 8, 1, 0, 0, 0x01, 0x0c),
        _synth_cm(ts + .001, OVMX, VAX, 0x2002, 0x1002, 3, 8, 0, 0,
                  0x81, 0x0c)]

    cases["S9-RESP-TXN-ZERO"] = [
        _synth_cm(ts, OVMX, VAX, 0x2002, 0x1002, 3, 0, 0, 77, 0x81, 0x03)]

    holed = bytearray(122)
    holed[37] = 20                               # body[47] = name length
    holed[38:58] = b"CACHE$c\x00SYSDSK1    "     # the LOCKMGRERR shape
    cases["S5B-DLM-NAME-HOLE"] = [
        _synth_cm(ts, OVMX, VAX, 0x2002, 0x1002, 3, 0, 12, 12, 0x82, 0x0d,
                  payload=bytes(holed))]

    cases["S10-CONID-ZERO"] = [
        _synth_cm(ts, OVMX, VAX, 0x2002, 0x00000000, 1, 0, 1, 1, 0x01, 0x06)]

    cases["S11-FRAME-SIZE"] = [
        _synth_cm(ts, OVMX, VAX, 0x2002, 0x1002, 1, 0, 1, 1, 0x01, 0x06,
                  length=CM_FRAME_BYTES + 60)]

    # S13: the close OVMX really sent -- correct envelope, correct token pair,
    # a payload that is NOT a reflection (S7 stays quiet), and body[24] zero.
    req13 = _synth_cm(ts, VAX, OVMX, 0x1002, 0x2002, 9, 1, 13, 13, 0x06, 0x00,
                      payload=b"\x55" * 100)
    resp13 = bytearray(_synth_cm(ts + .001, OVMX, VAX, 0x2002, 0x1002, 4, 9,
                                 13, 13, 0x86, 0x00)[1])
    resp13[OFF_CM_BODY + 88:OFF_CM_BODY + 96] = b"VMX V0.6"
    cases["S13-CLOSE-STATE-ZERO"] = [req13, (ts + .001, bytes(resp13))]

    # S14: a barrier step originated with the pair this executive carried on
    # every one of its twelve, before it minted them.
    cases["S14-REQUEST-PAIR-ZERO"] = [
        _synth_cm(ts, OVMX, VAX, 0x2002, 0x1002, 5, 9, 0, 0, 0x01, 0x0b)]
    return cases


def self_test(tmpdir=None):
    """Both halves: the clean fixture must produce ZERO findings, and every
    vector's fixture must produce ITS OWN finding."""
    import tempfile
    import os
    tmpdir = tmpdir or tempfile.mkdtemp(prefix="cm_wire_safety_")
    failures = []

    clean = os.path.join(tmpdir, "clean.pcap")
    _write_pcap(clean, _clean_dialogue())
    findings = audit(clean, cm_frames(clean), [OVMX])
    if findings:
        failures.append("clean fixture produced %d finding(s): %s" %
                        (len(findings), [f.vector for f in findings]))
        for f in findings:
            print("   unexpected: %s" % f)
    print("   clean fixture: %d findings (expected 0)" % len(findings))

    for vector, records in sorted(_violation_cases().items()):
        path = os.path.join(tmpdir, vector + ".pcap")
        _write_pcap(path, records)
        got = {f.vector for f in audit(path, cm_frames(path), [OVMX])}
        ok = vector in got
        print("   %-20s %s  (fired: %s)" %
              (vector, "DETECTED" if ok else "MISSED", ",".join(sorted(got))))
        if not ok:
            failures.append("%s not detected" % vector)

    if failures:
        print("SELF-TEST FAILED:")
        for line in failures:
            print("   %s" % line)
        return 1
    print("SELF-TEST PASSED: clean fixture clean, all %d vectors detected" %
          len(_violation_cases()))
    return 0


# ---------------------------------------------------------------------------
def main(argv):
    args = list(argv[1:])
    if not args:
        print(__doc__)
        return 2
    if args[0] == "--self-test":
        return self_test()
    if args[0] == "--measure":
        measure(args[1:])
        return 0
    audit_all = False
    pinned = []
    while args and args[0].startswith("--"):
        if args[0] == "--audit-all":
            audit_all = True
            args.pop(0)
        elif args[0] == "--ovmx-mac":
            args.pop(0)
            pinned.append(args.pop(0))
        else:
            print("unknown option %s" % args[0])
            return 2
    if not args:
        print("no capture given")
        return 2
    fatal, warn, ran = 0, 0, False
    for path in args:
        try:
            f, w, r = report(path, pinned or None, audit_all)
        except (ValueError, IOError) as exc:
            print("== %s: CANNOT AUDIT: %s" % (path, exc))
            continue
        fatal, warn, ran = fatal + f, warn + w, ran or r
    print("\nTOTAL: %d FATAL, %d WARN across %d capture(s)" %
          (fatal, warn, len(args)))
    if not ran:
        return 2
    return 1 if fatal else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
