#!/usr/bin/env python3
"""
test_scs_type10_figures.py -- vms-4eb: the SCA message-type-10 decode must keep
saying what the captures say, and must keep saying where it stops.

WHAT IT PINS. `docs/cluster-protocol-spec.md` sec 4(h)(1e) (the decode) and the
sec 5 register entries it opens, against the EXPECTED table in
`tools/cluster/scs_type10_measure.py` -- and, on a host that has the host-only
lab-1 captures, EXPECTED itself against the packets via `scs_wire.gate()`
(vms-371). Without the captures it says so loudly and still runs every
host-independent check; `OVMX_SCS_REQUIRE_WIRE=1` turns the absence into a
failure.

WHY IT EXISTS, in the specific terms of what this epic keeps getting wrong.

  (1) A TAXON GOT RECORDED AS A DECODE. sec 4(h)(1a) carried type 10 as "the SCS
      application-message MTYPE" -- true, and it says nothing about what the
      message contains. The 2,889-frame 110-content population stayed the
      largest unknown on our wire for another two weeks behind that sentence.
      So this gate requires the decode's LOAD-BEARING NUMBERS, not its adjectives.

  (2) A CENSUS RESTRICTED ITS OWN POPULATION (vms-c11) and the spec published a
      false absence. The measurement tool must therefore still run
      length-unrestricted and still call census_guard.check_census() with a
      written reason. Both are checked here, and a defined-but-uncalled guard
      fails.

  (3) OVMX'S OWN FRAMES GOT COUNTED AS EVIDENCE ABOUT VMS. The tool's OUI rule
      must still REFUSE an unplaceable source rather than silently choosing a
      population -- this gate calls classify_source() with a foreign MAC and
      requires None, and requires the comparison to red when the unclassified
      count is non-zero.

  (4) A WIRE CLAIM WAS WRITTEN AS "GROUNDED" WITH NOTHING BEHIND IT, and it was
      WRONG. So the decode's own admitted gaps -- body[48:52] and unit-flags bit
      15 -- must remain stated as gaps in sec 5. A future edit that quietly
      names either of them reds here.

WHAT IT DOES NOT DO: on a host without the captures it cannot tell you the
measurement is right. It tells you the documents still say what the measurement
said, and that the measurement tool still refuses the things it must refuse.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import scs_wire                                                    # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SPEC = os.environ.get("OVMX_TYPE10_SPEC",
                      os.path.join(ROOT, "docs/cluster-protocol-spec.md"))
MEASURE = os.environ.get("OVMX_SCS_TYPE10_MEASURE",
                         os.path.join(ROOT,
                                      "tools/cluster/scs_type10_measure.py"))
CTEST = os.environ.get("OVMX_TYPE10_CTEST",
                       os.path.join(ROOT, "tests/vmsscs/test_scs_mscp.c"))

failures = []
checks = 0


def check(cond, msg):
    global checks
    checks += 1
    if not cond:
        failures.append(msg)


def read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def digits(text):
    """Spec prose writes 2 889 / 2,889 / 2889 interchangeably. Compare on the
    digit stream so a thousands separator is never what a figures gate turns
    on."""
    return re.sub(r"(?<=\d)[  ,](?=\d)", "", text)


MOD = scs_wire.load_source(MEASURE, "scs_type10_measure")
EXP = MOD.EXPECTED
spec = read(SPEC)
spec_d = digits(spec)


def section(text, heading_re, stop_re=r"^(\*\*\(\d|### |## )"):
    m = re.search(heading_re, text, re.M)
    check(m is not None, "docs/cluster-protocol-spec.md has no %r -- the "
                         "vms-4eb decode is not in the spec at all" % heading_re)
    if m is None:
        return ""
    rest = text[m.end():]
    n = re.search(stop_re, rest, re.M)
    return rest[:n.start()] if n else rest


SEC = section(spec, r"^\*\*\(1e\) THE 110-CONTENT TYPE-10 CLASS")
SEC_D = digits(SEC)

# ===========================================================================
# 1. THE IDENTIFICATION -- the numbers the decode rests on
# ===========================================================================
for label, value in (
        ("110-content type-10 frames", EXP["gus_end_frames"]),
        ("frames paired to their command", EXP["gus_end_matched"]),
        ("frames with a bound SYSAP pair",
         EXP["gus_end_sysap_pairs"][("VMS$DISK_CL_DRVR", "MSCP$DISK")]),
        ("frames with no connect frame in-capture", EXP["gus_end_sysap_unbound"]),
        ("valid-unit frames", EXP["valid_unit_frames"])):
    check(str(value) in SEC_D,
          "sec 4(h)(1e) does not carry the %s (%d) -- the decode's own "
          "population" % (label, value))

check(EXP["gus_end_unmatched"] == 0 and re.search(r"[Uu]nmatched:?\*?\*?\s*0",
                                                  SEC) is not None,
      "sec 4(h)(1e) no longer states that ZERO of the 110-content type-10 "
      "frames failed to pair with an MSCP command; the pairing is the whole "
      "identification and 'most of them' would not be one")

# The opcode relation, which is what makes it an END message rather than a
# command that happens to be nearby.
for cmd, end in EXP["opcode_pairing"]:
    check(("0x%02x" % cmd) in SEC and ("0x%02x" % end) in SEC,
          "sec 4(h)(1e) does not carry the command->endcode pair 0x%02x -> "
          "0x%02x" % (cmd, end))
    check(end == cmd | MOD.MSCP_OP_END,
          "EXPECTED records an endcode 0x%02x that is not command|OP.END for "
          "0x%02x -- the identification does not hold in its own table"
          % (end, cmd))
check(re.search(r"OP\.END", SEC) is not None and "A-1" in SEC,
      "sec 4(h)(1e) no longer cites Table A-1 / OP.END, the public-spec rule "
      "that makes 0x83 an end message rather than an unknown opcode")
check("MSCP$DISK" in SEC and "VMS$DISK_CL_DRVR" in SEC,
      "sec 4(h)(1e) no longer names the SYSAP pair that owns the connection")

# ===========================================================================
# 2. THE UNRESTRICTED CENSUS -- the vms-c11 discipline, as figures
# ===========================================================================
# Every length class the census found must appear in the section's table, not
# just the one the decode is about. A census that publishes only its own
# subject is the failure this epic already made.
# Matched as a whole TABLE ROW, not as a loose digit: counts collide across
# this section (202 is both the 86-content class and the SCC command count),
# so a substring check stayed green with a whole census row deleted. Measured
# -- that mutant survived the first version of this check.
for (ln, mt), n in sorted(EXP["vax_census"].items()):
    row = r"\|\s*\*{0,2}%d\*{0,2}\s*\|\s*\*{0,2}%d\*{0,2}\s*\|\s*\*{0,2}%d\*{0,2}\s*\|" \
          % (ln, mt, n)
    check(re.search(row, SEC_D) is not None,
          "sec 4(h)(1e)'s census table has lost the row (%d-content, type %d, "
          "%d frames) -- the census must stay unrestricted in the PROSE too, "
          "or a reader cannot see what was in scope" % (ln, mt, n))
for ln, n in sorted(EXP["type10_lengths_vax"].items()):
    check(str(ln) in SEC_D,
          "sec 4(h)(1e) no longer records that type 10 also occupies the "
          "%d-content class -- type 10 is a carrier, not a class" % ln)
check(set(EXP["type10_lengths_ovmx"]) == {94, 190},
      "EXPECTED's OVMX type-10 lengths moved; the claim 'OVMX has never emitted "
      "a 110-content type-10 frame' is measured, not assumed: %s"
      % EXP["type10_lengths_ovmx"])
check(110 not in EXP["type10_lengths_ovmx"],
      "OVMX now emits a 110-content type-10 frame. That is an MSCP END message "
      "-- see the sec 4(h)(1e) ruling; it is gated behind "
      "docs/design-mscp-direction.md Phase D and must not appear without one.")

# ===========================================================================
# 3. THE SYSAP FILTER -- the category error, measured in both directions
# ===========================================================================
for op, n in sorted(EXP["body8_mscp_connection"].items()):
    check(str(n) in SEC_D and ("0x%02x" % op) in SEC,
          "sec 4(h)(1e) does not carry body[8]=0x%02x x%d on the MSCP "
          "connection" % (op, n))
for op, n in sorted(EXP["body8_directory_connection"].items()):
    check(str(n) in SEC_D and ("0x%02x" % op) in SEC,
          "sec 4(h)(1e) does not carry body[8]=0x%02x x%d on the DIRECTORY "
          "connections -- the contrast is what makes the filter necessary "
          "rather than tidy" % (op, n))
check(EXP["body8_mscp_all_table_a1"] is True
      and EXP["body8_directory_any_table_a1"] is False,
      "EXPECTED no longer records that body[8] is a Table A-1 opcode on the "
      "MSCP connection and on NO directory frame")
# Twice: once naming the measurement, once delivering the verdict on an
# UNFILTERED census. One occurrence survives deleting the other.
check(len(re.findall(r"category\s+error", SEC)) >= 2,
      "sec 4(h)(1e) no longer both measures the category error AND says that "
      "an unfiltered body[8] census is one; that verdict is the reusable "
      "lesson, not a flourish")
# The two ASYMMETRIC pairs are what establish the field roles, and they must
# appear AS PAIRS with their counts -- both names occur elsewhere in the
# section on their own, so an "is each name somewhere" check stayed green with
# the pair itself deleted. Measured -- that mutant survived the first version.
for (mt, dname, sname), n in EXP["connect_name_pairs"].items():
    if dname == sname or mt != 0:
        continue
    pair = re.compile(r"%s[^)]{0,12}%s`?\)\s*%d" % (re.escape(dname),
                                                    re.escape(sname), n))
    check(pair.search(SEC_D) is not None,
          "sec 4(h)(1e) no longer carries the CONNECT_REQ name pair (%s, %s) "
          "x%d as a pair with its count -- that pairing, and its swap on the "
          "ACCEPT_REQ, is the whole evidence for which 16-byte field is the "
          "sender's, and the Con.ID binding of the decode rests on it"
          % (dname, sname, n))
# ...and the OTHER half of that evidence: the ACCEPT_REQ carries the same two
# strings the other way round. Without the swap, "one field happens to hold the
# listener's name" is a coincidence, not a role.
check(re.search(r"swapped", SEC) is not None,
      "sec 4(h)(1e) no longer states that the ACCEPT_REQ carries the two SYSAP "
      "name strings SWAPPED. A single direction cannot distinguish a role from "
      "a coincidence, and the Con.ID binding of the whole decode rests on it.")
for (mt, dname, sname), n in EXP["connect_name_pairs"].items():
    if dname == sname or mt != 2:
        continue
    check(re.search(r"%d\s*×" % n, SEC_D) is not None or str(n) in SEC_D,
          "sec 4(h)(1e) no longer carries the ACCEPT_REQ swap count %d for "
          "(%s, %s)" % (n, dname, sname))

# ===========================================================================
# 4. THE FIELD DECODE -- Table A-7 names and the values behind them
# ===========================================================================
for sym in ("P.CRF", "P.UNIT", "P.OPCD", "P.FLGS", "P.STS", "P.MLUN", "P.UNFL",
            "P.UNTI", "P.MEDI", "P.SHUN"):
    check(sym in SEC,
          "sec 4(h)(1e) no longer names the Table A-7 field %s -- 'we decoded "
          "it' without the published symbol is not traceable to the spec" % sym)
for st, n in sorted(EXP["status_census"].items()):
    check(("0x%02x" % st) in SEC and str(n) in SEC_D,
          "sec 4(h)(1e) does not carry status 0x%02x x%d" % (st, n))
check(EXP["status_unitid_residuals"] == 0,
      "the sec 6.12 validity partition (unit identifier zero iff plain "
      "Unit-Offline) now has %d residual(s); the decode states it as exact"
      % EXP["status_unitid_residuals"])
check(re.search(r"zero residuals", SEC) is not None,
      "sec 4(h)(1e) no longer states the unit-identifier partition is exact; a "
      "partition without its residual count is a correlation")
check(EXP["reserved_violations_valid"] == 0,
      "EXPECTED now records %d reserved-field violation(s); AA-L619A-TK sec 5.2 "
      "requires zero and the decode says the wire agrees"
      % EXP["reserved_violations_valid"])

for unit, row in sorted(EXP["unit_table"].items()):
    _uid, media_id, media, _mlun, _flags, _n = row
    check(("0x%04x" % unit) in SEC,
          "sec 4(h)(1e)'s unit table has lost unit 0x%04x" % unit)
    check(("0x%08x" % media_id) in SEC,
          "sec 4(h)(1e) has lost media type identifier 0x%08x" % media_id)
    check(media.split()[-1] in SEC,
          "sec 4(h)(1e) has lost the decoded media name %r -- the decode's "
          "independent check is that it matches the lab's own vax.ini" % media)
    check(MOD.media_name(media_id) == media,
          "the tool's media_name() no longer decodes 0x%08x to %r"
          % (media_id, media))
ra80_val, ra80_name = EXP["media_name_ra80_check"]
check(MOD.media_name(ra80_val) == ra80_name,
      "media_name() no longer reproduces Appendix C Table C-3's published RA80 "
      "value 0x%08x -- the decoder's only calibration against the public spec"
      % ra80_val)
check(("0x%08x" % ra80_val).lower() in SEC.lower(),
      "sec 4(h)(1e) no longer quotes Appendix C Table C-3's published worked "
      "value RA80 = 0x%08x. Without it the media-type decode is an encoding "
      "the reader has to take on trust; with it, it is calibrated." % ra80_val)
check("vax.ini" in SEC and "ra92" in SEC,
      "sec 4(h)(1e) no longer cites the lab's own SIMH configuration; the "
      "media-type decode's independent confirmation is that the wire and a "
      "config file written years earlier agree with no free parameter")

for (mod_word, unit), n in sorted(EXP["next_unit_walk"].items()):
    if mod_word & MOD.MSCP_MD_NXU:
        check(("0x%04x" % unit) in SEC,
              "sec 4(h)(1e) no longer records the Next Unit walk reaching unit "
              "0x%04x (%d frames)" % (unit, n))
check("MD.NXU" in SEC and "Next Unit" in SEC,
      "sec 4(h)(1e) no longer names BOTH sec 6.12's 'Next Unit' modifier and "
      "its Table A-2 symbol MD.NXU. The prose name without the symbol is not "
      "traceable to the public spec, and the symbol without the prose is not "
      "readable -- an `or` here let either be deleted silently.")
check(("0x%04x" % MOD.MSCP_MD_NXU) in SEC,
      "sec 4(h)(1e) no longer carries the Next Unit modifier VALUE 0x%04x, "
      "which is the byte the enumeration is actually read off"
      % MOD.MSCP_MD_NXU)

# ===========================================================================
# 5. THE GAPS MUST STAY GAPS
# ===========================================================================
REG = section(spec, r"^For visibility, every field NOT marked GROUNDED above:",
              stop_re=r"^## ")
check("body[48:52]" in REG or "body`[48:52]`" in REG or "`body[48:52]`" in REG,
      "the sec 5 register no longer carries the undecoded body[48:52] residue "
      "of the 110-content type-10 class")
check("0x006e" in REG and "0x006e" in SEC,
      "the constant 0x006e at body[48:50] is no longer recorded in both the "
      "decode and the sec 5 register")
# Matched as a phrase, not as the WORD "indistinguishable": that word appears
# elsewhere in sec 5 about an unrelated finding, so a bare word search here
# stayed green with this sentence deleted. Measured -- that mutant survived the
# first version of this check.
check(re.search(r"INDISTINGUISHABLE ON THIS\s+POPULATION", REG) is not None,
      "the sec 5 register no longer says a length-echo reading and a constant "
      "reading of body[48:50] are indistinguishable on this population -- that "
      "sentence is the whole bound on the ambiguity")
# In BOTH places. The decode section is where a reader meets the field, the
# register is where a reader looks for what is still open; a gap recorded in
# one of the two is a gap that gets closed by accident in the other.
for label, body in (("sec 4(h)(1e)", SEC), ("the sec 5 register", REG)):
    check(re.search(r"[Tt]able A-5 defines no bit 15", body) is not None,
          "%s no longer records that the unit-flags value 0x%04x sets a bit "
          "Table A-5 does not define"
          % (label, EXP["unit_flags_undocumented_bit"]))
check(re.search(r"[Dd]o not name (them|either field)", SEC + REG) is not None,
      "the explicit refusal to name body[48:52] is gone from both sec 4(h)(1e) "
      "and the sec 5 register. vms-4eb's constraint is that a name must not be "
      "guessed; the refusal is the thing that is supposed to survive.")
check("vms-5da" in REG,
      "the sec 5 register no longer cites the rd item filed for sec 4(N)'s "
      "'local SYSAP name' label; a finding recorded nowhere actionable is lost")

# A NAMED gap is a filled gap. Rather than enumerate the names somebody might
# invent -- "checksum", "trailer", "sequence" -- this requires the OPPOSITE and
# cannot be routed around: EVERY sentence anywhere in the spec that mentions
# the residue must also carry a refusal marker. An added sentence that
# explains what the bytes are will not carry one, so it reds.
#
# Granularity is the PARAGRAPH, deliberately. Sentence granularity would ban
# stating the measured value ("body[48:50] is a constant 0x006e"), which is a
# figure and must stay; paragraph granularity lets the figures live beside
# their refusal and still reds on a NEW paragraph that explains the bytes.
RESIDUE_MENTION = re.compile(r"body.?\[4[89]:5[02]\]|body.?\[50:52\]|bit 15", re.I)
REFUSAL = re.compile(
    r"NOT DECODED|not decoded|undecoded|do not name|does not define|defines no|"
    r"INDISTINGUISHABLE|indistinguishable|nothing (here|we hold) identifies|"
    r"no meaning|must be ignored|unexplained", re.I)
NAMED = re.compile(
    r"(is|are|=|means|holds|carries|encodes)\s+(the|a|an)?\s*"
    r"(frame\s+)?(checksum|crc|trailer|sequence number|message length|"
    r"length field|padding|magic|signature)", re.I)
for para in re.split(r"\n\s*\n", spec):
    if not RESIDUE_MENTION.search(para):
        continue
    checks += 1
    if not REFUSAL.search(para):
        failures.append(
            "a paragraph mentions the undecoded type-10 residue without saying "
            "anywhere in it that it is undecoded: %r. vms-4eb's constraint is "
            "explicit -- do not guess a name; this epic already shipped one "
            "'grounded' claim with zero observations behind it and it was "
            "WRONG." % " ".join(para.split())[:200])
    checks += 1
    m = NAMED.search(para)
    if m is not None:
        failures.append(
            "a paragraph about the undecoded type-10 residue now NAMES it "
            "(%r): %r" % (m.group(0), " ".join(para.split())[:200]))

# ===========================================================================
# 6. THE RULING ON EMITTING ONE
# ===========================================================================
check(re.search(r"Phase D", SEC) is not None,
      "sec 4(h)(1e)'s ruling no longer gates emitting a 110-content type-10 "
      "frame behind docs/design-mscp-direction.md Phase D")
check("INV-6" in SEC,
      "sec 4(h)(1e)'s ruling no longer says why emitting one ungated is the "
      "dishonest-success shape")
check(re.search(r"[Pp]ars", SEC) is not None and "scs_mscp_parse" in SEC,
      "sec 4(h)(1e) no longer records that PARSING this frame is required "
      "today -- the half of the ruling that is not a deferral")

# ===========================================================================
# 7. THE TOOL MUST KEEP REFUSING WHAT IT REFUSES
# ===========================================================================
src = read(MEASURE)
check("import census_guard" in src or "import census_guard" in src,
      "scs_type10_measure.py no longer imports census_guard (vms-69c)")
check("check_census(" in src,
      "scs_type10_measure.py imports census_guard but never calls "
      "check_census() -- a guard nobody calls is not a guard")
check("restrict_reason=TYPE10_RESTRICT_REASON" in src,
      "scs_type10_measure.py calls check_census() without its written "
      "restriction reason")
check(len(MOD.TYPE10_RESTRICT_REASON.strip()) > 40,
      "TYPE10_RESTRICT_REASON is too short to be a reason")
check("def lab1_only(" in src and "lab1_only(sorted(glob.glob(" in src,
      "scs_type10_measure.py has lost the lab-1 manifest fence, or defines it "
      "without wrapping its glob with it (vms-beb/vms-096)")

# The OUI rule: an unplaceable source is a FAILURE, not a shrug.
check(MOD.classify_source("08002b785 6b9".replace(" ", "")) == MOD.VAX,
      "classify_source() no longer places a DEC-OUI source as a VAX")
check(MOD.classify_source("aa00040001 04".replace(" ", "")) == MOD.VAX,
      "classify_source() no longer places a DECnet logical address as a VAX")
check(MOD.classify_source("b6168adc3a53") == MOD.OVMX,
      "classify_source() no longer places a known OVMX MAC as OVMX")
for foreign in ("0200deadbeef", "525400123456", "001122334455"):
    check(MOD.classify_source(foreign) is None,
          "classify_source(%r) placed an UNKNOWN source instead of refusing "
          "it. The rule must red on a source it cannot account for, or a new "
          "OVMX MAC silently rejoins the VAX population." % foreign)
# ...and the refusal must reach the COMPARISON, not just the classifier -- a
# classifier that returns None into a comparison that ignores it is not a
# refusal. This is run as a MATCHED PAIR: the clean control must be entirely
# green FIRST, so the poisoned arm's red is attributable to the poison and to
# nothing else. (Kill-switch discipline: confirm the counter moved, and confirm
# it was at zero before.)
#
# The subject is a REAL measurement dict wherever the captures are readable, so
# the probe cannot drift out of shape as measure() evolves; only on a host
# without them does it fall back to one assembled from EXPECTED, and the clean
# control is what proves that fallback is a well-formed measurement either way.
_capdir = scs_wire.capture_dir(MOD.DEFAULT_CAPDIR)
if _capdir:
    clean_probe = MOD.measure(_capdir)
    probe_origin = "measure(%s)" % _capdir
else:
    clean_probe = dict(EXP)
    clean_probe["n_captures"] = EXP["pcaps_scanned"]
    clean_probe["unclassified"] = {}
    clean_probe["unit_table"] = {u: {(r[0], r[1], r[2], r[3], r[4]): r[5]}
                                 for u, r in EXP["unit_table"].items()}
    probe_origin = "EXPECTED (no captures on this host)"

control_bad = [what for ok, what in MOD.compare_results(clean_probe) if not ok]
check(not control_bad,
      "MATCHED CONTROL FAILED: the unpoisoned probe built from %s does not "
      "compare clean, so the poisoned arm below would prove nothing about the "
      "OUI refusal: %r" % (probe_origin, control_bad))

poisoned = dict(clean_probe)
poisoned["unclassified"] = {"0200deadbeef": 1}       # a locally-administered MAC
poisoned_bad = [what for ok, what in MOD.compare_results(poisoned) if not ok]
check(any("unclassified" in w for w in poisoned_bad),
      "compare_results() passed a measurement carrying an unplaceable source "
      "MAC (probe from %s). The OUI rule must RED on an unknown source, or a "
      "new OVMX MAC silently rejoins the VAX population; measured failures: %r"
      % (probe_origin, poisoned_bad))
check(len(poisoned_bad) == len(control_bad) + 1,
      "poisoning ONLY the source classification changed %d checks, not 1 -- "
      "the refusal is not isolated to the thing under test"
      % (len(poisoned_bad) - len(control_bad)))

# The C-side field map must still exist, because it is the half of this decode
# that runs on a host without the capture library.
ctest = read(CTEST)
check("test_gus_end_field_map" in ctest and "AA-L619A-TK" in ctest,
      "tests/vmsscs/test_scs_mscp.c no longer carries the Table A-7 field-map "
      "test over the golden GUS-END frame -- without it the decode is only "
      "checkable on a lab host")

# ===========================================================================
# 8. THE WIRE ITSELF (vms-371)
# ===========================================================================
scs_wire.gate("scs_type10_figures", MOD, MOD.DEFAULT_CAPDIR, check)

for f in failures:
    print("FAIL " + f)
print("%s: %d checks, %d failure(s)"
      % ("FAIL" if failures else "PASS", checks, len(failures)))
sys.exit(1 if failures else 0)
