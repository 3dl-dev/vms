#!/usr/bin/env python3
"""dlm_queued_answer_census.py -- what a REAL master answers to a cat-0x02
op-0x01 LOCK REQUEST IT QUEUED, cell by cell (rd vms-85f).

usage: dlm_queued_answer_census.py <pcap|dir> ...     the per-cell census
       dlm_queued_answer_census.py --selftest <repo>  pin the headline figures

WHY THIS EXISTS. OVMX did not answer a queued cross-node op-0x01 AT ALL. A real
OpenVMS VAX V7.3 node's $ENQW on a resource OVMX masters and holds at a
conflicting mode therefore had nothing to wait on but silence: its process sat
in RWSCS and `STOP` could not complete it, measured on real hardware
2026-10-21 20:48-20:52 UTC (tests/lab/captures/vms-3ac-promote-20261021, "What
this fire ALSO found"). The arm's own reason for the silence was written in
src/kernel-core/vms_dlm_scs.c: "a queued first ENQ (op 0x01) has master-side
fields no capture lets this node fill (body[40:44] reads as the master's own S0
address)". This census tests that reason against the corpus, and it is TRUE FOR
A SUB-LOCK REQUEST AND FALSE FOR A ROOT ONE -- which is the whole answer:

  - a real master DOES answer a queued op-0x01, at once, with the request
    echoed, the category made a response and the outcome byte body[34] = 0xfb;
  - when the request names NO PARENT (body[36:44] all zero -- a root $ENQ, which
    is what the ci.6 evacuation's workload lock is), the answer leaves
    body[36:44] zero too. There is no master-side cell in it this executive
    cannot hold;
  - when the request DOES name a parent, the answer overwrites body[40:44] with
    a value whose high byte is 0x87 -- an S0 address, the master's own kernel
    memory. That one OVMX cannot and must not mint, so a queued SUB-lock request
    stays the honest, counted silence until a driven arm grounds the cell.

WHAT IT REPORTS. The answer-status distribution for every 82/01 in the corpus
(by sender class), then, over REAL -> REAL queued pairs only: which body offsets
the answer REWRITES, split into the ROOT and the SUB subsets, and the value sets
of the rewritten cells. Finally the same reading of the GRANTED answers
(body[34] = 0xfa), because the contrast is what tells a rewritten cell's queued
form from its granted one (body[28]: 0x10 granted, 0x14/0x54 queued).

IT MAKES NO CLAIM ABOUT WHAT A CELL MEANS. In particular it does NOT say what
selects 0x14 from 0x54 at body[28], nor what the small number at body[52] counts.
Rule 8 forbids inferring an unpublished algorithm; a value OVMX must write with
no grounded determinant is written INSIDE the measured envelope and recorded as
a deviation row in tests/cluster/conformance/frame-grounding.tsv, never guessed.

PAIRING IS BY TRANSACTION, not by time or order: a request's body[4:8] token and
the (src,dst) pair it travelled on, the same rule remaster_census.py uses.
Frames are classified by remaster_census's SCS-MSG discriminators, so a frame
counted here is a frame the shipping codec also classifies as one.

THE CORPUS. The whole of tests/lab/captures, `.pcap` and `.pcap.gz` alike,
through the shared walker (remaster_census.walk_inputs). This tool was written
before that walker read `.pcap.gz` and carried its own decompressing wrapper;
rd vms-e80 moved the gzip handling into the walker, where every census gets it,
and the wrapper is gone. The plain-.pcap-only figure is still pinned below --
the shared walker's old view loses 71 of the 110 pairs -- so the difference
stays on the record.

CLEAN-ROOM: reads captured frames only. No VSI/HPE source, no binaries, no
recomputation of an unpublished algorithm.
"""
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import remaster_census as rc  # noqa: E402

BODY_LEN = 132
REQ_CAT, ANS_CAT, ENQ_OP = 0x02, 0x82, 0x01
QUEUED, GRANTED = 0xfb, 0xfa
PARENT_SPAN = slice(36, 44)      # (own parent lkid, parent's master lkid)

# The OUIs a real DEC/Digital interface sends from; everything else in this
# corpus is an OVMX node. Same split as dlm_deq_census.py.
REAL_OUI = ('08002b', 'aa0004')


def is_real(mac):
    return mac[:3].hex() in REAL_OUI


