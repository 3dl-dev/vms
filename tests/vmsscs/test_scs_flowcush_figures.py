#!/usr/bin/env python3
"""
test_scs_flowcush_figures.py -- the type-8 / SCSFLOWCUSH prose must match the wire.

vms-f03. tools/scs_flowcush_measure.py re-derives every figure of the
SCSFLOWCUSH dose-response experiment from five lab-2 captures, but those are
host-only (83 MB, not in git), so ctest cannot count on them. This gate runs
both arms of the vms-371 contract:

  * host-independent -- take the EXPECTED table straight out of the measurement
    script and assert every figure still appears in
    docs/cluster-protocol-spec.md and docs/design-mscp-direction.md;
  * on a host that HAS the captures -- call scs_flowcush_measure.rederive() and
    red on any figure the packets no longer support.

Green on a lab host therefore means wire == EXPECTED == prose. On a host without
the captures scs_wire prints a banner saying the wire was NOT read.

It also pins the three claims a reviewer must not be able to lose silently,
because each one is what stops this finding from being a name guessed into the
spec (the vms-c11 failure mode):

  1. type 8 is ABSENT at the default cushion -- bracketed on BOTH sides;
  2. the forward conservation identity, which is the actual decode of [48:50];
  3. the honest limits: the handle-swap was NOT testable on this lab, and
     type 9 is NOT named, because ch.2 documents no response.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import scs_wire                                                    # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
# Overrides exist only so a mutation battery can point this gate at a scratch
# copy of the tree. Nothing in the repo sets them.
SPEC = os.environ.get("OVMX_SCS_FLOWCUSH_SPEC",
                      os.path.join(ROOT, "docs", "cluster-protocol-spec.md"))
DESIGN = os.environ.get("OVMX_SCS_FLOWCUSH_DESIGN",
                        os.path.join(ROOT, "docs", "design-mscp-direction.md"))
MEASURE = os.environ.get("OVMX_SCS_FLOWCUSH_MEASURE",
                         os.path.join(ROOT, "tools", "scs_flowcush_measure.py"))

failures = []
checks = 0


def check(cond, msg):
    global checks
    checks += 1
    if not cond:
        failures.append(msg)


def num(n):
    """Match an integer in either rendering: '64305' or '64 305' (thin space).

    Built per-number rather than by collapsing digit-space-digit across the
    whole file -- that welds unrelated adjacent numbers together and silently
    breaks every adjacency check downstream (see test_scs_credit_figures.py).
    """
    s = str(n)
    head = s[: len(s) % 3 or 3]
    groups = [s[i:i + 3] for i in range(len(head), len(s), 3)]
    return r"\b" + r"[   ]?".join([head] + groups) + r"\b"


def plain(s):
    return s.replace("*", "").replace("`", "")


def main():
    mod = scs_wire.load_source(MEASURE, "scs_flowcush_measure")
    expected = mod.EXPECTED
    spec = open(SPEC).read()
    design = open(DESIGN).read()
    docs = {"cluster-protocol-spec.md": spec, "design-mscp-direction.md": design}

    # --- 1. the dose-response table ---------------------------------------
    # Each (cushion, type-8 count) must appear ADJACENT and IN ORDER, so that
    # swapping two rows -- the mutation that would invert the finding -- reds.
    for tag, (cush, n8) in expected["dose_response"].items():
        pair = re.compile(r"%s.{0,80}?%s" % (num(cush), num(n8)), re.S)
        for name, text in docs.items():
            check(pair.search(text) is not None,
                  "%s: dose-response row for %s (SCSFLOWCUSH %d -> %d type-8 "
                  "frames) is missing or has drifted" % (name, tag, cush, n8))

    # --- 2. the decode: forward conservation ------------------------------
    for tag, (v2msgs, c10, c8, _d) in expected["conservation_forward"].items():
        if not c8:
            continue                      # the cushion<=1 runs carry no type 8
        trip = re.compile(r"%s.{0,60}?%s.{0,60}?%s" % (num(c10), num(c8), num(v2msgs)),
                          re.S)
        for name, text in docs.items():
            check(trip.search(text) is not None,
                  "%s: the conservation identity for %s "
                  "(%d + %d == %d) is missing or has drifted"
                  % (name, tag, c10, c8, v2msgs))

    # --- 3. the value dose-response ---------------------------------------
    for tag, mean in expected["mean_credit_per_type8"].items():
        for name, text in docs.items():
            check(re.search(re.escape("%.4f" % mean), text) is not None,
                  "%s: mean credit per type-8 for %s (%.4f) is missing"
                  % (name, tag, mean))

    # --- 4. pairing and the echo ------------------------------------------
    for tag, (matched, unmatched, unanswered) in expected["pairing"].items():
        check(re.search(num(matched), spec) is not None,
              "cluster-protocol-spec.md: the %s pairing count %d is missing"
              % (tag, matched))
        check((unmatched, unanswered) == (0, 0),
              "EXPECTED: %s no longer reports a perfect pairing; the prose "
              "claims one" % tag)

    # --- 5. the claims that must not be lost -------------------------------
    # SCOPED to the section that makes the claim, never the whole file. An
    # unscoped search lets an unrelated mention elsewhere mask the deletion:
    # verified -- a first version of this gate passed a mutant that renamed
    # type 9 in (1g), because the (1c) bullet still said "unnamed" 60 lines up.
    blocks = {}
    m = re.search(r"\*\*\(1e\)(.*?)\*\*\(2\) SCS\$DIR_LOOKUP", spec, re.S)
    check(m is not None, "cluster-protocol-spec.md: section 4(h)(1g) is gone")
    blocks["cluster-protocol-spec.md"] = m.group(1) if m else ""
    m = re.search(r"\*\*RAN 2026-08-06(.*?)### 1\.4", design, re.S)
    check(m is not None,
          "design-mscp-direction.md: the sec 1.3 experiment-result block is gone")
    blocks["design-mscp-direction.md"] = m.group(1) if m else ""

    for name, block in blocks.items():
        flat = plain(block)
        check(re.search(r"(?i)SCSFLOWCUSH", flat) is not None,
              "%s: SCSFLOWCUSH -- the knob the whole experiment turns on -- "
              "is not named in the result block" % name)
        check(re.search(r"(?i)2-44", flat) is not None,
              "%s: the p. 2-44 page cite for the special credit message is gone "
              "(Rule 8: every claim carries its public-doc cite)" % name)
        check("tools/scs_flowcush_measure.py" in block,
              "%s: no pointer to the re-derivation script" % name)
        # Honest limits -- the vms-c11 guardrails for this finding. Each is
        # pinned by a FIGURE where one exists, so it cannot be paraphrased away.
        check(re.search(r"(?i)handle", flat) is not None
              and re.search(r"(?i)(not tested|could not be tested|undetectable"
                            r"|identical)", flat) is not None,
              "%s: the handle-swap limitation (both handle fields carry the SAME "
              "value on this lab, so the swap was NOT tested) has been dropped"
              % name)
        check(re.search(r"(?i)type\s*9\s+is\s+(deliberately\s+)?NOT\s+named", flat)
              is not None,
              "%s: the explicit 'type 9 is NOT named' commitment is gone -- if a "
              "name was added, ch.2 p. 2-44 documents no response and this is the "
              "vms-c11 pattern" % name)
        check(re.search(r"(?i)7\s?388", flat) is not None,
              "%s: the 7 388 credit-0 type-8 frames -- the observation that does "
              "NOT fit p. 2-44 -- have been dropped. Report the misfit." % name)

    # The negative control IS the finding: pin it to its frame counts, in the
    # spec, so 'zero' cannot quietly become 'few'.
    sblock = blocks["cluster-protocol-spec.md"]
    check(re.search(r"(?i)zero\s+type[- ]8\s+frames\s+at\s+the\s+default\s+cushion",
                    plain(sblock)) is not None,
          "cluster-protocol-spec.md: the negative control ('zero type-8 frames at "
          "the default cushion') is no longer stated in 4(h)(1g)")
    for n in (440367, 427958):
        check(re.search(num(n), sblock) is not None,
              "cluster-protocol-spec.md: the %d-frame bracket the negative control "
              "is measured over is missing" % n)

    # --- 6. THE WIRE ITSELF (vms-371) -------------------------------------
    # Sections 1-5 pin the PROSE to EXPECTED. This pins EXPECTED to the PACKETS.
    scs_wire.gate("scs_flowcush_figures", mod, mod.DEFAULT_CAPDIR, check)

    for f in failures:
        print("FAIL %s" % f)
    print("%d checks, %d failures" % (checks, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
