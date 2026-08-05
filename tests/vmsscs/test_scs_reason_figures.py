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

It does NOT re-derive the numbers from packets: the captures are host-only (not
in git) and only tools/cluster/scs_reason_measure.py on a lab host does that.

Precedent and shape: tests/vmsscs/test_scs_credit_figures.py (vms-76e).
"""

import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
HEADER = os.path.join(ROOT, "src", "vmsscs", "include", "scs_reason.h")
SPEC = os.path.join(ROOT, "docs", "cluster-protocol-spec.md")
MEASURE = os.path.join(ROOT, "tools", "cluster", "scs_reason_measure.py")

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
    """
    src = open(MEASURE).read()
    mod = importlib.util.module_from_spec(
        importlib.util.spec_from_loader("scs_reason_measure", loader=None))
    mod.__file__ = MEASURE
    exec(compile(src, MEASURE, "exec"), mod.__dict__)
    return mod


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

    # ---- 4. the two REFUTED sentences must stay dead ------------------------
    for name, text in docs.items():
        flat = re.sub(r"\s+", " ", text.replace("*", "").replace("`", ""))
        # Each refuted sentence may appear ONLY inside a passage that labels it
        # as refuted -- quoting the dead claim in order to kill it is fine,
        # asserting it is not.
        refuted = (
            (r"(?i)nothing after the Con\.?ID pair varies",
             "'nothing after the Con.ID pair varies' -- payload[60:62] varies "
             "on DISCONNECT_REQ, see CENSUS-A"),
            (r"(?i)only varying bytes are the sequence/ack fields",
             "'the only varying bytes are the sequence/ack fields and the "
             "Con.ID pair'"),
            (r"(?i)only (?:16-bit )?slot [^.]{0,80}zero in 100%",
             "'the only slot zero in 100% of observed frames' -- ACCEPT_RSP "
             "sets [58:60] on the same 62-byte layout"),
        )
        for pat, what in refuted:
            for m in re.finditer(pat, flat):
                window = flat[max(0, m.start() - 500):m.end() + 300]
                check(re.search(r"(?i)refut|is false|wrongly|revision 1|"
                                r"first revision|earlier revision", window) is not None,
                      "%s: the REFUTED claim %s is asserted again at %r"
                      % (name, what, m.group(0)))

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

    for f in failures:
        print("FAIL %s" % f)
    print("%d checks, %d failures" % (checks, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