def plain_only(paths):
    """The subset of a walk that is an uncompressed `.pcap`.

    What the shared walker saw before rd vms-e80 -- kept so the selftest can
    still pin how much of the evidence that view lost.
    """
    return [p for p in paths if not p.endswith('.gz')]


def pair_one_capture(path, status):
    """[(request_body, answer_body, req_src, ans_src)] for one capture.

    An op-0x01 request is held under (src, dst, token) and claimed by the 82/01
    whose (dst, src, token) matches it -- the transaction, not the order.
    """
    pairs, pending = [], {}
    for _idx, src, dst, body in rc.sca_bodies_with_src(path):
        if len(body) < BODY_LEN:
            continue
        if body[8] == REQ_CAT and body[9] == ENQ_OP and body[4:8] != b'\0' * 4:
            pending[(src, dst, bytes(body[4:8]))] = (bytes(body), src)
        elif body[8] == ANS_CAT and body[9] == ENQ_OP and body[34] == status:
            key = (dst, src, bytes(body[4:8]))
            if key in pending:
                req, rsrc = pending.pop(key)
                pairs.append((req, bytes(body), rsrc, src))
    return pairs


def real_pairs(paths, status):
    """Every REAL -> REAL (request, answer) pair whose answer carries `status`."""
    out = []
    for p in paths:
        for req, ans, rsrc, asrc in pair_one_capture(p, status):
            if is_real(rsrc) and is_real(asrc):
                out.append((p, req, ans, rsrc, asrc))
    return out


def split_root_sub(pairs):
    """(root, sub): the requests that name no parent, and the ones that do."""
    root = [x for x in pairs if x[1][PARENT_SPAN] == b'\0' * 8]
    sub = [x for x in pairs if x[1][PARENT_SPAN] != b'\0' * 8]
    return root, sub


def rewritten(pairs):
    """Offsets where ANY answer differs from its own request."""
    return [o for o in range(BODY_LEN)
            if any(req[o] != ans[o] for _p, req, ans, _r, _a in pairs)]


def answer_status_table(paths):
    """Counter of (sender class, body[34]) over every 82/01 in the corpus."""
    tab = collections.Counter()
    for p in paths:
        for _idx, src, _dst, body in rc.sca_bodies_with_src(p):
            if len(body) >= BODY_LEN and body[8] == ANS_CAT and \
               body[9] == ENQ_OP:
                tab[('real' if is_real(src) else 'ovmx', body[34])] += 1
    return tab


def ovmx_queued_answers(paths):
    """Every cat-0x82 op-0x01 QUEUED answer an OVMX node emitted, as bodies."""
    out = []
    for p in paths:
        for _idx, src, _dst, body in rc.sca_bodies_with_src(p):
            if len(body) >= BODY_LEN and body[8] == ANS_CAT and \
               body[9] == ENQ_OP and body[34] == QUEUED and not is_real(src):
                out.append(bytes(body))
    return out


def print_status_table(tab):
    print("=== every cat-0x82 op-0x01 in the corpus, by sender and body[34]")
    for (cls, st), n in sorted(tab.items(), key=lambda kv: -kv[1]):
        print("    %-5s body[34]=%02x  %d" % (cls, st, n))


def print_rewrite_report(pairs, label):
    print("\n=== %s: n=%d  offsets the answer REWRITES" % (label, len(pairs)))
    if not pairs:
        return
    for off in rewritten(pairs):
        changed = [(req[off], ans[off]) for _p, req, ans, _r, _a in pairs
                   if req[off] != ans[off]]
        vals = collections.Counter(ans[off] for _p, _q, ans, _r, _a in pairs)
        print("  body[%3d] (abs %3d) rewritten %3d/%d -> %s"
              % (off, 72 + off, len(changed), len(pairs),
                 ", ".join("%02x:%d" % (v, n) for v, n in vals.most_common(6))))


def print_topology(pairs, label):
    print("\n=== %s: the arrangements it was measured in" % label)
    print("    masters (answer senders): %s"
          % collections.Counter(a.hex() for *_x, a in pairs).most_common())
    print("    requesters:               %s"
          % collections.Counter(r.hex() for *_x, r, _a in pairs).most_common())
    print("    captures:                 %s"
          % collections.Counter(os.path.basename(os.path.dirname(p))
                                for p, *_x in pairs).most_common())
    print("    requested modes body[30]: %s"
          % collections.Counter(req[30] for _p, req, *_x in pairs).most_common())
    names = collections.Counter(
        bytes(req[48:48 + req[47]]).decode('latin1', 'replace')
        for _p, req, *_x in pairs if req[47] <= 31)
    print("    resource names:           %s" % names.most_common())


