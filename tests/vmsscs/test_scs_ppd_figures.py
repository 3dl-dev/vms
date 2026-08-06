#!/usr/bin/env python3
"""
test_scs_ppd_figures.py -- vms-0fe: the port-layer decode must not drift, and
the "the VAX is REFUSING our DISCONNECT_REQ" reading must stay dead.

NEEDS NO CAPTURES AND NEEDS NO LAB. Everything it checks is prose-against-table.

WHAT vms-0fe ESTABLISHED, and why it needs a gate at all.

  `%PEA0, Inappropriate SCA Control Message - FLAGS/OPC/STATUS/PORT 00/22/00/DD`
  was carried for two items as evidence that a real VAX RECEIVES and REFUSES
  OVMX's DISCONNECT_REQ. It never meant that. vms-096 measured the SCA answer
  (10/10, sub-millisecond) and vms-0fe read the line off VMS's own shipped
  decode tables on a lab-2 VAX:

    HELP/MESSAGE                -> facility "Cluster Port Driver", and the
                                   documented effect is a PORT-layer VC close
    ANALYZE/ERROR_LOG           -> the quadruple is PPD$B_FLAGS / PPD$B_OPC /
                                   PPD$B_STATUS / PPD$B_PORT, and PPD$B_PORT
                                   renders as "REMOTE NODE # n."
    $SCSDEF (SYS$LIBRARY:LIB.MLB) -> SCS$S_PPD = 16 at SCS$B_PPD = -32, i.e. a
                                   SEPARATE 16-byte port header in front of the
                                   SCS header, whose message-type enum stops at
                                   SCS$C_APPL_DG = 11

  So OPC/22 is a PPD opcode and can never be an SCS message type. Every figure
  above is a constant somebody will be tempted to "tidy" or restate; each one is
  pinned here EXACTLY ONCE in the spec, because a second copy is how a drift in
  one copy hides (the defect that got the vms-6b3 gate rejected twice).

  It also pins the REPRODUCTION RESULT, which is a negative: four fresh lab-2
  runs, one of them a matched OVMX_NO_CLEAN_SHUTDOWN=1 control that put ZERO
  disconnect frames on the wire, and the anomalous line appeared in none of
  them. A negative that stops being written down turns back into a rumour.

THE QUARANTINE. The two dead sentences may be written down only inside a
PPD-REFUTED-BEGIN / PPD-REFUTED-END block (markers deliberately distinct from
the REFUTED-QUOTE-* markers test_scs_disc_figures.py owns, so the two gates
cannot interfere). Anywhere else -- spec, header, scsd.c -- they red, and the
match is by claim FAMILY (subject + the assertion, within one sentence) so a
reworded revival is caught too. The blocks are size-capped: "quarantine the
whole file" is not an escape hatch.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

SPEC = os.path.join(ROOT, "docs", "cluster-protocol-spec.md")
HDR = os.path.join(ROOT, "src", "vmsscs", "include", "scs_disc.h")
DAEMON = os.path.join(ROOT, "src", "vmsscs", "scsd.c")

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
    return cond


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


# ---------------------------------------------------------------------------
# 1. The spec block exists, is delimited, and is the ONLY home for the figures.
# ---------------------------------------------------------------------------
spec = read(SPEC)
BEGIN, END = "<!-- PPD-FIGURES-BEGIN -->", "<!-- PPD-FIGURES-END -->"
check(spec.count(BEGIN) == 1 and spec.count(END) == 1,
      f"cluster-protocol-spec.md must carry exactly one {BEGIN}/{END} pair "
      f"(found {spec.count(BEGIN)}/{spec.count(END)})")
block = ""
if spec.count(BEGIN) == 1 and spec.count(END) == 1:
    b = spec.index(BEGIN) + len(BEGIN)
    e = spec.index(END)
    check(b < e, "PPD-FIGURES-END appears before PPD-FIGURES-BEGIN")
    block = spec[b:e] if b < e else ""

# ---------------------------------------------------------------------------
# 2. THE FIGURES. Each must appear exactly once inside the block -- and, for the
#    ones that are structure rather than a lone number, exactly once in the
#    whole spec, so a "helpful" restatement elsewhere reds instead of diverging.
# ---------------------------------------------------------------------------
# (figure, human name, must-be-unique-in-the-whole-spec?)
FIGURES = [
    (r"`SCS\$S_PPD`\s*=\s*\*\*16\*\*", "SCS$S_PPD = 16 (PPD header size)", True),
    (r"`SCS\$B_PPD`\s*=\s*`-32`", "SCS$B_PPD = -32 (PPD header offset)", True),
    (r"`SCS\$W_MTYPE`\s*.12", "SCS$W_MTYPE = -12", True),
    (r"`PPD\$B_FLAGS`\s*/\s*`PPD\$B_OPC`\s*/\s*`PPD\$B_STATUS`\s*/\s*`PPD\$B_PORT`",
     "the four PPD field names, in printed order", True),
    (r"`REMOTE NODE # n\.`", "PPD$B_PORT renders as REMOTE NODE # n.", True),
    (r"APPL_DG 11", "the SCS message-type enum ends at APPL_DG 11", True),
    (r"`Cluster Port Driver`", "the facility name", True),
    (r"descending from `0xDE`", "the remote-node allocation rule", True),
]
for pattern, name, unique_in_spec in FIGURES:
    n_block = len(re.findall(pattern, block))
    check(n_block == 1,
          f"figure '{name}' must appear EXACTLY ONCE inside the PPD-FIGURES "
          f"block; found {n_block}")
    if unique_in_spec:
        n_spec = len(re.findall(pattern, spec))
        check(n_spec == 1,
              f"figure '{name}' must appear EXACTLY ONCE in the whole spec "
              f"(one copy per figure); found {n_spec}")

# The station-number -> SCSSYSTEMID pairs that ground PPD$B_PORT. These are the
# rows ANALYZE/ERROR_LOG produced; if one is edited the grounding is gone.
PAIRS = [("0xDE", "1026"), ("0xDD", "1602"), ("0xDC", "1603")]
for station, sysid in PAIRS:
    row = re.search(r"^\s*\|\s*`%s`.*$" % re.escape(station), block, re.M)
    check(row is not None,
          f"the PPD$B_PORT grounding table has no row for station {station}")
    if row:
        check(sysid in row.group(0),
              f"station {station} must still be paired with SCSSYSTEMID {sysid} "
              f"(row reads: {row.group(0).strip()[:120]})")

# ---------------------------------------------------------------------------
# 3. THE REPRODUCTION RESULT -- a negative, pinned so it cannot quietly lapse.
# ---------------------------------------------------------------------------
RUNS = ["0feA", "0feA1", "0feB1", "0feA2"]
for tag in RUNS:
    # must be a TABLE ROW, not merely a mention in the prose around it --
    # otherwise deleting the row and leaving one narrative reference passes.
    check(re.search(r"^\s*\|\s*`%s`\s*\|" % re.escape(tag), block, re.M)
          is not None,
          f"the reproduction table has lost the row for run {tag}")
check(re.search(r"appeared in none of the four", block, re.I) is not None,
      "the spec must still state that the anomalous line did NOT reproduce in "
      "the four vms-0fe runs -- a negative result that stops being written down "
      "turns back into a rumour")
check("OVMX_NO_CLEAN_SHUTDOWN=1" in block and "req-sent=0" in block,
      "the matched control must stay named WITH the evidence that it gated the "
      "wire (OVMX_NO_CLEAN_SHUTDOWN=1 / req-sent=0); a kill switch that is not "
      "shown to have suppressed anything proves nothing (guardrail 23)")

# The decode procedure for the one field that is still unknown.
check("PPD$B_OPC 22" in block and "ANALYZE/ERROR_LOG" in block,
      "the spec must keep the executable next step for OPC/22: reproduce, then "
      "read the symbolic name ANALYZE/ERROR_LOG prints under PPD$B_OPC 22")

# ---------------------------------------------------------------------------
# 4. THE QUARANTINE. The dead claims live only inside PPD-REFUTED-BEGIN/END.
# ---------------------------------------------------------------------------
QB, QE = "PPD-REFUTED-BEGIN", "PPD-REFUTED-END"
MAX_QUARANTINE_CHARS = 900


def quarantine_spans(text, doc_name):
    """Balanced, non-nested, size-capped quarantine blocks."""
    spans = []
    open_at = None
    for m in re.finditer("%s|%s" % (QB, QE), text):
        if m.group(0) == QB:
            check(open_at is None,
                  f"{doc_name}: nested {QB} at offset {m.start()} -- nesting is "
                  f"how a quarantine gets silently widened")
            open_at = m.end()
        else:
            check(open_at is not None,
                  f"{doc_name}: {QE} at offset {m.start()} with no open block")
            if open_at is not None:
                size = m.start() - open_at
                check(size <= MAX_QUARANTINE_CHARS,
                      f"{doc_name}: quarantine block is {size} chars "
                      f"(cap {MAX_QUARANTINE_CHARS}) -- quarantining a whole "
                      f"section is not an escape hatch")
                spans.append((open_at, m.start()))
            open_at = None
    check(open_at is None, f"{doc_name}: unterminated {QB}")
    return spans


def outside_quarantine(text, doc_name):
    out, last = [], 0
    for b, e in quarantine_spans(text, doc_name):
        out.append(text[last:b])
        last = e
    out.append(text[last:])
    return " ".join(out)


# Claim families, matched inside ONE sentence: subject + the assertion.
DEAD = [
    ("the refusal reading",
     re.compile(r"\b(?:VAX\d*|peer|port driver)\b[^.]{0,120}"
                r"\brecei(?:ve|ves|ving)\b[^.]{0,120}"
                r"\brefus(?:e|es|ed|ing)\b", re.I | re.S)),
    ("the never-answers reading",
     re.compile(r"(real VAX|peer|VAX1)[^.]{0,80}"
                r"(answers?\s+no\b|never\s+answers?)[^.]{0,60}DISCONNECT",
                re.I | re.S)),
]

for path, doc_name in ((SPEC, "cluster-protocol-spec.md"),
                       (HDR, "scs_disc.h"),
                       (DAEMON, "scsd.c")):
    body = outside_quarantine(read(path), doc_name)
    for sentence in re.split(r"(?<=[.!?])\s+", body):
        for claim_name, rx in DEAD:
            if rx.search(sentence):
                FAILURES.append(
                    f"{doc_name}: {claim_name} is asserted OUTSIDE a "
                    f"{QB}/{QE} block: {' '.join(sentence.split())[:160]}")

# Each source file must actually carry the correction, not merely lack the claim.
hdr = read(HDR)
check(hdr.count(QB) == 1 and "PPD$B_OPC" in hdr,
      "scs_disc.h must quarantine the old refusal reading exactly once AND "
      "carry the PPD decode that replaces it")
daemon = read(DAEMON)
check(daemon.count(QB) == 1 and "0feB1" in daemon,
      "scsd.c's shutdown-wait justification must quarantine the old "
      "never-answers claim exactly once AND cite the run that refutes it")

# ---------------------------------------------------------------------------
if FAILURES:
    print("test_scs_ppd_figures: FAIL (%d)" % len(FAILURES))
    for f in FAILURES:
        print("  - " + f)
    sys.exit(1)
print("test_scs_ppd_figures: PASS -- port-layer figures pinned, "
      "reproduction negative pinned, both dead claims quarantined")
