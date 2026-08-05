#!/usr/bin/env python3
"""
test_scs_reason_figures.py -- the reason-code prose must match the measurement.

vms-6b3, review round 2. BOTH defects that round found were the same defect:
a figure that only a comment carried.

  1. src/vmsscs/include/scs_reason.h and spec sec 5 justified payload [58:60]
     as "the only slot zero in 100% of observed frames". Re-running the census
     with the file's OWN population rule refutes it twice over -- neighbouring
     message types on the SAME 62-byte layout DO set the slot (ACCEPT_RSP,
     msgtype 3, shares the identical 62-byte layout), and it is not the only
     always-zero slot either.
  2. Spec sec 4(h)(1a) said "nothing after the Con.ID pair varies", while
     tools/cluster/scs_reason_measure.py printed, in the same commit,
     payload[60:62] = 0x0000 x131 / 0x0001 x89 on DISCONNECT_REQ.

Neither could be caught by a green run, because nothing in ctest read the
measurement.

HOW THIS GATE WORKS, AND WHY IT IS A PARSER AND NOT A GREP. Both documents now
carry the figures on machine-readable CENSUS lines: `CENSUS-A/B/C ...` inside
the scs_reason.h comment, and markdown tables introduced by `<!-- CENSUS-x:`
markers in the spec. This test PARSES those into dicts and compares them, field
by field, against the EXPECTED table in tools/cluster/scs_reason_measure.py --
the checked-in record of what the captures measured. Any single digit that
drifts in either document, or in EXPECTED, reds.

A first version of this gate searched for each figure as a substring with an
adjacency window. Six of fourteen mutants SURVIVED it, because a figure that
appears twice in a document masks a drift in one copy. Hence: one copy per
figure, parsed, compared. Do not regress it to substring searching.

HOW THE REFUTED-CLAIM HALF WORKS, AND WHY IT IS NOT A PROXIMITY WINDOW. Review
round 3 measured the round-2 version of this same check and found it did not
work: it matched each dead sentence and then asked whether an EXCUSE WORD
("refuted", "is false", "wrongly", "earlier revision") appeared within 500
characters before / 300 after. Both documents are ABOUT the refutation, so those
words are everywhere -- the excuse windows cover roughly 4 kB of the spec and
2 kB of the header. Re-inserting "Nothing after the Con.ID pair varies." into
its own sec 4(h)(1a) paragraph, into the sec 5 refutation paragraph, and into
the scs_reason.h census paragraph left the gate GREEN in all three. A gate that
is defeated by writing the claim next to the word "refuted" is not a gate.

The replacement has three parts and no windows:

  1. QUARANTINE. A refuted claim may be written down ONLY inside an explicit
     `REFUTED-QUOTE-BEGIN` ... `REFUTED-QUOTE-END` block. Anywhere else, in
     either document, it reds -- position is irrelevant, neighbouring prose is
     irrelevant. The blocks themselves are checked: balanced, size-capped (so
     "quarantine the whole section" is not an escape), each one must carry a
     refutation label, and none may swallow a CENSUS or REFUTATION-FACT line.

  2. CLAIM FAMILIES, NOT SENTENCES. The prose can be reworded, so each dead
     claim is matched as SUBJECT + CONSTANCY-ASSERTION within one sentence,
     with an explicit RESCUE clause naming the exception that makes a scoped
     version of the sentence true. "payload [60:62] is a constant on
     REJECT_REQ but VARIES on DISCONNECT_REQ" is rescued; "nothing after the
     Con.ID pair varies" is not, however it is phrased.

  3. THE POSITIVE FACT, PINNED IN BOTH DOCUMENTS. `REFUTATION-FACT` lines carry
     the two measurements that DO the refuting -- payload[60:62] taking two
     values on DISCONNECT_REQ (0x0000 x131 / 0x0001 x89), and ACCEPT_RSP
     setting payload[58:60] 62 times on the identical 62-byte layout -- parsed
     and compared against EXPECTED exactly like the CENSUS lines. Deleting the
     dead claim's contradiction is now as loud as asserting the dead claim.

ROUND 4 FOUND ONE MORE HOLE, AND IT WAS STRUCTURAL, NOT A MISSED CASE. The
scan flattened with re.sub(r"\\s+", " ", ...) and then split on (?<=[.!?])\\s+.
CENSUS markdown tables and REFUTATION-FACT lines carry no sentence-ending
punctuation, so every data block merged with the paragraph BELOW it into one
"sentence" -- and a rescue token sitting in the DATA (`0x0000 x 131` in the
CENSUS-A row, `0x0000:131` in REFUTATION-FACT, `at or after payload 50` in the
CENSUS-C row) excused a dead claim re-asserted in that prose. Sweeping the
claim into every paragraph-leading site and running this gate: 246/248 spec
sites and 46/48 header sites died, and all four survivors were the paragraph
right after a data block -- one of them the head of sec 4(h)(1a)'s own
paragraph. flatten() below fixes it by never letting a data line or a paragraph
break merge into a neighbouring unit. Post-fix the same sweep is 248/248 and
48/48; the superset sweep (every non-blank line of both documents) is
2122/2132, the ten exceptions being the ten lines that lie INSIDE a quarantine
block, which is the one place a dead claim is legal. The four sites are pinned
permanently as the DATABLOCK-* group of the mutation battery.

KNOWN, DELIBERATE FALSE POSITIVE: spelling the quarantine markers in ordinary
prose opens a span that has no partner (or wraps the wrong text) and reds this
gate. That is why neither document writes them out in explanatory text -- both
say "a quarantine block" and point at a real one. The failure mode is LOUD, not
silent, which is the correct trade for a mechanism whose entire job is to be
un-evadable; do not "fix" it by making the markers fuzzier.

The mutation battery for all of this is tests/vmsscs/test_scs_reason_mutants.py
(ctest name `scs_reason_mutants`), which re-derives the kill count on every run
instead of asserting one in a comment. That test drives THIS file by path, via
the OVMX_SCS_REASON_{HEADER,SPEC,MEASURE} environment overrides below.

AND SINCE vms-371 IT DOES RE-DERIVE FROM PACKETS WHEN THEY ARE HERE. On a host
with the lab captures main() calls scs_reason_measure.rederive() and reds on any
figure the packets no longer support. On a host without them it prints a banner
saying the wire was NOT read -- loudly, because the earlier silence is how
`ctest -L scs` stayed 32/32 green while this script was 13 checks red. See
tests/vmsscs/scs_wire.py.

Precedent and shape: tests/vmsscs/test_scs_credit_figures.py (vms-76e).
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import scs_wire                                                    # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

# The three inputs are overridable ONLY so the mutation battery can point this
# gate at a scratch copy of the tree. Nothing in the repo sets them.
HEADER = os.environ.get(
    "OVMX_SCS_REASON_HEADER",
    os.path.join(ROOT, "src", "vmsscs", "include", "scs_reason.h"))
SPEC = os.environ.get(
    "OVMX_SCS_REASON_SPEC",
    os.path.join(ROOT, "docs", "cluster-protocol-spec.md"))
MEASURE = os.environ.get(
    "OVMX_SCS_REASON_MEASURE",
    os.path.join(ROOT, "tools", "cluster", "scs_reason_measure.py"))

failures = []
checks = 0


def check(cond, msg):
    global checks
    checks += 1
    if not cond:
        failures.append(msg)


def eq(got, want, msg):
    check(got == want, "%s\n       got  %r\n       want %r" % (msg, got, want))


def load_measure():
    """Import the measure script WITHOUT running it, and WITHOUT __pycache__.

    It does sys.path.insert + `from dissect_sca import read_pcap` at import
    time, and dissect_sca.py sits next to it in tools/cluster, so the import
    succeeds with no captures present -- read_pcap is only called from
    measure().

    Why compile()+exec() rather than spec_from_file_location: the source-file
    loader validates its cached .pyc on (mtime-in-SECONDS, size), so an edit
    that keeps the file the same length and lands in the same second as a
    previous run silently reuses the OLD bytecode -- i.e. the OLD EXPECTED.
    That is not hypothetical: the mutation battery for this gate had exactly
    one survivor, `EXPECTED pcaps 25 -> 26`, and this was why. Reading the
    source every time makes the gate depend on the bytes on disk.

    The loader now lives in scs_wire.load_source() so all six figures gates
    share ONE implementation and the mutation battery attacks one place.
    """
    return scs_wire.load_source(MEASURE, "scs_reason_measure")


# ---------------------------------------------------------------------------
# parsers
# ---------------------------------------------------------------------------

def parse_values(s):
    """'0x0000:131,0x0001:89' or '0x0000 x 131, 0x0001 x 89' -> {0: 131, 1: 89}"""
    out = {}
    s = s.replace("`", "").replace("*", "")
    for m in re.finditer(r"0x([0-9a-fA-F]{1,4})\s*[:x×]\s*(\d+)", s):
        out[int(m.group(1), 16)] = int(m.group(2))
    return out


def parse_header(text):
    """Pull the CENSUS-A/B/C lines out of the scs_reason.h comment."""
    carriers, neighbours, zero_slots = {}, {}, {}
    after = None
    for m in re.finditer(r"CENSUS-A\s+type=(\d+)\s+name=(\S+)\s+frames=(\d+)\s+pcaps=(\d+)",
                         text):
        t = int(m.group(1))
        carriers.setdefault(t, {}).update(
            name=m.group(2), frames=int(m.group(3)), pcaps=int(m.group(4)))
    for m in re.finditer(r"CENSUS-A\s+type=(\d+)\s+off=(\d+)\s+values=(\S+)", text):
        t, off = int(m.group(1)), int(m.group(2))
        carriers.setdefault(t, {})["off%d" % off] = parse_values(m.group(3))
    for m in re.finditer(r"CENSUS-B\s+len=(\d+)\s+type=(\d+)\s+name=(\S+)\s+"
                         r"frames=(\d+)\s+pcaps=(\d+)\s+nonzero58=(\d+)", text):
        neighbours[(int(m.group(1)), int(m.group(2)))] = {
            "name": m.group(3), "frames": int(m.group(4)),
            "pcaps": int(m.group(5)), "nonzero58": int(m.group(6))}
    for m in re.finditer(r"CENSUS-C\s+type=(\d+)\s+zero_slots=([\d,]+)", text):
        zero_slots[int(m.group(1))] = tuple(int(x) for x in m.group(2).split(","))
    m = re.search(r"CENSUS-C\s+common_at_or_after_payload=(\d+)\s+zero_slots=([\d,]+)",
                  text)
    if m:
        after = (int(m.group(1)), tuple(int(x) for x in m.group(2).split(",")))
    return carriers, neighbours, zero_slots, after


def parse_population(text):
    """CENSUS-P sca_len_classes=62,66,110 pcaps_scanned=47 -- same in both docs."""
    m = re.search(r"CENSUS-P\s+sca_len_classes=([\d,]+)\s+pcaps_scanned=(\d+)", text)
    if not m:
        return None
    return (tuple(int(x) for x in m.group(1).split(",")), int(m.group(2)))


def parse_sda(text):
    """CENSUS-D sda_file=... cdts=N values=v:n,v:n -- same line in both docs."""
    m = re.search(r"CENSUS-D\s+sda_file=(\S+)\s+cdts=(\d+)\s+values=(\S+)", text)
    if not m:
        return None
    vals = {}
    for pair in m.group(3).split(","):
        k, _, v = pair.partition(":")
        vals[int(k)] = int(v)
    return {"file": m.group(1), "cdts": int(m.group(2)), "values": vals}


def md_table_after(text, marker):
    """Rows (list of stripped cell lists) of the first markdown table after
    `marker`, skipping the header and separator rows."""
    i = text.find(marker)
    if i < 0:
        return None
    rows = []
    started = False
    for line in text[i + len(marker):].splitlines():
        s = line.strip()
        if not s.startswith("|"):
            if started:
                break
            continue
        started = True
        cells = [c.strip() for c in s.strip("|").split("|")]
        if all(re.fullmatch(r":?-{2,}:?", c) for c in cells):
            continue
        rows.append(cells)
    return rows[1:] if rows else rows      # drop the header row


def cell_int(c):
    m = re.search(r"\d+", c.replace("`", "").replace("*", ""))
    return int(m.group(0)) if m else None


def parse_spec_carriers(spec):
    """§4(h)(1a) CENSUS-A table: | `4` REJECT_REQ | frames | pcaps | [58:60] | [60:62] |"""
    rows = md_table_after(spec, "<!-- CENSUS-A:")
    if rows is None:
        return None
    out = {}
    for cells in rows:
        if len(cells) != 5:
            return "MALFORMED ROW: %r" % (cells,)
        head = cells[0].replace("`", "").replace("*", "").split()
        out[int(head[0])] = {
            "name": head[1],
            "frames": cell_int(cells[1]),
            "pcaps": cell_int(cells[2]),
            "off58": parse_values(cells[3]),
            "off60": parse_values(cells[4]),
        }
    return out


def parse_spec_neighbours(spec):
    """§5 CENSUS-B table: | len | `type` | name | frames | pcaps | nonzero58 |"""
    rows = md_table_after(spec, "<!-- CENSUS-B:")
    if rows is None:
        return None
    out = {}
    for cells in rows:
        if len(cells) != 6:
            return "MALFORMED ROW: %r" % (cells,)
        out[(cell_int(cells[0]), cell_int(cells[1]))] = {
            "name": cells[2].replace("`", "").replace("*", "").strip(),
            "frames": cell_int(cells[3]),
            "pcaps": cell_int(cells[4]),
            "nonzero58": cell_int(cells[5]),
        }
    return out


def parse_refutation_facts(text):
    """The two POSITIVE facts that do the refuting, one machine-readable line
    each, required in BOTH documents:

      REFUTATION-FACT off=60 type=6 distinct_values=2 values=0x0000:131,0x0001:89
      REFUTATION-FACT off=58 len=62 type=3 name=ACCEPT_RSP nonzero=62

    Returns (varies_fact_or_None, neighbour_fact_or_None)."""
    varies = None
    m = re.search(r"REFUTATION-FACT\s+off=60\s+type=(\d+)\s+"
                  r"distinct_values=(\d+)\s+values=(\S+)", text)
    if m:
        varies = {"type": int(m.group(1)), "distinct": int(m.group(2)),
                  "values": parse_values(m.group(3))}
    nb = None
    m = re.search(r"REFUTATION-FACT\s+off=58\s+len=(\d+)\s+type=(\d+)\s+"
                  r"name=(\S+)\s+nonzero=(\d+)", text)
    if m:
        nb = {"len": int(m.group(1)), "type": int(m.group(2)),
              "name": m.group(3), "nonzero": int(m.group(4))}
    return varies, nb


# --- the refuted-claim gate -------------------------------------------------
#
# See the module docstring. No proximity windows: a dead claim is legal ONLY
# inside a quarantine block.

QUOTE_BEGIN = "REFUTED-QUOTE-BEGIN"
QUOTE_END = "REFUTED-QUOTE-END"
MAX_QUOTE_SPAN = 400        # flattened chars in ONE quarantine block
MAX_QUOTE_TOTAL = 1200      # flattened chars quarantined in ONE document
REFUTATION_LABEL = re.compile(
    r"(?i)refut|is false|claim is false|wrongly|revision 1|first revision|"
    r"earlier revision")

# Each entry: (id, subject, constancy-or-None, rescue-or-None, what).
#   subject    -- what the sentence is about
#   constancy  -- the assertion that the subject does not vary / is unique.
#                 None when the subject pattern already IS the whole assertion.
#   rescue     -- names the exception that makes a SCOPED version of the
#                 sentence true; None means the claim is dead in every form
REFUTED_CLAIMS = (
    ("post-conid-invariance",
     r"(?i)(?:after|following|past|beyond)\s+the\s+"
     r"(?:Con\.?\s?ID|CONID|connection[- ]identifier)s?(?:\s+pair)?"
     r"|\[58:62\]|\[60:62\]",
     # NB: `.` and not `[^.]` -- the subject itself contains a full stop
     # ("Con.ID"), and a character class excluding it made this whole family
     # unmatchable. That bug let all four REASSERT mutants through the first
     # cut of this rewrite; tests/vmsscs/test_scs_reason_mutants.py caught it.
     # Scanning is already per-sentence, so `.` cannot run past the claim.
     r"(?i)nothing.{0,60}?var|does not vary|do not vary|never var|"
     r"invarian|is constant|are constant|constant in (?:all|every)|"
     r"unchanging|identical in every frame|"
     r"only.{0,40}?(?:bytes|words|fields).{0,30}?(?:that )?var",
     r"(?i)var(?:ies|ying|y)\s+(?:0/1\s+)?on\s+DISCONNECT_REQ|"
     r"0x0000\s*[:x×]\s*131",
     "payload[60:62] VARIES on DISCONNECT_REQ -- 0x0000 x131 / 0x0001 x89, "
     "the CENSUS-A off=60 row for msgtype 6"),

    ("only-always-zero-slot",
     r"(?i)only\s.{0,80}?(?:slot|word|field|halfword).{0,90}?zero in 100%"
     r"|only\s.{0,40}?always-zero\s+(?:slot|word|field)"
     r"|only\s(?:16-bit\s)?(?:slot|word|field).{0,80}?"
     r"(?:always|never non-?)zero",
     None,                          # the pattern above IS the assertion
     r"(?i)at or after payload \d+|(?:not|no longer|isn't|is not) the only",
     "'the only slot zero in 100% of observed frames' -- ACCEPT_RSP sets "
     "payload[58:60] on the identical 62-byte layout (CENSUS-B len=62 type=3)"),

    ("only-varying-is-seq-ack",
     r"(?i)only.{0,40}?(?:varying\s+)?bytes.{0,30}?are the sequence/ack"
     r"|only.{0,40}?(?:bytes|words|fields) that vary are",
     None,
     None,
     "'the only varying bytes are the sequence/ack fields and the Con.ID pair'"),
)


def quarantine_spans(flat):
    """[(start, end)] of the REFUTED-QUOTE blocks, plus a list of structural
    complaints. Spans are half-open over `flat` and INCLUDE the markers."""
    problems = []
    marks = [(m.start(), m.end(), m.group(0))
             for m in re.finditer("%s|%s" % (QUOTE_BEGIN, QUOTE_END), flat)]
    spans, open_at = [], None
    for start, end, tok in marks:
        if tok == QUOTE_BEGIN:
            if open_at is not None:
                problems.append("nested %s at offset %d" % (QUOTE_BEGIN, start))
            open_at = start
        else:
            if open_at is None:
                problems.append("%s at offset %d with no %s"
                                % (QUOTE_END, start, QUOTE_BEGIN))
                continue
            spans.append((open_at, end))
            open_at = None
    if open_at is not None:
        problems.append("unterminated %s at offset %d" % (QUOTE_BEGIN, open_at))
    return spans, problems


# A DATA LINE is a machine-readable record, not prose: a markdown table row (or
# its separator), or a line that IS a CENSUS / REFUTATION-FACT record. Matched
# against the already-flattened line, so the ` * ` comment decoration and the
# markdown emphasis are gone by the time it is tested. Deliberately anchored:
# prose that MENTIONS "the CENSUS-A off=60 line" is not a data line, so this
# never chops a prose sentence into pieces the claim scan could slip between.
DATA_LINE = re.compile(r"^\|.*\|$|^(?:CENSUS-[A-Z]|REFUTATION-FACT)\b")


def flatten(text):
    """(flat, breaks) -- the document on one line, plus HARD UNIT BOUNDARIES.

    Why this is not just re.sub(r"\\s+", " ", text), which is what round 3 did:
    the CENSUS markdown tables and the REFUTATION-FACT lines contain NO
    sentence-ending punctuation, so a naive flatten glues each data block to the
    paragraph that FOLLOWS it into one giant "sentence". A rescue token sitting
    in the DATA then excuses a dead claim re-asserted in that paragraph --
    `0x0000 x 131` in the CENSUS-A row, `0x0000:131` in REFUTATION-FACT,
    `at or after payload 50` in the CENSUS-C row.

    Review round 4 MEASURED that: sweeping the dead claim into every
    paragraph-leading site and running the real gate, 246 of 248 spec sites and
    46 of 48 header sites died -- and the four survivors were exactly the four
    paragraphs that follow a data block, one of them the head of sec 4(h)(1a)'s
    own paragraph, i.e. the most natural place for the claim to come back.

    So units never merge across a data line or a blank line. A prose sentence
    never spans either, so nothing that was scanned as one sentence is split.
    """
    text = text.replace("*", "").replace("`", "")
    out, breaks, pos, prev_data = [], [], 0, False
    for raw in text.splitlines():
        line = re.sub(r"\s+", " ", raw).strip()
        if not line:
            # A blank line ends a paragraph. Headings and list items do not end
            # in a full stop either, so without this they would glue to the
            # paragraph below exactly as the tables did.
            prev_data = True
            continue
        is_data = DATA_LINE.search(line) is not None
        if out:
            out.append(" ")
            pos += 1
        if is_data or prev_data:
            breaks.append(pos)
        out.append(line)
        pos += len(line)
        prev_data = is_data
    return "".join(out), breaks


def sentences(flat, breaks=()):
    """(offset, text) for each SCAN UNIT: sentences, split additionally at every
    hard unit boundary so a data block can never merge into neighbouring prose."""
    cuts = sorted({0, len(flat)} | {b for b in breaks if 0 < b < len(flat)})
    out = []
    for a, b in zip(cuts, cuts[1:]):
        pos = a
        for piece in re.split(r"(?<=[.!?])\s+", flat[a:b]):
            out.append((pos, piece))
            pos += len(piece) + 1
    return out


def parse_spec_zero_slots(spec):
    """§5 CENSUS-C table: per-msgtype slot lists plus the 'at or after' row."""
    rows = md_table_after(spec, "<!-- CENSUS-C:")
    if rows is None:
        return None, None
    per, after = {}, None
    for cells in rows:
        if len(cells) != 2:
            return "MALFORMED ROW: %r" % (cells,), None
        slots = tuple(int(x) for x in re.findall(r"\d+", cells[1]))
        head = cells[0].replace("`", "").replace("*", "")
        m = re.search(r"at or after payload (\d+)", head)
        if m:
            after = (int(m.group(1)), slots)
        else:
            per[int(re.search(r"\d+", head).group(0))] = slots
    return per, after


# ---------------------------------------------------------------------------

def main():
    mod = load_measure()
    exp = mod.EXPECTED
    header = open(HEADER).read()
    spec = open(SPEC).read()

    # ---- 0. EXPECTED must still be internally consistent -------------------
    for t, c in exp["carriers"].items():
        eq(c["off58"], {0: c["frames"]},
           "EXPECTED msgtype %d: [58:60] is no longer zero in 100%% of the "
           "population. The whole placement rationale rests on that -- "
           "re-derive with tools/cluster/scs_reason_measure.py before editing "
           "any prose." % t)
    eq(exp["zero_slots_after_counters"], (mod.CONID_PAYLOAD_END,),
       "EXPECTED zero_slots_after_counters no longer names the OVMX slot "
       "(payload %d); the surviving 'only' claim is void" % mod.CONID_PAYLOAD_END)
    same_layout = sorted(k for k, v in exp["neighbours"].items()
                         if k[0] == 62 and k[1] not in exp["carriers"] and v["nonzero58"])
    check(same_layout,
          "EXPECTED records NO 62-byte neighbour setting [58:60]. The corrected "
          "rationale is built on that fact (ACCEPT_RSP); re-derive it before "
          "relaxing this gate.")

    # ---- 1. scs_reason.h CENSUS lines --------------------------------------
    h_car, h_nb, h_zs, h_after = parse_header(header)
    eq(h_car, {t: dict(c) for t, c in exp["carriers"].items()},
       "scs_reason.h: the CENSUS-A lines do not match the measurement")
    eq(h_nb, {k: dict(v) for k, v in exp["neighbours"].items()},
       "scs_reason.h: the CENSUS-B neighbour census does not match the "
       "measurement -- this census IS the refutation of the 'only slot zero in "
       "100%' rationale, so it must be exact")
    eq(h_zs, {t: tuple(v) for t, v in exp["zero_slots"].items()},
       "scs_reason.h: the CENSUS-C always-zero slot lists do not match")
    eq(h_after, (mod.COUNTER_PAYLOAD_END, tuple(exp["zero_slots_after_counters"])),
       "scs_reason.h: the CENSUS-C 'common at or after payload %d' line -- the "
       "only surviving 'only' claim -- is missing or wrong"
       % mod.COUNTER_PAYLOAD_END)

    # ---- 2. spec §4(h)(1a) CENSUS-A table ----------------------------------
    s_car = parse_spec_carriers(spec)
    check(isinstance(s_car, dict),
          "cluster-protocol-spec.md: the §4(h)(1a) CENSUS-A table is missing or "
          "malformed (%r)" % (s_car,))
    if isinstance(s_car, dict):
        eq(s_car, {t: dict(c) for t, c in exp["carriers"].items()},
           "cluster-protocol-spec.md §4(h)(1a): the CENSUS-A table does not "
           "match the measurement. The [60:62] row for msgtype 6 is the figure "
           "the refuted 'nothing after the Con.ID pair varies' claim "
           "contradicted -- it must show the split.")
        check(len(s_car.get(6, {}).get("off60", {})) > 1,
              "cluster-protocol-spec.md §4(h)(1a): payload[60:62] on "
              "DISCONNECT_REQ no longer shows more than one value. It VARIES; "
              "a single-valued cell restates the refuted claim.")

    # ---- 3. spec §5 CENSUS-B / CENSUS-C tables -----------------------------
    s_nb = parse_spec_neighbours(spec)
    check(isinstance(s_nb, dict),
          "cluster-protocol-spec.md: the §5 CENSUS-B table is missing or "
          "malformed (%r)" % (s_nb,))
    if isinstance(s_nb, dict):
        eq(s_nb, {k: dict(v) for k, v in exp["neighbours"].items()},
           "cluster-protocol-spec.md §5: the CENSUS-B neighbour table does not "
           "match the measurement")
    s_zs, s_after = parse_spec_zero_slots(spec)
    check(isinstance(s_zs, dict),
          "cluster-protocol-spec.md: the §5 CENSUS-C table is missing or "
          "malformed (%r)" % (s_zs,))
    if isinstance(s_zs, dict):
        eq(s_zs, {t: tuple(v) for t, v in exp["zero_slots"].items()},
           "cluster-protocol-spec.md §5: the CENSUS-C always-zero slot table "
           "does not match")
    eq(s_after, (mod.COUNTER_PAYLOAD_END, tuple(exp["zero_slots_after_counters"])),
       "cluster-protocol-spec.md §5: the CENSUS-C 'at or after payload %d' row "
       "is missing or wrong -- it is the only surviving 'only' claim"
       % mod.COUNTER_PAYLOAD_END)

    # ---- 3b. the SDA oracle (CENSUS-D), identical line in both documents ----
    docs = {"scs_reason.h": header, "cluster-protocol-spec.md": spec}
    eq(exp["sda"]["values"], {0: exp["sda"]["cdts"]},
       "EXPECTED: the SDA 'Rej/Disconn Reason' field is no longer zero on every "
       "CDT. That is the second oracle for 'no reason code was ever observed' -- "
       "the ungroundedness claim must be revisited, not the prose.")
    for name, text in docs.items():
        eq(parse_sda(text), exp["sda"],
           "%s: the CENSUS-D SDA-oracle line does not match the measurement" % name)
        eq(parse_population(text),
           (tuple(mod.CONNCTL_CLASSES), exp["n_captures"]),
           "%s: the CENSUS-P population line does not match the script's own "
           "CONNCTL_CLASSES / capture-set size" % name)
        # Every neighbour row must be inside the declared length classes --
        # otherwise CENSUS-P is describing a different population from CENSUS-B.
        stray = sorted({k[0] for k in exp["neighbours"]} - set(mod.CONNCTL_CLASSES))
        check(not stray,
              "EXPECTED has neighbour rows at SCA lengths %r outside the "
              "population CENSUS-P declares" % (stray,))

    # ---- 3c. the POSITIVE facts that do the refuting, pinned in both docs ---
    # A contradicting sentence cannot coexist with these: they ARE the
    # measurement the dead claims deny, and they are compared to EXPECTED.
    for name, text in docs.items():
        v_fact, n_fact = parse_refutation_facts(text)
        check(v_fact is not None,
              "%s: the REFUTATION-FACT off=60 line is missing. It carries the "
              "fact that kills 'nothing after the Con.ID pair varies'; without "
              "it the document no longer states its own refutation" % name)
        if v_fact is not None:
            eq(v_fact["type"], 6,
               "%s: REFUTATION-FACT off=60 must be about msgtype 6 "
               "(DISCONNECT_REQ), the type the slot varies on" % name)
            eq(v_fact["values"], dict(exp["carriers"][6]["off60"]),
               "%s: the REFUTATION-FACT off=60 histogram does not match the "
               "measurement" % name)
            eq(v_fact["distinct"], len(exp["carriers"][6]["off60"]),
               "%s: REFUTATION-FACT off=60 distinct_values disagrees with its "
               "own histogram / with EXPECTED" % name)
            check(v_fact["distinct"] > 1,
                  "%s: REFUTATION-FACT off=60 now claims a SINGLE value, which "
                  "IS the refuted claim in machine-readable form" % name)
        check(n_fact is not None,
              "%s: the REFUTATION-FACT off=58 line is missing. It carries the "
              "fact that kills 'the only slot zero in 100%% of observed "
              "frames'" % name)
        if n_fact is not None:
            eq((n_fact["len"], n_fact["type"]), (62, 3),
               "%s: REFUTATION-FACT off=58 must name the 62-byte neighbour "
               "(ACCEPT_RSP, type 3) that shares the carriers' layout" % name)
            want = exp["neighbours"][(62, 3)]
            eq((n_fact["name"], n_fact["nonzero"]),
               (want["name"], want["nonzero58"]),
               "%s: the REFUTATION-FACT off=58 neighbour figures do not match "
               "the measurement" % name)
            check(n_fact["nonzero"] > 0,
                  "%s: REFUTATION-FACT off=58 now reports the neighbour never "
                  "setting the slot, which IS the refuted claim" % name)
            check(n_fact["type"] not in exp["carriers"],
                  "%s: REFUTATION-FACT off=58 names a CARRIER as the "
                  "counter-example; the whole point is that it is a NEIGHBOUR "
                  "on the same layout" % name)

    # ---- 4. the REFUTED claims must stay dead -------------------------------
    #
    # Round 2 did this with a proximity window and it did not work: both
    # documents are about the refutation, so the excuse words are everywhere
    # and three natural re-assertion sites stayed green. There is no window
    # here. A dead claim is legal in exactly one place -- inside a quarantine
    # block -- and the blocks are themselves constrained so that quarantining
    # the whole section is not a way out.
    for name, text in docs.items():
        flat, breaks = flatten(text)
        spans, problems = quarantine_spans(flat)
        for p in problems:
            check(False, "%s: malformed %s block -- %s" % (name, QUOTE_BEGIN, p))
        total = 0
        for a, b in spans:
            # The INTERIOR, with the markers excluded. `REFUTED-QUOTE-BEGIN`
            # itself contains "REFUT", so testing the whole span for a
            # refutation label made that check vacuously true -- the battery's
            # QUARANTINE-no-refutation-label mutant survived on exactly this.
            body = flat[a + len(QUOTE_BEGIN):b - len(QUOTE_END)]
            total += b - a
            check(b - a <= MAX_QUOTE_SPAN,
                  "%s: a %s block is %d flattened chars (limit %d). Quarantine "
                  "the QUOTATION, not the argument around it -- an oversized "
                  "block is how a dead claim gets licensed wholesale"
                  % (name, QUOTE_BEGIN, b - a, MAX_QUOTE_SPAN))
            check(REFUTATION_LABEL.search(body) is not None,
                  "%s: a %s block does not say the claim inside it is refuted. "
                  "Quarantine is for quoting a dead claim in order to kill it, "
                  "not for parking it: %r" % (name, QUOTE_BEGIN, body[:120]))
            # No MEASUREMENT may sit inside quarantine -- tested by running the
            # parsers on the block itself rather than by grepping for a marker
            # word, so a prose mention of "the CENSUS-A off=60 line" is fine
            # while an actual data line is not, and this stays correct if a
            # census line ever changes shape.
            swallowed = (any(parse_header(body)[:3])
                         or parse_header(body)[3] is not None
                         or parse_population(body) is not None
                         or parse_sda(body) is not None
                         or parse_refutation_facts(body) != (None, None))
            check(not swallowed,
                  "%s: a %s block swallows a CENSUS / REFUTATION-FACT data "
                  "line. The measurement must never sit inside quarantine: %r"
                  % (name, QUOTE_BEGIN, body[:120]))
        check(total <= MAX_QUOTE_TOTAL,
              "%s: %d flattened chars are quarantined (limit %d) -- the "
              "exemption has grown into a second document"
              % (name, total, MAX_QUOTE_TOTAL))

        # Scan every sentence. A hit is excused ONLY when the whole matched
        # claim -- subject through assertion -- lies inside a quarantine block.
        # Deliberately NOT "the sentence overlaps a block": that would let a
        # two-word quarantined aside license the sentence around it.
        scanned = 0
        for off, sent in sentences(flat, breaks):
            scanned += 1
            for cid, subject, constancy, rescue, what in REFUTED_CLAIMS:
                sm = re.search(subject, sent)
                if sm is None:
                    continue
                cm = re.search(constancy, sent) if constancy else sm
                if cm is None:
                    continue
                if rescue is not None and re.search(rescue, sent):
                    continue
                lo = off + min(sm.start(), cm.start())
                hi = off + max(sm.end(), cm.end())
                if any(a <= lo and hi <= b for a, b in spans):
                    continue
                check(False,
                      "%s: the REFUTED claim [%s] is asserted again -- %s\n"
                      "       matched: %r\n"
                      "       If you are QUOTING the dead claim in order to "
                      "kill it, put it in a %s block."
                      % (name, cid, what, flat[lo:hi][:220], QUOTE_BEGIN))
        check(scanned > 20,
              "%s: only %d sentences reached the refuted-claim scan. Either "
              "the document collapsed or quarantine swallowed it"
              % (name, scanned))

    # ---- 5. structure ------------------------------------------------------
    for name, text in docs.items():
        check("tools/cluster/scs_reason_measure.py" in text,
              "%s: no pointer to the re-derivation script" % name)
        check(str(exp["n_captures"]) in text,
              "%s: the capture-set size %d is no longer stated"
              % (name, exp["n_captures"]))
        check("ACCEPT_RSP" in text,
              "%s: ACCEPT_RSP is not named -- it is the message type that shares "
              "the 62-byte layout and DOES set payload[58:60]" % name)
    # The LABEL, checked where it is load-bearing: next to the offset macro in
    # the header, and next to the macro's name in the spec. A whole-file search
    # would pass on any of the other four occurrences.
    m = re.search(r"([^\n]*\n[^\n]*\n[^\n]*)#define\s+SCS_REASON_PAYLOAD_OFF\s+(\d+)",
                  header)
    check(m is not None, "scs_reason.h: SCS_REASON_PAYLOAD_OFF is gone")
    if m:
        eq(int(m.group(2)), mod.CONID_PAYLOAD_END,
           "scs_reason.h: SCS_REASON_PAYLOAD_OFF disagrees with the measured "
           "slot; every figure in this gate is about that payload offset")
        check(re.search(r"(?i)labell?ed OVMX design choice", m.group(1)) is not None,
              "scs_reason.h: the comment ON the SCS_REASON_PAYLOAD_OFF macro no "
              "longer LABELS it an OVMX design choice -- it is not a decoded VMS "
              "field and must never read as one")
    m = re.search(r"SCS_REASON_PAYLOAD_OFF = 58`?\s*\n?[^\n]*\n?[^\n]*", spec)
    check(m is not None and
          re.search(r"(?i)labell?ed OVMX design choice", m.group(0)) is not None,
          "cluster-protocol-spec.md: the SCS_REASON_PAYLOAD_OFF ruling no longer "
          "LABELS the placement an OVMX design choice")

    # ---- N. THE WIRE ITSELF (vms-371) -------------------------------------
    # Everything above pins the PROSE to EXPECTED. This pins EXPECTED to the
    # PACKETS. Together they are what makes a green run mean "the prose matches
    # a re-derived measurement" instead of "the prose matches a table that also
    # drifted".
    scs_wire.gate("scs_reason_figures", mod, mod.DEFAULT_CAPDIR, check)

    for f in failures:
        print("FAIL %s" % f)
    print("%d checks, %d failures" % (checks, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