def report(paths):
    print_status_table(answer_status_table(paths))
    q = real_pairs(paths, QUEUED)
    root, sub = split_root_sub(q)
    print("\nREAL -> REAL queued (0xfb) pairs: %d  (root %d, sub-lock %d)"
          % (len(q), len(root), len(sub)))
    print_rewrite_report(root, "QUEUED answer, ROOT request")
    print_topology(root, "QUEUED answer, ROOT request")
    print_rewrite_report(sub, "QUEUED answer, SUB-LOCK request")
    print("\n    of the %d sub-lock answers, %d carry an S0 address at "
          "body[40:44] (high byte 0x87) -- the master's own memory"
          % (len(sub), sum(1 for _p, _q, a, _r, _s in sub if a[43] == 0x87)))
    g = real_pairs(paths, GRANTED)
    print_rewrite_report(g, "GRANTED answer (0xfa), for contrast")


# ---------------------------------------------------------------------------
# --selftest: every figure quoted in vms_cluster_codec_dlm.h's queued-answer
# note and in the dlm_enq_ans_queued grounding rows, re-measured.
# ---------------------------------------------------------------------------
def expect(ok, what):
    print("%s %s" % ("ok  " if ok else "FAIL", what))
    return 0 if ok else 1


def selftest(repo):
    caps = os.path.join(repo, 'tests', 'lab', 'captures')
    paths = rc.walk_inputs([caps])
    pcap_only = plain_only(paths)
    bad = 0
    q = real_pairs(paths, QUEUED)
    root, sub = split_root_sub(q)
    # 2026-10-22, rd vms-5cd's FIX FIRE added the 111th pair and the 18th ROOT
    # one, and it is the most useful kind of addition: a THIRD real master
    # (VAX2), holding a remote CR for an OVMX node on a tree it masters,
    # answered a real VAX's conflicting EX 0xfb QUEUED -- the same decisive
    # cell OVMX writes, from a direction the corpus had never held
    # (tests/lab/captures/vms-5cd-fixfire-20261022, f9378). Every RULE below
    # survived it unchanged: the same 15 rewritten offsets, the same record,
    # the same echoed handle.
    bad += expect(len(q) == 111, "111 real->real queued op-0x01 pairs (got %d)"
                  % len(q))
    bad += expect(len(root) == 18, "18 of them name no parent -- ROOT (got %d)"
                  % len(root))
    bad += expect(len(sub) == 93, "93 name a parent -- SUB-LOCK (got %d)"
                  % len(sub))
    bad += expect(len(real_pairs(pcap_only, QUEUED)) == 39,
                  "39 of the 110 are in a plain .pcap (the shared walker's view)")

    # The ROOT answer's shape: exactly these offsets are rewritten.
    rw = rewritten(root)
    want = [0, 1, 2, 3, 8, 20, 21, 22, 23, 28, 29, 32, 34, 52, 53]
    bad += expect(rw == want, "the ROOT answer rewrites exactly %s (got %s)"
                  % (want, rw))
    bad += expect(all(a[8] == 0x82 for _p, _q, a, _r, _s in root),
                  "body[8] = 0x82 on all 17: the category is made a response")
    bad += expect(all(a[9] == 0x01 for _p, _q, a, _r, _s in root),
                  "body[9] stays 0x01: an ENQ is answered as an ENQ")
    bad += expect(all(a[32:36] == bytes((0, 0, QUEUED, 0))
                      for _p, _q, a, _r, _s in root),
                  "the record at body[32:36] is 00 00 fb 00 on all 17")
    bad += expect(all(a[24:28] == r[24:28] for _p, r, a, _x, _y in root),
                  "body[24:28] is NOT rewritten: the requester's own handle, "
                  "echoed -- what it correlates by")
    bad += expect(all(a[30] == r[30] for _p, r, a, _x, _y in root),
                  "body[30] is NOT rewritten: the requested mode rides back")
    bad += expect(all(a[PARENT_SPAN] == b'\0' * 8 for _p, _q, a, _r, _s in root),
                  "body[36:44] stays ZERO on all 17: a root answer names no "
                  "parent, and has no cell this executive cannot hold")
    b28 = collections.Counter(a[28] for _p, _q, a, _r, _s in root)
    bad += expect(sorted(b28) == [0x14, 0x54] and b28[0x14] == 9 and
                  b28[0x54] == 9,
                  "body[28] is 0x14 (9) or 0x54 (9) -- two forms, no grounded "
                  "determinant (got %s)" % dict(b28))
    b29 = collections.Counter(a[29] for _p, _q, a, _r, _s in root)
    bad += expect(sorted(b29) == [0x00, 0x02],
                  "body[29] is 0x00 or 0x02 (got %s)" % dict(b29))
    bad += expect(all(a[53] == 0 for _p, _q, a, _r, _s in root),
                  "body[53] is 0 on all 17")
    b52 = {a[52] for _p, _q, a, _r, _s in root}
    bad += expect(b52 == {0, 1, 2, 3, 4},
                  "body[52] is a small number 0..4 over the 17, with no "
                  "request cell that predicts it (got %s)" % sorted(b52))

    # The arrangements: this is not one master answering one requester.
    bad += expect(len({a.hex() for *_x, a in root}) == 3,
                  "measured at 3 distinct masters")
    bad += expect(len({r.hex() for *_x, r, _a in root}) == 8,
                  "from 8 distinct requesters")
    bad += expect(len({os.path.basename(os.path.dirname(p))
                       for p, *_x in root}) == 8,
                  "in 8 distinct captures")
    bad += expect({req[30] for _p, req, *_x in root} == {4, 5},
                  "at 2 requested modes (PW and EX)")

    # The SUB-lock cell this executive must not mint.
    bad += expect(sum(1 for _p, _q, a, _r, _s in sub if a[43] == 0x87) == 93,
                  "all 93 SUB-lock answers carry an S0 address (high byte "
                  "0x87) at body[40:44] -- the master's own memory")
    bad += expect(all(a[PARENT_SPAN][:4] == r[PARENT_SPAN][:4]
                      for _p, r, a, _x, _y in sub),
                  "...while body[36:40], the requester's own parent handle, "
                  "is echoed unchanged on all 93")

    # The contrast that tells a queued answer from a granted one.
    g = real_pairs(paths, GRANTED)
    b28g = collections.Counter(a[28] for _p, _q, a, _r, _s in g)
    bad += expect(len(g) > 1000 and sorted(b28g) == [0x10],
                  "body[28] is 0x10 on every one of the %d GRANTED answers, "
                  "and never 0x14/0x54 (got %s)" % (len(g), dict(b28g)))
    bad += expect(all(a[32:36] == bytes((1, 0, GRANTED, 0))
                      for _p, _q, a, _r, _s in g),
                  "and their record at body[32:36] is 01 00 fa 00")
    # 2026-10-22: THIS CONTROL IS NOW A MEASUREMENT, NOT A SILENCE.
    #
    # It used to assert that no OVMX node had ever emitted a queued answer --
    # "the silence this item is about". The rd vms-85f fire ended that silence
    # on a real cluster: two OVMX-emitted 0xfb answers reached a real V7.3's
    # lock manager, which accepted them (its process left RWSCS for LEF and
    # STOP completed it) -- tests/lab/captures/vms-85f-queued-answer-20261021.
    #
    # So the control keeps its teeth by asserting WHAT they are instead of that
    # they are absent: every OVMX-emitted queued answer in the whole corpus
    # must carry the queue bit at body[28], the 0xfb status, and a master
    # handle that is non-zero and is NOT the requester's own -- the minted-value
    # class that bugchecked two lab VAXes. A malformed one cannot slip in.
    ovmx_q = ovmx_queued_answers(paths)
    bad += expect(len(ovmx_q) == 4,
                  "OVMX-emitted queued answers in the corpus: %d (two from the "
                  "rd vms-85f fire, two from the rd vms-5cd fix fire -- the "
                  "latter the answers that replaced a DOUBLE GRANT)" % len(ovmx_q))
    bad += expect(all(a[28] & 0x04 for a in ovmx_q),
                  "every one carries the queue bit 0x04 at body[28]")
    bad += expect(all(a[20:24] != bytes(4) and a[20:24] != a[24:28]
                      for a in ovmx_q),
                  "...and a master handle that is neither zero nor the "
                  "requester's own")
    print("\n%s" % ("selftest: all figures hold" if bad == 0
                    else "selftest: %d FAILED" % bad))
    return 1 if bad else 0


def main(argv):
    if len(argv) >= 3 and argv[1] == '--selftest':
        return selftest(argv[2])
    if len(argv) < 2:
        print(__doc__)
        return 2
    report(rc.walk_inputs(argv[1:]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
