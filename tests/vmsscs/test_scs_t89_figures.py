#!/usr/bin/env python3
"""
test_scs_t89_figures.py -- vms-a58: the types-8/9 census must stay pinned to
the packets, and the four readings it eliminated must stay dead.

NEEDS NO CAPTURES for the prose half. It compares the figures written into
  docs/cluster-protocol-spec.md   (CENSUS-T89-A/B/C/D and the sec 5 register)
  src/vmsscs/scsd.c              (the T89-CENSUS-INV lines carrying the ruling
                                  at the one place OVMX touches these messages)
against the EXPECTED table in tools/cluster/scs_t89_measure.py, which is the
checked-in record of what the 47 lab-1 captures measured. On a host WITH the
captures it also calls that tool's rederive() through scs_wire.gate(), so a
green run there means wire == EXPECTED == prose (vms-371).

WHY THIS GATE EXISTS, in the specific terms of what went wrong before.

  Message types 8 and 9 have now been read three different ways in this repo's
  own documents: "a ninth and tenth connection-control message" (spec
  sec 4(h)(1b)), "the SCA credit-flow control pair" (design-mscp-direction.md
  sec 1.3), and "constant credit, so not a credit message" (spec sec 4(h)(1c)).
  Each was written from a census that measured something slightly different,
  and nothing in ctest could tell any of them apart, because no gate read a
  census of these two types at all.

  vms-a58 settled the part that is settleable -- the credit field is a COUNT,
  and 8/9 are inside the credit account that connection-control types sit
  outside -- and explicitly did NOT settle the names. Both halves are fragile
  in the same way: the positive half is a table of numbers that can drift, and
  the negative half is a refusal that a later reader can quietly undo by
  writing the tempting name back down. This gate holds both.

  (1) PARSES every figure out of the spec's four marked tables and out of
      scsd.c's T89-CENSUS-INV lines, keyed, with a hard no-duplicate rule --
      a figure that appears twice lets a drift in one copy hide, which is the
      defect that got the vms-6b3 gate rewritten. ONE COPY PER FIGURE PER
      DOCUMENT.

  (2) KEEPS THE FOUR ELIMINATED READINGS DEAD. A sentence that names types 8
      and 9 as a credit message / the special credit message, or as
      connection-control messages, or that calls their credit field a version
      or a flag, is a failure unless the same sentence carries a refusal or a
      historical marker. Matching is by CLAIM FAMILY, not by exact sentence, so
      a reworded revival is caught too.

  (3) REQUIRES THE HONEST LIMIT TO SURVIVE. The SYSAP census returned a
      NEGATIVE -- 131/131 SCS$DIRECTORY, but so is the entire teardown
      population, so it cannot discriminate. That sentence is the single most
      deletable thing in the whole entry (it is the one that says the pretty
      131/131 result proves less than it looks like it does), so its absence
      is a red.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import scs_wire                                                    # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

SPEC = os.environ.get("OVMX_SCS_T89_SPEC",
                      os.path.join(ROOT, "docs/cluster-protocol-spec.md"))
DAEMON = os.environ.get("OVMX_SCS_T89_DAEMON",
                        os.path.join(ROOT, "src/vmsscs/scsd.c"))
DESIGN = os.environ.get("OVMX_SCS_T89_DESIGN",
                        os.path.join(ROOT, "docs/design-mscp-direction.md"))
MEASURE = os.environ.get("OVMX_SCS_T89_MEASURE",
                         os.path.join(ROOT, "tools/cluster/scs_t89_measure.py"))

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


def load_measure():
    """Execute the measure script FROM SOURCE, never from cached bytecode --
    see scs_wire.load_source(): importlib would accept a stale __pycache__
    entry for a same-size, same-second edit, and this gate reads EXPECTED out
    of the module it returns."""
    d = os.path.dirname(MEASURE)
    if d not in sys.path:
        sys.path.insert(0, d)
    return scs_wire.load_source(MEASURE, "scs_t89_measure")


MEASURE_MOD = load_measure()
EXPECTED = MEASURE_MOD.EXPECTED

spec = read(SPEC)
daemon = read(DAEMON)
design = read(DESIGN)

print("test_scs_t89_figures: vms-a58 types-8/9 census vs the prose")


def table_after(marker, text):
    """The markdown table that follows `marker`, minus its rule row."""
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


def num(cell):
    m = re.search(r"(\d+)", cell.replace(",", ""))
    return int(m.group(1)) if m else None


# ===========================================================================
# 0. THE POPULATION ITSELF
# ===========================================================================
# Pinned in the prose because every other figure is a count OVER it: a census
# whose corpus size drifts is a different census wearing the same numbers. (The
# vms-371 mutation battery flips EXPECTED["n_captures"] behind a primed
# __pycache__ with the wire arm off; without this check that mutant SURVIVED.)
caps = re.findall(r"(\d+)-capture lab-1 library", spec)
check(len(caps) == 1,
      f"spec: expected exactly one statement of the capture-library size, got {len(caps)}")
if caps:
    check(int(caps[0]) == EXPECTED["n_captures"],
          f"spec says a {caps[0]}-capture lab-1 library, EXPECTED "
          f"{EXPECTED['n_captures']}")

# ===========================================================================
# 1. CENSUS-T89-A -- the UNRESTRICTED length census, split by the OUI rule
# ===========================================================================
a_rows = table_after("<!-- CENSUS-T89-A:", spec)
check(len(a_rows) >= 2, "spec: CENSUS-T89-A table not found or too short")
got_vms, got_ovmx = {}, {}
for cells in a_rows[1:]:
    if len(cells) < 4:
        continue
    ln, mt = num(cells[0]), num(cells[1])
    if ln is None or mt is None:
        continue
    key = (ln, mt)
    check(key not in got_vms, f"spec CENSUS-T89-A: row for {key} appears more than once")
    got_vms[key] = num(cells[2])
    got_ovmx[key] = num(cells[3])
check(got_vms == EXPECTED["census_vms"],
      f"spec CENSUS-T89-A VMS-origin column != EXPECTED; differences: "
      f"{ {k: (got_vms.get(k), EXPECTED['census_vms'].get(k)) for k in set(got_vms) | set(EXPECTED['census_vms']) if got_vms.get(k) != EXPECTED['census_vms'].get(k)} }")
check(got_ovmx == EXPECTED["census_ovmx"],
      f"spec CENSUS-T89-A OVMX-origin column != EXPECTED; differences: "
      f"{ {k: (got_ovmx.get(k), EXPECTED['census_ovmx'].get(k)) for k in set(got_ovmx) | set(EXPECTED['census_ovmx']) if got_ovmx.get(k) != EXPECTED['census_ovmx'].get(k)} }")

# The population the census did NOT read at [46:48] -- sec 4(h)(1d)'s rule
# executed. One copy of each figure in the document.
nc = re.findall(r"\*\*(\d+)\*\* SCA-ethertype frames across\s*\n?\s*\*\*(\d+)\*\* length classes", spec)
check(len(nc) == 1,
      f"spec: expected exactly one statement of the non-conformant population, got {len(nc)}")
if nc:
    check(int(nc[0][0]) == EXPECTED["non_conformant_total"],
          f"spec non-conformant frames {nc[0][0]} != EXPECTED "
          f"{EXPECTED['non_conformant_total']}")
    check(int(nc[0][1]) == EXPECTED["non_conformant_classes"],
          f"spec non-conformant classes {nc[0][1]} != EXPECTED "
          f"{EXPECTED['non_conformant_classes']}")

# ===========================================================================
# 2. CENSUS-T89-B -- the owning-SYSAP census
# ===========================================================================
b_rows = table_after("<!-- CENSUS-T89-B:", spec)
check(len(b_rows) >= 2, "spec: CENSUS-T89-B table not found or too short")
got_sysap = {}
for cells in b_rows[1:]:
    if len(cells) < 5:
        continue
    name = cells[0].strip().strip("`")
    check(name not in got_sysap, f"spec CENSUS-T89-B: row for {name} appears more than once")
    got_sysap[name] = {"dialogues": num(cells[1]), "accepted": num(cells[2]),
                       "torn_down": num(cells[3]), "with_t8": num(cells[4])}
check(set(got_sysap) == set(EXPECTED["sysap"]),
      f"spec CENSUS-T89-B rows {sorted(got_sysap)} != EXPECTED {sorted(EXPECTED['sysap'])}")
for name, want in EXPECTED["sysap"].items():
    got = got_sysap.get(name)
    if got is None:
        continue
    check(got == want, f"spec CENSUS-T89-B {name}: {got} != EXPECTED {want}")

# ===========================================================================
# 3. CENSUS-T89-C -- the structural invariants, IN ORDER
# ===========================================================================
# (row-label regex, EXPECTED key or pair of keys). Order is part of the check:
# a row that moves is a row someone rewrote.
ROWS_C = [
    (r"type `8` frames / type `9` frames", ("t8_frames", "t9_frames")),
    (r"dialogues carrying a type `8`", "dialogues_with_t8"),
    (r"dialogues that also disconnect", "t8_dialogues_that_disconnect"),
    (r"disconnect WITHOUT a type `8`", "disconnecting_dialogues_without_t8"),
    (r"sender is the connection's opener", "t8_sender_is_connection_initiator"),
    (r"first `DISCONNECT_REQ` sender", "t8_sender_is_disconnect_initiator"),
    (r"handle pair swapped", "t9_answers_t8_with_handles_swapped"),
    (r"exactly the `9`", "t8_to_disc_gap_is_exactly_t9"),
    (r"before the last application message", "t8_before_last_application_message"),
    (r"sourced by OVMX", "t8_sourced_by_ovmx"),
    (r"rank 0", "ovmx_disconnect_req_rank0"),
    (r"rank 1", "ovmx_disconnect_req_rank1"),
]
c_rows = table_after("<!-- CENSUS-T89-C:", spec)
check(len(c_rows) - 1 == len(ROWS_C),
      f"spec CENSUS-T89-C has {len(c_rows) - 1} data rows, expected {len(ROWS_C)}")
for i, (pat, key) in enumerate(ROWS_C):
    if i + 1 >= len(c_rows):
        check(False, f"spec CENSUS-T89-C: no row {i} for {key}")
        continue
    cells = c_rows[i + 1]
    check(re.search(pat, cells[0]) is not None,
          f"spec CENSUS-T89-C row {i} is {cells[0]!r}, expected one matching {pat!r}")
    if isinstance(key, tuple):
        vals = [int(v) for v in re.findall(r"\d+", cells[1])]
        want = [EXPECTED["invariants"][k] for k in key]
        check(vals == want, f"spec CENSUS-T89-C row {i} ({key}): {vals} != EXPECTED {want}")
    else:
        check(num(cells[1]) == EXPECTED["invariants"][key],
              f"spec CENSUS-T89-C row {i} ({key}): {num(cells[1])} != EXPECTED "
              f"{EXPECTED['invariants'][key]}")

# ===========================================================================
# 4. CENSUS-T89-D -- the credit ledger, agreeing vs residual
# ===========================================================================
d_rows = table_after("<!-- CENSUS-T89-D:", spec)
check(len(d_rows) == 3,
      f"spec CENSUS-T89-D has {len(d_rows)} rows (header + agreeing + residual expected)")
if len(d_rows) == 3:
    header, agree, resid = d_rows
    mts = [num(c) for c in header[1:]]
    check(mts == [8, 9, 10],
          f"spec CENSUS-T89-D columns are msgtypes {mts}, expected [8, 9, 10]")
    check(re.search(r"agree", agree[0], re.I) is not None,
          f"spec CENSUS-T89-D: row 1 is {agree[0]!r}, expected the agreeing row")
    check(re.search(r"residual", resid[0], re.I) is not None,
          f"spec CENSUS-T89-D: row 2 is {resid[0]!r}, expected the residual row")
    for i, mt in enumerate(mts):
        if mt is None:
            continue
        check(num(agree[i + 1]) == EXPECTED["ledger_ok"].get(mt),
              f"spec CENSUS-T89-D agreeing[{mt}] {num(agree[i + 1])} != EXPECTED "
              f"{EXPECTED['ledger_ok'].get(mt)}")
        check(num(resid[i + 1]) == EXPECTED["ledger_bad"].get(mt, 0),
              f"spec CENSUS-T89-D residual[{mt}] {num(resid[i + 1])} != EXPECTED "
              f"{EXPECTED['ledger_bad'].get(mt, 0)}")

# The two credit values the count reading turns on, and the two latency bounds.
for pat, want, what in (
        (r"reads `0` on the first application message of every connection\s*\n?\s*\(\s*(\d+) frames",
         EXPECTED["app_credit_first_in_dialogue"][0], "credit-0 first application messages"),
        (r"`1` on all (\d+) later ones",
         EXPECTED["app_credit_later"][1], "credit-1 later application messages"),
        (r"worst case \*\*([0-9.]+) s\*\*,",
         EXPECTED["latency"]["t8_to_t9_max"], "8->9 worst-case latency"),
        (r"\*\*([0-9.]+) s\*\* from the `9` to the `DISCONNECT_REQ`",
         EXPECTED["latency"]["t9_to_disc_max"], "9->DISCONNECT_REQ worst-case latency"),
        (r"in \*\*(\d+) of 131\*\*\s*\n?dialogues",
         EXPECTED["disc_leaves_one_credit_unreturned"], "unreturned-credit residual"),
):
    found = re.findall(pat, spec)
    check(len(found) == 1,
          f"spec: the {what} figure appears {len(found)} time(s), expected exactly 1")
    if len(found) == 1:
        got = float(found[0]) if "." in found[0] else int(found[0])
        check(got == want, f"spec {what} {got} != EXPECTED {want}")

# ===========================================================================
# 5. THE RULING, AT THE CODE SITE
# ===========================================================================
inv_lines = re.findall(r"T89-CENSUS-INV:\s*(\w+)=(\d+)", daemon)
check(len(inv_lines) == 2,
      f"scsd.c carries {len(inv_lines)} T89-CENSUS-INV lines, expected 2 -- the "
      f"vms-a58 ruling at scs_reflect_credit() has no measurement behind it")
seen = set()
for name, val in inv_lines:
    check(name not in seen, f"scsd.c: T89-CENSUS-INV {name} appears more than once")
    seen.add(name)
    check(name in EXPECTED["invariants"],
          f"scsd.c: T89-CENSUS-INV names {name}, which is not an EXPECTED invariant")
    if name in EXPECTED["invariants"]:
        check(int(val) == EXPECTED["invariants"][name],
              f"scsd.c T89-CENSUS-INV {name}={val} != EXPECTED "
              f"{EXPECTED['invariants'][name]}")

# The two halves of the emission ruling must both survive. Deleting either one
# turns "OVMX must send the 8 it does not send" back into silence.
for phrase, why in (
        ("Emitting the type 8 first is REQUIRED",
         "scsd.c no longer says an OVMX-initiated teardown must be preceded by "
         "a type 8"),
        ("do NOT hard-code the 1 the",
         "scsd.c no longer warns that the credit value must be derived, not "
         "replayed from the captures"),
):
    check(phrase in daemon, f"{why}. A ruling that lives only in a doc decays.")

# ===========================================================================
# 6. THE HONEST LIMIT -- the most deletable sentence in the entry
# ===========================================================================
check(re.search(r"SYSAP split cannot discriminate", spec) is not None,
      "the spec no longer records that the owning-SYSAP census CANNOT "
      "discriminate an SCA-level exchange from an SCS$DIRECTORY-level one -- "
      "without it the 131/131 result reads as proof of something it does not "
      "prove")
check(re.search(r"both hypotheses predict\s+the observation exactly", spec) is not None,
      "the spec no longer says WHY the SYSAP census cannot discriminate")
check(re.search(r"are not\s*\nconnection-control messages at all", spec) is not None,
      "the spec no longer states the positive finding: types 8 and 9 are not "
      "connection-control messages")
check("count-of-one" in spec or "COUNT OF ONE" in spec,
      "the spec no longer records the answer to the constant-1 lead")
check("scs_t89_measure.py" in spec,
      "the spec no longer cites the tool its types-8/9 figures come from")

# ===========================================================================
# 7. THE FOUR ELIMINATED READINGS MUST STAY DEAD
# ===========================================================================
# A sentence that IDENTIFIES types 8/9 as one of the eliminated things is a
# failure unless it also carries a refusal or a historical marker. Matching is
# by family so a rewording is caught; RESCUE is what lets the documents keep
# discussing the dead readings honestly, which they must.
SUBJECT = (r"(?:types?\s*`?8`?\s*(?:and|/|,|&)\s*`?9`?|`?8`?\s*/\s*`?9`?|"
           r"msgtypes?\s*`?8`?\s*(?:and|/|,)\s*`?9`?)")
CLAIM = (r"(?:special\s+credit\s+message|credit\s+message|credit\s+pair|"
         r"credit-flow\s+control\s+pair|connection-control\s+message|"
         r"ninth\s+and\s+tenth|version\s+(?:field|word|byte)|flag\s+(?:field|word|byte))")
RESCUE = (r"(?:\bnot\b|\bno\b|\bnever\b|\bneither\b|\bnor\b|refut|eliminat|"
          r"weaken|supersed|overturn|dead|candidate|do\s+not|cannot|"
          r"originally|used\s+to|earlier|previously|unnamed|unidentif|"
          r"UNGROUNDED|against|open|still|question|hypothes|reading below|"
          r"this paragraph)")


def sentences(doc):
    """Split into sentences WITHOUT splitting inside an abbreviation.

    This matters here and is not hypothetical: the mutation battery's M7 --
    "Types `8` and `9` are the p. 2-44 special credit message." -- SURVIVED the
    first version of this check, because the period in the page cite `p. 2-44`
    cut the sentence in half and left the subject in one piece and the claim in
    the next. Every dead reading in this section is stated with a page cite, so
    an abbreviation-blind splitter cannot see any of them.
    """
    doc = re.sub(r"\b(pp?|sec|fig|Fig|no|vs|cf|e\.g|i\.e)\.\s+(?=[\dA-Za-z])",
                 lambda m: m.group(1) + ".", doc)
    return re.split(r"(?<=[.!?])\s+|\n\n", doc)


DEAD = []
for doc_name, doc in (("spec", spec), ("scsd.c", daemon), ("design-mscp-direction.md", design)):
    for sentence in sentences(doc):
        flat = " ".join(sentence.split())
        if not re.search(SUBJECT, flat, re.I):
            continue
        if not re.search(CLAIM, flat, re.I):
            continue
        if re.search(RESCUE, flat, re.I):
            continue
        DEAD.append((doc_name, flat[:160]))
check(not DEAD,
      "an ELIMINATED reading of message types 8 and 9 is asserted as an "
      "identification (vms-a58 sec 5 lists the four and what killed each):\n" +
      "\n".join(f"      {d}: {s}" for d, s in DEAD))

# ===========================================================================
# 8. THE LAB FENCE (vms-096 / vms-beb)
# ===========================================================================
check(hasattr(MEASURE_MOD, "lab1_only"),
      "scs_t89_measure.py has lost its lab1_only() fence -- a lab-2 capture "
      "dropped into the lab-1 library would silently move every figure here")
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
src = read(MEASURE)
check("lab1_only(sorted(glob.glob(" in src,
      "scs_t89_measure.py defines lab1_only() but does not wrap its glob with it")

# The census guard must be CALLED, not merely imported: this census is
# unrestricted by construction and the call is what makes a later narrowing red.
check("check_census(" in src,
      "scs_t89_measure.py no longer calls census_guard.check_census() -- nothing "
      "would stop a future edit narrowing this census the way vms-c11 did")

# ===========================================================================
# 9. THE WIRE ITSELF (vms-371)
# ===========================================================================
scs_wire.gate("scs_t89_figures", MEASURE_MOD, MEASURE_MOD.DEFAULT_CAPDIR, check)

print(f"{'FAIL' if failures else 'PASS'}: {checks} checks, {failures} failure(s)")
sys.exit(1 if failures else 0)
