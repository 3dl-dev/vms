#!/usr/bin/env python3
"""check_heldout_run.py -- score the driven held-out run (rd vms-c6e leg 2).

Two jobs, and they are deliberately in one file so the lab cannot score a
capture with a prediction file that was not the one committed beforehand:

  --verify   STATIC. predicted.tsv is exactly refhash.py over names.tsv, the
             generator's four artifacts are what the generator writes, and
             refhash.py still reproduces real VMS wire values (the anchor set
             below, every byte of it captured from a real OpenVMS VAX). Runs
             with no pcap and no lab. This is the ctest.

  --pcap P   SCORING. Reads every cat-0x02 op-0x01/op-0x0d ROOT request a real
             (non-OVMX) VAX sent in P, and sorts each against predicted.tsv:

               MATCH          the triple was pre-registered and the wire value
                              is the predicted one
               MISMATCH       the triple was pre-registered and the wire value
                              is NOT the predicted one   <- the only failure
               UNREGISTERED   the wire carried a (name, mode, group) the run
                              recipe did not pre-register -- reported with what
                              the function says for it, NEVER scored as a win
               ABSENT         a pre-registered triple never appeared on the wire

A MISMATCH is the result that would retire the function. UNREGISTERED is a
finding about the mode/group semantics of a flavour, not about the hash;
ABSENT means the lab run did not drive that name (wrong privilege, wrong pass,
or the lookup never left the node).
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcapio  # noqa: E402
import refhash  # noqa: E402
import gen_heldout_run as gen  # noqa: E402

# refhash.py's anchor set: real values real OpenVMS VAX nodes put on the wire,
# from tests/cluster/fixtures/dlm_hash_corpus.tsv (which records the pcap and
# frame each came from). These pin this module to measured VMS behaviour
# independently of the C -- if someone edits a constant in refhash.py to make a
# lab run "pass", these go red first.
ANCHORS = (
    # (name, mode, group, value)
    (b"DLMTA", 3, 0, 0x00336FE3),
    (b"LMF$DPD", 0, 0, 0x47C461DF),
    (b"OVMXBLK2", 3, 0, 0x9F13FDE7),
    (b"JBC$VAX2\x00\x00", 3, 1, 0xC47EC6C5),
    (b"SYS$_VAX2$MUA0:", 0, 0, 0xB53B9F0C),
    (b"LMF$SMM1_\x01\x00\x01\x00", 0, 0, 0x541F7AB8),
    (b"F11B$aSYSDSK1     \x2a\x00\x00\x00", 0, 0, 0x48501B6D),
    (b"DMT$_$2$DUA1:", 1, 0, 0xFCCDECD3),
)

TRUSTED_OPS = (pcapio.DLM_WIREOP_ENQ, pcapio.DLM_WIREOP_REBUILD)


def read_predicted(path):
    """(name_hex, mode, group) -> predicted value."""
    out = {}
    with open(path) as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line or line.startswith("#") or line.startswith("name_hex"):
                continue
            f = line.split("\t")
            out[(f[0], int(f[2]), int(f[3]))] = int(f[4], 16)
    return out


def check_anchors(fail):
    for name, mode, group, value in ANCHORS:
        got = refhash.name_hash(name, mode, group)
        if got != value:
            fail("anchor %r mode=%d group=%d: refhash says %08x, the real VAX "
                 "wire says %08x" % (name, mode, group, got, value))


def check_predictions(outdir, fail):
    names = gen.name_set()
    want = {}
    for name, mode, group in gen.pre_registered(names):
        want[(name.hex(), mode, group)] = refhash.name_hash(name, mode, group)
    got = read_predicted(os.path.join(outdir, "predicted.tsv"))
    if got != want:
        fail("predicted.tsv is not refhash.py over names.tsv (%d committed "
             "rows, %d computed)" % (len(got), len(want)))
    return len(want)


def check_generator(outdir, fail):
    if gen.main(["--outdir", outdir, "--check"]) != 0:
        fail("the generator's artifacts have drifted from the generator")


def cmd_verify(outdir):
    failures = []
    check_anchors(failures.append)
    check_generator(outdir, failures.append)
    n = check_predictions(outdir, failures.append)
    for m in failures:
        sys.stderr.write("FAIL %s\n" % m)
    print("%d anchors, %d pre-registered predictions: %s"
          % (len(ANCHORS), n, "FAILED" if failures else "OK"))
    return 1 if failures else 0


def wire_rows(paths, exclude_prefix):
    """Yield (name, mode, group, value, op, sender) for trusted VAX requests."""
    macs = {}
    for path in paths:
        for _, frame in pcapio.frames(path):
            if pcapio.is_sca(frame):
                nm = pcapio.disc_node_name(frame)
                if nm:
                    macs.setdefault(pcapio.src_mac(frame), set()).add(nm)
    for path in paths:
        for _, frame in pcapio.frames(path):
            if not pcapio.is_sca(frame):
                continue
            body = pcapio.sysap_body(frame)
            if body is None:
                continue
            hit = pcapio.root_named_request(body, TRUSTED_OPS)
            if hit is None:
                continue
            names = macs.get(pcapio.src_mac(frame))
            if not names or len(names) > 1:
                continue
            sender = next(iter(names))
            if sender.upper().startswith(exclude_prefix.upper()):
                continue
            yield hit[0], hit[1], hit[2], hit[3], body[pcapio.OFB_OP], sender


def cmd_score(paths, outdir, exclude_prefix):
    predicted = read_predicted(os.path.join(outdir, "predicted.tsv"))
    seen, buckets = {}, collections.Counter()
    unreg, bad = {}, []
    for name, mode, group, value, op, sender in wire_rows(paths, exclude_prefix):
        key = (name.hex(), mode, group)
        if key in predicted:
            if value == predicted[key]:
                if key not in seen:
                    buckets["MATCH"] += 1
                seen[key] = value
            else:
                bad.append((name, mode, group, predicted[key], value, op, sender))
        elif key not in unreg:
            unreg[key] = (name, mode, group, value,
                          refhash.name_hash(name, mode, group), op, sender)
    absent = [k for k in predicted if k not in seen
              and not any(k == (n.hex(), m, g) for n, m, g, _, _, _, _ in bad)]

    print("pre-registered triples : %d" % len(predicted))
    print("  MATCH                : %d" % buckets["MATCH"])
    print("  MISMATCH             : %d" % len(bad))
    print("  ABSENT from the wire : %d" % len(absent))
    print("unregistered triples   : %d" % len(unreg))
    for n, m, g, want, got, op, s in bad:
        print("  MISMATCH %r mode=%d group=%d op=0x%02x from %s: predicted "
              "%08x, wire %08x" % (n, m, g, op, s, want, got))
    for n, m, g, got, recomputed, op, s in unreg.values():
        tag = "value matches the function" if got == recomputed else \
              "value does NOT match the function"
        print("  UNREGISTERED %r mode=%d group=%d op=0x%02x from %s: wire "
              "%08x (%s)" % (n, m, g, op, s, got, tag))
    for k in sorted(absent):
        print("  ABSENT %s mode=%d group=%d" % k)
    return 1 if bad else 0


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    default = os.path.normpath(os.path.join(
        here, "..", "..", "..", "tests", "lab", "captures",
        "vms-c6e-heldout-20261008"))
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rundir", default=default)
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--pcap", action="append", default=[])
    ap.add_argument("--exclude-prefix", default="OVMX")
    args = ap.parse_args(argv)

    if args.verify or not args.pcap:
        rc = cmd_verify(args.rundir)
        if not args.pcap:
            return rc
        if rc:
            return rc
    return cmd_score(args.pcap, args.rundir, args.exclude_prefix)


if __name__ == "__main__":
    sys.exit(main())
