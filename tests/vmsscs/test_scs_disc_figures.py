#!/usr/bin/env python3
"""
test_scs_disc_figures.py -- vms-591: the disconnect prose must match the
measurement, and the refuted claim must stay dead.

NEEDS NO CAPTURES. It compares the figures written into
  src/vmsscs/include/scs_disc.h   (the pairing / population / matching-flag census)
  src/vmsscs/scsd.c               (the REQ->RSP latency that justifies the
                                   500 ms bounded shutdown wait)
  docs/cluster-protocol-spec.md   (CENSUS-D and CENSUS-E)
against the EXPECTED table in tools/cluster/scs_disc_measure.py, which is the
checked-in record of what the 47 lab captures measured. Only that script, on a
host with the captures, re-derives EXPECTED itself.

WHY THIS GATE EXISTS, in the specific terms of what went wrong.

  docs/cluster-protocol-spec.md sec 4(h)(1a) carried, from vms-dd5 until
  vms-591, an instruction to future agents built on a measurement that was
  wrong: that connection-control message types 5 and 7 appear on no capture we
  hold, and therefore "do not build a 5 or 7 frame on this section". Both types
  are on our wire in quantity. The error was a SAMPLING error -- every census
  had been restricted to the SCA length classes {62, 66, 110}, and both
  response messages are 58 bytes -- and nothing in ctest could catch it,
  because nothing in ctest read a census that included the 58-byte class.

  So this test does two things a comment cannot:

  (1) PARSES the figures out of all three documents and compares them field by
      field with EXPECTED. A single digit that drifts in any of them reds. The
      figures are carried on machine-readable `DISC-CENSUS-*` lines (header and
      scsd.c) and in tables introduced by `<!-- CENSUS-D/E:` markers (spec), so
      this is a parser and not a substring search: a figure that appears twice
      in a document would let a drift in one copy hide, which is exactly the
      defect that got the vms-6b3 gate rewritten. ONE COPY PER FIGURE.

  (2) QUARANTINES THE REFUTED CLAIM. The dead sentences may be written down
      only inside a `REFUTED-QUOTE-BEGIN` / `REFUTED-QUOTE-END` block in the
      spec. Anywhere else -- in the spec, in the header, in scsd.c -- they red.
      Matching is by CLAIM FAMILY (subject + the assertion of absence within
      one sentence), not by exact sentence, so a reworded revival is caught
      too. The quarantine block is itself size-capped, so "quarantine the whole
      section" is not an escape hatch.

AND SINCE vms-371, IT READS THE WIRE WHEN THE WIRE IS HERE. On a host with the
lab captures this gate calls `scs_disc_measure.rederive()` and reds on any
figure the packets no longer support, so "the documents still say what the
measurement said" is joined by "and the measurement still says it". On a host
without the captures it prints a banner saying the wire was NOT read -- loudly,
because the earlier silence is how `ctest -L scs` stayed 32/32 green while this
script was 23 checks red. See tests/vmsscs/scs_wire.py.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import scs_wire                                                    # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

HEADER = os.environ.get("OVMX_SCS_DISC_HEADER",
                        os.path.join(ROOT, "src/vmsscs/include/scs_disc.h"))
DAEMON = os.environ.get("OVMX_SCS_DISC_DAEMON",
                        os.path.join(ROOT, "src/vmsscs/scsd.c"))
SPEC = os.environ.get("OVMX_SCS_DISC_SPEC",
                      os.path.join(ROOT, "docs/cluster-protocol-spec.md"))
MEASURE = os.environ.get("OVMX_SCS_DISC_MEASURE",
                         os.path.join(ROOT, "tools/cluster/scs_disc_measure.py"))

failures = 0
checks = 0


def check(cond, msg):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print(f"  FAIL {msg}")


def read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


# --- EXPECTED, imported from the measure script ----------------------------
def load_measure():
    """Execute the measure script FROM SOURCE, never from cached bytecode.

    `__import__` (and importlib's source-file loader generally) validates its
    cached .pyc on (mtime-in-SECONDS, size). An edit that keeps the file the
    same length and lands in the same second as a previous run therefore
    silently reuses the OLD bytecode -- i.e. the OLD EXPECTED -- and this gate
    reads EXPECTED out of that module, so the mutation would survive.
    test_scs_reason_figures.py hit exactly that (one surviving mutant,
    `EXPECTED pcaps 25 -> 26`) and switched to compile()+exec(); this gate had
    not. Reading the source text every time removes the cache from the path.

    The loader now lives in scs_wire.load_source() so all six figures gates
    share ONE implementation and the mutation battery attacks one place.
    """
    # scs_disc_measure.py imports dissect_sca LAZILY, so nothing here needs
    # the dissector or a capture -- but keep its directory on sys.path so a
    # future eager import does not turn this gate red for the wrong reason.
    d = os.path.dirname(MEASURE)
    if d not in sys.path:
        sys.path.insert(0, d)
    return scs_wire.load_source(MEASURE, "scs_disc_measure")


MEASURE_MOD = load_measure()
EXPECTED = MEASURE_MOD.EXPECTED

header = read(HEADER)
daemon = read(DAEMON)
spec = read(SPEC)

print("test_scs_disc_figures: vms-591 disconnect census vs the prose")

# ===========================================================================
# 1. THE HEADER'S DISC-CENSUS-* LINES
# ===========================================================================

def one_per_line(text, pattern, what):
    """Every match, with a hard requirement that no key appears twice: a
    duplicated figure is how a drift in one copy hides."""
    rows = re.findall(pattern, text)
    return rows


caps = re.findall(r"DISC-CENSUS-CAPTURES:\s*(\d+)", header)
check(len(caps) == 1, f"header: expected exactly 1 DISC-CENSUS-CAPTURES line, got {len(caps)}")
if caps:
    check(int(caps[0]) == EXPECTED["n_captures"],
          f"header capture count {caps[0]} != EXPECTED {EXPECTED['n_captures']}")

pair_rows = one_per_line(
    header, r"DISC-CENSUS-PAIR:\s*(\d+)\s+(\d+)\s*->\s*(\d+)\s+(\d+)\s+n=(\d+)\s+pcaps=(\d+)",
    "pair")
got_pairs = {}
for rl, rm, sl, sm, n, p in pair_rows:
    key = (int(rl), int(rm))
    check(key not in got_pairs, f"header: DISC-CENSUS-PAIR for {key} appears more than once")
    got_pairs[key] = {"rsp": (int(sl), int(sm)), "frames": int(n), "pcaps": int(p)}
check(set(got_pairs) == set(EXPECTED["pairs"]),
      f"header pair rows {sorted(got_pairs)} != EXPECTED {sorted(EXPECTED['pairs'])}")
for key, want in EXPECTED["pairs"].items():
    got = got_pairs.get(key)
    if got is None:
        continue
    check(got["rsp"] == tuple(want["rsp"]),
          f"header pair {key}: response {got['rsp']} != EXPECTED {tuple(want['rsp'])}")
    check(got["frames"] == want["frames"],
          f"header pair {key}: n={got['frames']} != EXPECTED {want['frames']}")
    check(got["pcaps"] == want["pcaps"],
          f"header pair {key}: pcaps={got['pcaps']} != EXPECTED {want['pcaps']}")

pop_rows = one_per_line(header, r"DISC-CENSUS-POP:\s*(\d+)\s+(\d+)\s+n=(\d+)\s+pcaps=(\d+)", "pop")
got_pops = {}
for l, m, n, p in pop_rows:
    key = (int(l), int(m))
    check(key not in got_pops, f"header: DISC-CENSUS-POP for {key} appears more than once")
    got_pops[key] = {"frames": int(n), "pcaps": int(p)}
check(set(got_pops) == set(EXPECTED["populations"]),
      f"header population rows {sorted(got_pops)} != EXPECTED {sorted(EXPECTED['populations'])}")
for key, want in EXPECTED["populations"].items():
    got = got_pops.get(key)
    if got is None:
        continue
    check(got["frames"] == want["frames"],
          f"header population {key}: n={got['frames']} != EXPECTED {want['frames']}")
    check(got["pcaps"] == want["pcaps"],
          f"header population {key}: pcaps={got['pcaps']} != EXPECTED {want['pcaps']}")

match_rows = one_per_line(
    header, r"DISC-CENSUS-MATCH:\s*rank=(\d+)\s+value=0x([0-9a-fA-F]{4})\s+n=(\d+)\s+pcaps=(\d+)",
    "match")
got_match = {}
for r, v, n, p in match_rows:
    key = (int(r), int(v, 16))
    check(key not in got_match, f"header: DISC-CENSUS-MATCH for {key} appears more than once")
    got_match[key] = {"frames": int(n), "pcaps": int(p)}
check(set(got_match) == set(EXPECTED["match_flag"]),
      f"header match rows {sorted(got_match)} != EXPECTED {sorted(EXPECTED['match_flag'])}")
for key, want in EXPECTED["match_flag"].items():
    got = got_match.get(key)
    if got is None:
        continue
    check(got["frames"] == want["frames"],
          f"header match {key}: n={got['frames']} != EXPECTED {want['frames']}")
    check(got["pcaps"] == want["pcaps"],
          f"header match {key}: pcaps={got['pcaps']} != EXPECTED {want['pcaps']}")

resid = re.findall(r"DISC-CENSUS-MATCH-RESIDUALS:\s*(\d+)", header)
check(len(resid) == 1, f"header: expected 1 residual line, got {len(resid)}")
if resid:
    check(int(resid[0]) == EXPECTED["match_flag_residuals"],
          f"header residuals {resid[0]} != EXPECTED {EXPECTED['match_flag_residuals']}")

# ===========================================================================
# 2. THE DAEMON'S LATENCY FIGURES -- the shutdown timeout's whole justification
# ===========================================================================

lat_text = "\n".join(re.findall(r"DISC-CENSUS-LAT:(.*)", daemon))
check(lat_text.strip() != "", "scsd.c carries no DISC-CENSUS-LAT lines -- the "
                              "500 ms shutdown wait has no measurement behind it")
for field, want in EXPECTED["latency"].items():
    key = "over10ms" if field == "over_10ms" else field
    found = re.findall(rf"\b{re.escape(key)}=([0-9.]+)", lat_text)
    check(len(found) == 1,
          f"scsd.c: latency field '{key}' appears {len(found)} time(s) in the "
          f"DISC-CENSUS-LAT lines, expected exactly 1")
    if len(found) == 1:
        got = float(found[0]) if "." in found[0] else int(found[0])
        check(got == want, f"scsd.c latency {key}={got} != EXPECTED {want}")

# The bound itself must still be the one the prose argues for.
wait = re.findall(r"#define\s+SCSD_SHUTDOWN_WAIT_MS\s+(\d+)", daemon)
check(len(wait) == 1, "scsd.c does not define SCSD_SHUTDOWN_WAIT_MS exactly once")
if wait:
    check(int(wait[0]) / 1000.0 > EXPECTED["latency"]["max"] * 10,
          f"SCSD_SHUTDOWN_WAIT_MS={wait[0]} is not comfortably above the measured "
          f"max REQ->RSP latency of {EXPECTED['latency']['max']} s -- the prose "
          f"argues from a margin that no longer exists")

# ===========================================================================
# 3. THE SPEC'S CENSUS-D AND CENSUS-E TABLES
# ===========================================================================

def table_after(marker, text):
    i = text.find(marker)
    if i < 0:
        return []
    rows = []
    for line in text[i:].split("\n"):
        if line.startswith("|"):
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            rows.append(cells)
        elif rows:
            break
    return [r for r in rows if not all(set(c) <= set("-: ") for c in r)]


d_rows = table_after("<!-- CENSUS-D:", spec)
check(len(d_rows) >= 5, "spec: CENSUS-D table not found or too short")
spec_pairs = {}
for cells in d_rows[1:]:
    if len(cells) < 6:
        continue
    m_req = re.match(r"(\d+)\s*B", cells[0])
    m_rsp = re.match(r"(\d+)\s*B", cells[2])
    t_req = re.search(r"`(\d+)`", cells[1])
    t_rsp = re.search(r"`(\d+)`", cells[3])
    if not (m_req and m_rsp and t_req and t_rsp):
        continue
    key = (int(m_req.group(1)), int(t_req.group(1)))
    check(key not in spec_pairs, f"spec CENSUS-D: row for {key} appears more than once")
    spec_pairs[key] = {"rsp": (int(m_rsp.group(1)), int(t_rsp.group(1))),
                       "frames": int(cells[4]), "pcaps": int(cells[5])}
check(set(spec_pairs) == set(EXPECTED["pairs"]),
      f"spec CENSUS-D rows {sorted(spec_pairs)} != EXPECTED {sorted(EXPECTED['pairs'])}")
for key, want in EXPECTED["pairs"].items():
    got = spec_pairs.get(key)
    if got is None:
        continue
    check(got["rsp"] == tuple(want["rsp"]),
          f"spec CENSUS-D {key}: response {got['rsp']} != EXPECTED {tuple(want['rsp'])}")
    check(got["frames"] == want["frames"] and got["pcaps"] == want["pcaps"],
          f"spec CENSUS-D {key}: {got['frames']}/{got['pcaps']} != EXPECTED "
          f"{want['frames']}/{want['pcaps']}")

e_rows = table_after("<!-- CENSUS-E:", spec)
check(len(e_rows) >= 3, "spec: CENSUS-E table not found or too short")
spec_match = {}
for cells in e_rows[1:]:
    if len(cells) < 4:
        continue
    m_rank = re.match(r"(\d+)", cells[0])
    m_val = re.search(r"`0x([0-9a-fA-F]{4})`", cells[1])
    if not (m_rank and m_val):
        continue
    key = (int(m_rank.group(1)), int(m_val.group(1), 16))
    check(key not in spec_match, f"spec CENSUS-E: row for {key} appears more than once")
    spec_match[key] = {"frames": int(cells[2]), "pcaps": int(cells[3])}
check(set(spec_match) == set(EXPECTED["match_flag"]),
      f"spec CENSUS-E rows {sorted(spec_match)} != EXPECTED {sorted(EXPECTED['match_flag'])}")
for key, want in EXPECTED["match_flag"].items():
    got = spec_match.get(key)
    if got is None:
        continue
    check(got["frames"] == want["frames"] and got["pcaps"] == want["pcaps"],
          f"spec CENSUS-E {key}: {got['frames']}/{got['pcaps']} != EXPECTED "
          f"{want['frames']}/{want['pcaps']}")

# ===========================================================================
# 4. THE QUARANTINE -- the refuted claim may live in exactly one place
# ===========================================================================

QBEGIN = "REFUTED-QUOTE-BEGIN"
QEND = "REFUTED-QUOTE-END"
MAX_QUARANTINE_CHARS = 1200


def quarantine_spans(text, complain=True):
    """The quarantine blocks, with the block STRUCTURE itself checked.

    The markers must strictly alternate BEGIN, END, BEGIN, END -- no nesting,
    no stray END -- and each block is size-capped. Both rules exist because the
    obvious way to defeat a quarantine gate is to make the quarantine bigger:
    wrap a whole section, or open a second BEGIN inside an open block so that
    the span the parser computes is the small inner one while the text a reader
    sees as quarantined is the large outer one.
    """
    toks = sorted([(m.start(), QBEGIN) for m in re.finditer(re.escape(QBEGIN), text)] +
                  [(m.start(), QEND) for m in re.finditer(re.escape(QEND), text)])
    spans = []
    open_at = None
    for pos, kind in toks:
        if kind == QBEGIN:
            if open_at is not None and complain:
                check(False, f"a REFUTED-QUOTE-BEGIN at offset {pos} opens inside a "
                             f"block already open at {open_at} -- nested quarantine "
                             f"markers make the parsed span smaller than the "
                             f"quarantined text")
            if open_at is None:
                open_at = pos
        else:
            if open_at is None:
                if complain:
                    check(False, f"a REFUTED-QUOTE-END at offset {pos} with no open "
                                 f"block")
                continue
            end = pos + len(QEND)
            if complain:
                check(end - open_at <= MAX_QUARANTINE_CHARS,
                      f"a REFUTED-QUOTE block is {end - open_at} characters long "
                      f"(cap {MAX_QUARANTINE_CHARS}). Quarantining a whole section "
                      f"is not a way to make the dead claim legal again.")
            spans.append((open_at, end))
            open_at = None
    if open_at is not None and complain:
        check(False, "an unbalanced REFUTED-QUOTE-BEGIN with no matching END")
    return spans


def outside_quarantine(text):
    spans = quarantine_spans(text, complain=False)
    out = []
    prev = 0
    for b, e in spans:
        out.append(text[prev:b])
        prev = e
    out.append(text[prev:])
    return "".join(out)



# The dead claim must be quarantined SOMEWHERE, in exactly one block per
# document, so the reader who saw the old text can still recognise it. (The
# spec already carries an unrelated vms-6b3 quarantine block, so the count is
# not pinned globally -- what is pinned is that the 5/7 claim appears inside a
# block, once, in each of the two documents that quote it.)
def quarantined_5_7_blocks(text):
    return [text[b:e] for b, e in quarantine_spans(text, complain=False)
            if re.search(SUBJECT_PRE, text[b:e])]


SUBJECT_PRE = r"(?:`?5`?\s*(?:and|or|nor|/|,)\s*`?7`?|REJECT_RSP\s*(?:and|or|nor|/|,)\s*DISCONNECT_RSP)"

# Run the STRUCTURAL checks on every document's quarantine markers -- balance,
# nesting and the size cap. (The spec also carries an unrelated vms-6b3 block;
# these rules apply to it too, which is the point of checking structure rather
# than counting blocks.)
for doc_name, doc in (("spec", spec), ("scs_disc.h", header), ("scsd.c", daemon)):
    quarantine_spans(doc, complain=True)

for doc_name, doc in (("spec", spec), ("scs_disc.h", header)):
    n = len(quarantined_5_7_blocks(doc))
    check(n == 1,
          f"{doc_name} has {n} quarantine block(s) carrying the dead '5 and 7 "
          f"do not exist' claim; expected exactly 1")

# The dead claim, as a FAMILY: a subject naming message types 5 and 7 (or the
# two RESPONSE messages by name) together with an assertion of absence, inside
# ONE sentence. Reworded revivals are caught; the true scoped statement -- that
# the OLD census, restricted to the 62/66/110 classes, did not see them -- is
# rescued.
SUBJECT = r"(?:`?5`?\s*(?:and|or|nor|/|,)\s*`?7`?|REJECT_RSP\s*(?:and|or|nor|/|,)\s*DISCONNECT_RSP)"
ABSENCE = (r"(?:do(?:es)?\s+not\s+exist|never\s+(?:appears?|occurs?)|"
           r"absent\s+from|not\s+(?:present|observed|seen)|"
           r"appears?\s+(?:in\s+)?no\s+capture|on\s+no\s+capture|"
           r"do\s+not\s+build)")
# "Neither 5 nor 7 appears in ANY capture we hold" carries its negation in the
# determiner, not in the verb, so the ABSENCE alternation above cannot see it.
# This is not hypothetical: that exact rewording SURVIVED the first version of
# this gate. A negation word anywhere in the sentence makes an otherwise
# positive existence claim about 5 and 7 a dead claim too.
NEGATION = r"\b(?:neither|nor|never|no|not|none)\b"
EXISTENCE = r"(?:appears?|occurs?|exists?|present|observed|seen|in\s+any\s+capture)"
RESCUE = (r"(?:restrict|{62,\s*66,\s*110}|length\s+class|sampling|refuted|"
          r"58[- ]byte|corrected|earlier|old\s+census|used\s+to)")

DEAD_CLAIMS = []
for doc_name, doc in (("spec", spec), ("scs_disc.h", header), ("scsd.c", daemon)):
    body = outside_quarantine(doc)
    for sentence in re.split(r"(?<=[.!?])\s+|\n\n", body):
        if not re.search(SUBJECT, sentence):
            continue
        dead = bool(re.search(ABSENCE, sentence, re.I)) or (
            bool(re.search(NEGATION, sentence, re.I)) and
            bool(re.search(EXISTENCE, sentence, re.I)))
        if not dead:
            continue
        if re.search(RESCUE, sentence, re.I):
            continue
        DEAD_CLAIMS.append((doc_name, " ".join(sentence.split())[:160]))
check(not DEAD_CLAIMS,
      "the REFUTED claim that connection-control message types 5 and 7 are "
      "absent from our captures is asserted outside the quarantine block:\n" +
      "\n".join(f"      {d}: {s}" for d, s in DEAD_CLAIMS))

# And the correction must actually be present, not merely the deletion.
check("58" in spec and re.search(r"DISCONNECT_RSP", spec) is not None,
      "the spec no longer states the 58-byte response class -- deleting the "
      "wrong claim is not the same as recording the right one")
# The NEW gap must survive in BOTH places it was written: the sec 4(h)(1b)
# paragraph that found it and the sec 5 register. Checking one phrase in one
# place is not enough -- "unidentified" appears in the sec 5 entry, so a check
# that only looked for the word stayed green when the sec 4(h)(1b) sentence was
# neutered. Measured: that mutant SURVIVED the first version of this check.
for phrase, why in (
        ("nothing we hold identifies them",
         "sec 4(h)(1b) no longer says the message types 8 and 9 it turned up "
         "are unidentified"),
        ("They are not\nnamed here.",
         "sec 4(h)(1b) no longer refuses to name message types 8 and 9"),
        ("nothing we hold identifies them",
         "the sec 5 register no longer carries the 8/9 gap")):
    check(phrase.replace("\\n", "\n") in spec,
          f"{why}. A new gap found while correcting an old one must be written "
          f"down, not dropped.")
check(re.search(r"\b8\b.{0,80}\b9\b", spec, re.S) is not None,
      "the spec no longer names message types 8 and 9 together")

# ===========================================================================
# THE LAB FENCE (vms-096)
# ===========================================================================
# Six 2026-08-05 lab-2 vaxlab-4 captures were deposited into the LAB-1
# grounding library and silently mixed two labs -- lab-2 replicas reuse lab-1's
# SCSSYSTEMIDs and node MACs by design (tests/lab/README.md), so the frames look
# like lab-1's nodes. Measured at the time: 23 of scs_disc_measure's 34 checks
# red, 13 of 27 in scs_reason_measure, 18 of 67 in scs_connect_data_measure, 11
# of 30 in scs_credit_measure. The captures now live in a
# /data/training/vax/cluster/captures-lab2 SIBLING and each globbing tool
# carries a `lab1_only()` guard. This gate keeps the guard alive: it is the
# executable half of the fence, since the guard itself only fires on a host
# that has the captures.
check(hasattr(MEASURE_MOD, "lab1_only"),
      "scs_disc_measure.py has lost its lab1_only() fence -- a lab-2 capture "
      "dropped into the lab-1 library would silently move every census again")
if hasattr(MEASURE_MOD, "lab1_only"):
    clean = ["/x/cd0-baseline-current-20260728.pcap", "/x/formation-01.pcap"]
    check(MEASURE_MOD.lab1_only(list(clean)) == clean,
          "lab1_only() rejected a clean lab-1 capture list")
    try:
        MEASURE_MOD.lab1_only(clean + ["/x/vms578-B1-lab2-vaxlab4-20260805.pcap"])
        check(False, "lab1_only() accepted a lab-2 capture in the lab-1 library")
    except SystemExit as exc:
        check("vms578-B1-lab2-vaxlab4-20260805.pcap" in str(exc),
              "lab1_only() refused the mixed list without naming the offender")

# Every lab-1-grounded tool that GLOBS the library must carry the same fence,
# and the fence must be wired into the glob -- a defined-but-uncalled guard is
# the same defect in a different place.
FENCED = {
    "tools/cluster/scs_disc_measure.py": "lab1_only(sorted(glob.glob(",
    "tools/cluster/scs_reason_measure.py": "lab1_only(sorted(glob.glob(",
    "tools/scs_credit_measure.py": "lab1_only(sorted(glob.glob(",
    "tools/scs_connect_data_measure.py": "lab1_only(sorted(glob.glob(",
}
for rel, wiring in FENCED.items():
    path = os.path.join(ROOT, rel)
    if not os.path.exists(path):
        check(False, f"{rel} is missing")
        continue
    src = read(path)
    check("def lab1_only(" in src, f"{rel} has no lab1_only() fence")
    check(wiring in src,
          f"{rel} defines lab1_only() but does not wrap its glob with it")

# The one tool that legitimately reads lab-2 must point AWAY from the lab-1
# library, or the separation is only a comment.
JC = os.path.join(ROOT, "tools/cluster/scs_join_capability_measure.py")
if os.path.exists(JC):
    jc = read(JC)
    check(re.search(r'DEFAULT_CAPTURE_DIR\s*=\s*"[^"]*captures-lab2"', jc),
          "scs_join_capability_measure.py still defaults to the lab-1 capture "
          "directory; its six lab-2 captures belong in the -lab2 sibling")

# ===========================================================================
# N. THE WIRE ITSELF (vms-371)
# ===========================================================================
# Everything above pins the PROSE to EXPECTED. This pins EXPECTED to the
# PACKETS -- and the two together are what make a green run mean "the prose
# matches a re-derived measurement" rather than "the prose matches a table that
# also drifted". Without captures it announces that loudly and reds nothing;
# with OVMX_SCS_REQUIRE_WIRE=1 the absence itself is a failure.
scs_wire.gate("scs_disc_figures", MEASURE_MOD, MEASURE_MOD.DEFAULT_CAPDIR, check)

print(f"{'FAIL' if failures else 'PASS'}: {checks} checks, {failures} failure(s)")
sys.exit(1 if failures else 0)
