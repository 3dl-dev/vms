#!/usr/bin/env python3
"""
test_scs_join_capability_figures.py -- vms-70e2.

THREE JOBS, AND THE FIRST ONE IS THE ONLY ONE THAT RUNS REAL CODE.

(1) IT EXERCISES THE DECODER. tools/cluster/scs_join_capability_measure.py is
    what produced every figure in docs/cluster-protocol-spec.md sec 4(O). Its
    captures are host-only (rule 8), so ctest cannot re-run the measurement --
    but the measurement's LOGIC is not host-only. `measure_frames()` is driven
    here with synthesized (src, dst, sca) tuples whose right answer is known by
    construction: the 190-byte class counted per direction, the [46:48] message
    type read over the grounded length classes {58,62,66,110} and NOT over 94
    or 106 (reading those produced 1410/1412/1413 -- SCSSYSTEMIDs, not message
    types -- which is the mistake this gate exists to keep dead), peer-to-peer
    frames excluded, and the identity strings recovered from the bytes.

(2) IT PINS THE PROSE TO THE MEASUREMENT. Every figure in EXPECTED must still
    appear in the spec's sec 4(O), so the section cannot drift away from the
    numbers the script re-derives.

(3) IT QUARANTINES THE REFUTED CLAIM. scs_sdir.h used to justify a deliberate
    deviation from p. 2-50 partly on "OVMX HAS NEVER OBSERVED AN ACCEPT_RSP
    ADDRESSED TO ITSELF". Run A1 contains one, addressed to the OVMX MAC and
    carrying OVMX's own Con.ID pair, 0.5 ms after OVMX's ACCEPT_REQ. The dead
    sentence may be written down only inside a REFUTED-QUOTE-BEGIN/END block --
    the same discipline test_scs_disc_figures.py uses -- and nowhere else in the
    header or the spec.

(4) AND IF THE CAPTURES ARE PRESENT (a lab host), it runs the real measurement
    and requires it to PASS -- BOTH brackets: vms-70e2's (which binary joins)
    and vms-578's acceptance bracket (the three runs that JOIN, the evidence
    the whole SCA layer rests on). On a machine without them the absence is
    announced with a BANNER, not a parenthetical (vms-371: a quiet skip is how
    `ctest -L scs` stayed green while four measurement tools were red), and the
    three checks above still gate. OVMX_SCS_REQUIRE_WIRE=1 turns the absence
    itself into a failure.

WHAT THIS TEST DOES NOT CLAIM: nothing here says the missing ACCEPT_RSP causes
the failed join. Section 4(O) states the non-claim; this test only keeps the
measured numbers and the refutation from drifting.
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import scs_wire                                                    # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
# The three overrides exist so test_scs_join_capability_mutants.py can point
# this gate at scratch copies. Nothing under src/, docs/ or tools/ is ever
# written by either file.
MEASURE = os.environ.get("OVMX_SCS_JOINCAP_MEASURE",
                         os.path.join(ROOT, "tools", "cluster", "scs_join_capability_measure.py"))
SPEC = os.environ.get("OVMX_SCS_JOINCAP_SPEC",
                      os.path.join(ROOT, "docs", "cluster-protocol-spec.md"))
HEADER = os.environ.get("OVMX_SCS_JOINCAP_HEADER",
                        os.path.join(ROOT, "src", "vmsscs", "include", "scs_sdir.h"))

failures = []
checks = 0


def check(cond, msg):
    global checks
    checks += 1
    if not cond:
        failures.append(msg)


def load(path, name):
    """Execute the measure script FROM SOURCE, never from cached bytecode.

    importlib's source-file loader validates its cached .pyc on
    (mtime-in-SECONDS, size), so an edit that keeps the file the same length
    and lands in the same second as a previous run silently reuses the OLD
    bytecode -- i.e. the OLD EXPECTED / EXPECTED_578 -- and this gate reads
    both out of that module, so such a mutation would survive. The mutation
    battery for test_scs_reason_figures.py had exactly one survivor for this
    reason; this is the compile()+exec() form that fixed it, now shared by all
    six figures gates as scs_wire.load_source().
    """
    return scs_wire.load_source(path, name)


check(os.path.exists(MEASURE), "tools/cluster/scs_join_capability_measure.py is missing")
if not os.path.exists(MEASURE):
    print("FAIL: the measurement script is gone; nothing else can be checked")
    sys.exit(1)

M = load(MEASURE, "scs_join_capability_measure")
EXPECTED = M.EXPECTED

# ===========================================================================
# 1. THE DECODER, DRIVEN WITH FRAMES WHOSE ANSWER IS KNOWN BY CONSTRUCTION
# ===========================================================================
OVMX = "4e:83:cd:c4:fe:54"
PEER = "aa:00:04:00:01:04"
OTHER = "08:00:2b:a9:a3:96"


# The message-type offset is written out here rather than imported from the
# measurement. Importing it makes the synthesized frame move WITH a mutant that
# changes the offset, and such a mutant then survives -- measured, it did.
SPEC_MSGTYPE_OFF = 46


def sca(length, msgtype=None, text=b""):
    """A synthetic SCA payload of `length` bytes, optionally carrying a
    connection-control message type at [46:48] and a node name in the body."""
    buf = bytearray(length)
    if msgtype is not None and length >= SPEC_MSGTYPE_OFF + 2:
        buf[SPEC_MSGTYPE_OFF:SPEC_MSGTYPE_OFF + 2] = struct.pack("<H", msgtype)
    if text:
        buf[8:8 + len(text)] = text
    return bytes(buf)


# --- the 190-byte CM class is counted per direction, and only for OVMX ---
m = M.measure_frames([
    (OVMX, PEER, sca(190)),
    (OVMX, PEER, sca(190)),
    (OVMX, OTHER, sca(190)),
    (PEER, OVMX, sca(190)),
    (PEER, OTHER, sca(190)),      # peer-to-peer: must NOT be counted
    (OTHER, PEER, sca(190)),      # peer-to-peer: must NOT be counted
], OVMX)
check(m["cm_190_tx"] == 3, f"cm_190_tx {m['cm_190_tx']} != 3")
check(m["cm_190_rx"] == 1, f"cm_190_rx {m['cm_190_rx']} != 1")

# --- a 190-byte frame is NOT read as connection control ---
check(m["ctl_tx"] == {}, f"a 190-byte frame leaked into ctl_tx: {m['ctl_tx']}")

# --- [46:48] is read over the grounded classes and over no others ---
for length in (58, 62, 66, 110):
    g = M.measure_frames([(OVMX, PEER, sca(length, msgtype=3))], OVMX)
    check(g["ctl_tx"] == {3: 1},
          f"len={length} is a grounded connection-control class and was not decoded: {g['ctl_tx']}")
    check(g["accept_rsp_tx"] == 1,
          f"len={length} type 3 did not reach accept_rsp_tx")
for length in (94, 106):
    g = M.measure_frames([(OVMX, PEER, sca(length, msgtype=1410))], OVMX)
    check(g["ctl_tx"] == {},
          f"len={length} is NOT a connection-control class -- its [46:48] is a "
          f"SCSSYSTEMID, not a message type -- but it decoded as {g['ctl_tx']}")

# --- direction is not confused ---
g = M.measure_frames([
    (OVMX, PEER, sca(62, msgtype=6)),
    (PEER, OVMX, sca(62, msgtype=3)),
], OVMX)
check(g["ctl_tx"] == {6: 1}, f"ctl_tx {g['ctl_tx']} != {{6: 1}}")
check(g["ctl_rx"] == {3: 1}, f"ctl_rx {g['ctl_rx']} != {{3: 1}}")
check(g["accept_rsp_tx"] == 0,
      "an ACCEPT_RSP RECEIVED was counted as one TRANSMITTED -- the whole "
      "finding is about what OVMX builds, and this is the way to fake it")

# --- identity comes out of the bytes, guardrail 18 ---
g = M.measure_frames([
    (OVMX, PEER, sca(190, text=b"OVMXA1")),
    (PEER, OVMX, sca(190, text=b"OVMXA1")),
    (PEER, OTHER, sca(190, text=b"OVMXZZ")),   # peer-to-peer: not ours
], OVMX)
check(g["identity"] == ["OVMXA1"], f"identity {g['identity']} != ['OVMXA1']")

# --- a frame too short to hold [46:48] must not be indexed ---
g = M.measure_frames([(OVMX, PEER, b"\x00" * 20)], OVMX)
check(g["ctl_tx"] == {}, "a 20-byte frame was decoded as connection control")

# --- and the offset itself is pinned to the spec, not to the measurement ---
check(M.MSGTYPE_OFF == SPEC_MSGTYPE_OFF,
      f"the measurement reads the message type at [{M.MSGTYPE_OFF}:] but spec "
      f"sec 4(h)(1a) grounds it at [{SPEC_MSGTYPE_OFF}:48]")

# ===========================================================================
# 2. THE FIGURES MUST STILL APPEAR IN THE SPEC
# ===========================================================================
spec = open(SPEC, encoding="utf-8").read()
check("### 4(O)" in spec, "spec section 4(O) is missing")

sec = spec.split("### 4(O)", 1)[1].split("\n## 5.", 1)[0] if "### 4(O)" in spec else ""

for tag, e in EXPECTED["runs"].items():
    check(tag in sec, f"run tag {tag} is not named in spec sec 4(O)")
    for ident in e["identity"]:
        check(ident in sec, f"identity {ident} is not in spec sec 4(O)")
    for field in ("cm_190_tx", "cm_190_rx"):
        check(re.search(r"\b%d\b" % e[field], sec),
              f"{tag} {field} = {e[field]} is not quoted in spec sec 4(O)")
    check(e["branch"] in sec, f"branch {e['branch']} is not named in spec sec 4(O)")

check(EXPECTED["pod"] in sec, "the pod is not named in spec sec 4(O)")
check(EXPECTED["date"] in sec, "the run date is not in spec sec 4(O)")

# --- THE FINDING ITSELF, PINNED IN THE TABLE AND NOT ONLY IN THE PROSE ---
# The bracket is only a bracket if the two runs of the failing binary agree with
# each other and disagree with the joining one on all three figures at once.
# Without this, EXPECTED can be edited into a shape that says the opposite while
# every per-run number still "matches the spec".
closure = [e for e in EXPECTED["runs"].values() if e["branch"] == "work/vms-187-closure"]
joiner = [e for e in EXPECTED["runs"].values() if e["branch"] != "work/vms-187-closure"]
check(len(closure) == 2 and len(joiner) == 1,
      f"the bracket must be two runs of the failing binary around one of the "
      f"joining one; EXPECTED has {len(closure)} and {len(joiner)}")
for e in closure:
    check(e["joined"] is False, f"a {e['branch']} run is recorded as JOINED")
    check(e["accept_rsp_tx"] == 0,
          f"a {e['branch']} run is recorded as sending {e['accept_rsp_tx']} "
          f"ACCEPT_RSP(s); the measurement found none")
    check(e["cm_190_rx"] == 0,
          f"a {e['branch']} run is recorded as receiving {e['cm_190_rx']} CM "
          f"frames; the measurement found none")
    check(3 not in e["ctl_tx"],
          "a work/vms-187-closure run lists message type 3 among the types it "
          "transmitted, which contradicts its own accept_rsp_tx")
for e in joiner:
    check(e["joined"] is True, "the joining control is recorded as NOT JOINED")
    check(e["accept_rsp_tx"] > 0 and e["ctl_tx"].get(3) == e["accept_rsp_tx"],
          "the joining control's ACCEPT_RSP count and its type-3 count disagree")
    check(e["cm_190_rx"] > 0, "the joining control received no CM frames")

# The ACCEPT_RSP counts are the discriminator, so they are pinned by name and
# not just as loose digits.
check(re.search(r"ACCEPT_RSP.{0,40}?2\b|`3`\s*ACCEPT_RSP\s*×2", sec, re.S),
      "spec sec 4(O) does not state that the joining binary sent 2 ACCEPT_RSPs")
check("actions-required-but-not-emitted=1" in sec,
      "spec sec 4(O) drops the daemon's own unemitted-action counter")

# The non-claim is load-bearing: nine items in this epic were sent back for a
# claim outrunning its evidence, and 4(O) is one correlation away from being
# the tenth.
check("does **not** establish" in sec,
      "spec sec 4(O) lost its explicit non-claim -- the section must keep saying "
      "that the missing ACCEPT_RSP is NOT established as the cause")

# ===========================================================================
# 2b. THE vms-578 ACCEPTANCE BRACKET -- EXPECTED_578 vs spec sec 4(O.1)
# ===========================================================================
# WHY THIS EXISTS (vms-096). EXPECTED_578 is the bracket in which the
# integrated tree JOINS -- the evidence the whole SCA-closure epic rests on --
# and until now NO ctest gate pinned it to the prose. Every figure in sec
# 4(O.1) could drift, or be edited to say the opposite, with the suite green.
# The bracket above (EXPECTED) has been gated since vms-70e2; this is the same
# discipline applied to the run that matters more.
check(hasattr(M, "EXPECTED_578"),
      "the measurement no longer defines EXPECTED_578 -- the vms-578 "
      "acceptance bracket has lost its recorded figures")
EXPECTED_578 = getattr(M, "EXPECTED_578", None)
CAPTURES_578 = getattr(M, "CAPTURES_578", {})

if EXPECTED_578:
    check("#### 4(O.1)" in spec, "spec section 4(O.1) is missing")
    sec1 = spec.split("#### 4(O.1)", 1)[1].split("\n## 5.", 1)[0] if "#### 4(O.1)" in spec else ""
    # The prose writes branches inside backticks; compare against a copy with
    # the markup removed rather than teaching EXPECTED_578 to carry markup.
    sec1_plain = sec1.replace("`", "")

    check(EXPECTED_578["pod"] in sec1, "the pod is not named in spec sec 4(O.1)")
    check(EXPECTED_578["date"] in sec1, "the run date is not in spec sec 4(O.1)")
    check(EXPECTED_578["lab"].replace("-", "-") in sec1,
          "spec sec 4(O.1) does not say which LAB the runs were taken on -- "
          "the lab-1/lab-2 distinction is what vms-096 fenced")

    # THE FIGURES ARE READ OUT OF THE TABLE ROW, NOT SEARCHED FOR AS LOOSE
    # DIGITS. Measured: with a bare `\b%d\b` search over the section, mutating
    # B1's cm_190_tx from 509 to 508 SURVIVED -- 508 is B2's value and appears
    # one row down. A figures gate that cannot tell two rows apart is not
    # pinning anything.
    def table_rows(text):
        """Parse the sec 4(O.1) markdown table into {run tag: [cells]}."""
        rows = {}
        for line in text.splitlines():
            line = line.strip()
            if not (line.startswith("|") and line.endswith("|")):
                continue
            cells = [c.strip().strip("`").replace("**", "").strip()
                     for c in line[1:-1].split("|")]
            if len(cells) < 6 or not re.fullmatch(r"B\d", cells[0]):
                continue
            rows[cells[0]] = cells
        return rows

    rows578 = table_rows(sec1)
    check(set(rows578) == set(EXPECTED_578["runs"]),
          f"spec sec 4(O.1)'s table lists runs {sorted(rows578)}, EXPECTED_578 "
          f"has {sorted(EXPECTED_578['runs'])}")

    for tag, e in EXPECTED_578["runs"].items():
        check(tag in sec1, f"run tag {tag} is not named in spec sec 4(O.1)")
        for ident in e["identity"]:
            check(ident in sec1, f"identity {ident} is not in spec sec 4(O.1)")
        check(e["branch"] in sec1_plain,
              f"branch {e['branch']} is not named in spec sec 4(O.1)")
        row = rows578.get(tag)
        if not row:
            check(False, f"{tag} has no row in the spec sec 4(O.1) table")
            continue
        # | run | binary | identity | CM 190 tx | CM 190 rx | ACCEPT_RSP | verdict |
        check(row[1].replace("`", "") in e["branch"] or e["branch"].startswith(row[1]),
              f"{tag}: the table names binary {row[1]!r}, EXPECTED_578 says "
              f"{e['branch']!r}")
        check(row[2] in e["identity"],
              f"{tag}: the table's identity {row[2]!r} is not among the "
              f"identities measured on the wire {e['identity']!r}")
        for col, field in ((3, "cm_190_tx"), (4, "cm_190_rx"), (5, "accept_rsp_tx")):
            check(row[col].isdigit() and int(row[col]) == e[field],
                  f"{tag} {field}: the spec sec 4(O.1) table row says {row[col]!r}, "
                  f"the measurement says {e[field]}")
        check(("JOINED" in row[6]) is bool(e["joined"]),
              f"{tag}: the table verdict {row[6]!r} contradicts joined={e['joined']}")
        # internal consistency: a run cannot claim N ACCEPT_RSPs and a
        # different count of message type 3.
        check(e["ctl_tx"].get(3) == e["accept_rsp_tx"],
              f"{tag}: ctl_tx type-3 count {e['ctl_tx'].get(3)} disagrees with "
              f"accept_rsp_tx {e['accept_rsp_tx']}")

    # --- THE SHAPE OF THE BRACKET, not just the per-run digits -------------
    # Two runs of the INTEGRATED tree with the control BETWEEN them, and all
    # three joining. Without this, EXPECTED_578 could be rewritten into a shape
    # that says the merge failed while every individual number still "appears
    # in the spec".
    integrated = [t for t, e in EXPECTED_578["runs"].items() if "vms-578" in e["branch"]]
    control = [t for t, e in EXPECTED_578["runs"].items() if "vms-578" not in e["branch"]]
    check(len(integrated) == 2 and len(control) == 1,
          f"the acceptance bracket must be two runs of the integrated tree "
          f"around one control; EXPECTED_578 has {len(integrated)} and {len(control)}")
    check(control and EXPECTED_578["runs"][control[0]]["branch"]
          == "worktree-760-active-directory",
          "the control arm is not the worktree-760-active-directory binary")
    for tag, e in EXPECTED_578["runs"].items():
        check(e["joined"] is True,
              f"{tag} is recorded as NOT JOINED -- sec 4(O.1) is the bracket in "
              f"which every arm joins; a false arm here would invert the finding")
        check(e["accept_rsp_tx"] > 0,
              f"{tag} joined while sending no ACCEPT_RSP, which contradicts the "
              f"4(O) bracket's own discriminator")
        check(e["cm_190_rx"] > 0, f"{tag} joined while receiving no CM frames")

    # --- the claims the section must keep making ---------------------------
    check("residue" in sec1,
          "spec sec 4(O.1) drops the OVMXA0 residue note -- the B1 capture "
          "carries two identities and that is recorded, not filtered")
    check("NOT CLAIMED" in sec1 or "not claimed" in sec1,
          "spec sec 4(O.1) lost its non-claim about the CONNECT_RSP/REJECT_REQ "
          "reduction being an improvement")
    check("FIRST join" in sec1,
          "spec sec 4(O.1) must keep saying these are FIRST joins -- the "
          "rejoin (vms-2f3) is explicitly out of scope and a stale incarnation "
          "cannot affect a first join")
    check(set(CAPTURES_578) == set(EXPECTED_578["runs"]),
          f"CAPTURES_578 {sorted(CAPTURES_578)} does not cover exactly the runs "
          f"in EXPECTED_578 {sorted(EXPECTED_578['runs'])}")
    for fn in CAPTURES_578.values():
        check(fn.rsplit("-", 1)[0] in spec or fn in spec
              or "vms578-{B1,B3,B2}-lab2-vaxlab4-20260805.pcap" in spec,
              f"spec sec 4(O.1) does not name the capture {fn}")

    # The re-derivation of BOTH brackets now happens once, at the end of this
    # file, through scs_wire.gate() -- see section 4.

# ===========================================================================
# 3. THE QUARANTINE
# ===========================================================================
QBEGIN, QEND = "REFUTED-QUOTE-BEGIN", "REFUTED-QUOTE-END"
MAX_QUARANTINE_CHARS = 1200
DEAD = re.compile(r"NEVER\s+OBSERVED\s+AN\s+ACCEPT_RSP", re.I)


def quarantine_spans(text):
    toks = sorted([(mm.start(), QBEGIN) for mm in re.finditer(re.escape(QBEGIN), text)] +
                  [(mm.start(), QEND) for mm in re.finditer(re.escape(QEND), text)])
    spans, open_at = [], None
    for pos, kind in toks:
        if kind == QBEGIN:
            check(open_at is None,
                  f"a {QBEGIN} at {pos} opens inside a block already open at {open_at} "
                  f"-- nesting makes the parsed span smaller than the quarantined text")
            if open_at is None:
                open_at = pos
        else:
            if open_at is None:
                check(False, f"a {QEND} at {pos} with no open block")
                continue
            end = pos + len(QEND)
            check(end - open_at <= MAX_QUARANTINE_CHARS,
                  f"a quarantine block is {end - open_at} chars (cap "
                  f"{MAX_QUARANTINE_CHARS}); quarantining a whole section is not a "
                  f"way to make the dead claim legal again")
            spans.append((open_at, end))
            open_at = None
    check(open_at is None, f"an unbalanced {QBEGIN} with no matching {QEND}")
    return spans


def outside(text):
    spans, out, prev = quarantine_spans(text), [], 0
    for b, e in spans:
        out.append(text[prev:b])
        prev = e
    out.append(text[prev:])
    return "".join(out)


header = open(HEADER, encoding="utf-8").read()
for name, text in (("scs_sdir.h", header), ("cluster-protocol-spec.md", spec)):
    quarantined = [text[b:e] for b, e in quarantine_spans(text) if DEAD.search(text[b:e])]
    check(len(quarantined) == 1,
          f"{name}: the refuted 'never observed an ACCEPT_RSP' claim must appear "
          f"in exactly one quarantine block, found {len(quarantined)}")
    check(not DEAD.search(outside(text)),
          f"{name}: the refuted 'never observed an ACCEPT_RSP' claim is asserted "
          f"outside a {QBEGIN}/{QEND} block")

# The correction has to be reachable from the header, not only from the spec.
check("vms70e2-A1-lab2-vaxlab4-20260805.pcap" in header,
      "scs_sdir.h does not name the capture that refutes the removed claim")
check("4F580007" in header and "B751000C" in header,
      "scs_sdir.h does not carry the Con.ID pair of the observed ACCEPT_RSP")

# ===========================================================================
# 4. THE REAL MEASUREMENT, WHEN THE CAPTURES ARE HERE (vms-371)
# ===========================================================================
# Sections 1-3 pin the DECODER, the PROSE and the QUARANTINE. This pins
# EXPECTED and EXPECTED_578 to the PACKETS, via the tool's own rederive(), so a
# green gate on a lab host means wire == EXPECTED == prose for BOTH brackets.
#
# BRACKET_CAPTURES is passed as the `need` set rather than "*.pcap": a lab-2
# directory holding only the vms-70e2 files must count as ABSENT and get the
# banner, or the vms-578 acceptance figures would be skipped silently -- which
# is exactly the state vms-371 found them in (pinned by no gate at all).
#
# OVMX_LAB2_CAPTURES is honoured first because these are LAB-2 captures living
# in a sibling directory of the lab-1 grounding library (vms-096); scs_wire's
# own OVMX_LAB_CAPTURES is the fallback so the mutation battery can force the
# no-captures arm with one variable.
_cap2 = os.environ.get("OVMX_LAB2_CAPTURES")
if _cap2:
    os.environ[scs_wire.ENV_CAPTURES] = _cap2
_capdir = scs_wire.capture_dir(M.DEFAULT_CAPTURE_DIR, need=M.BRACKET_CAPTURES)
if _capdir is None:
    scs_wire.require_coverage("scs_join_capability_figures", M, None, check)
    scs_wire.announce_absent("scs_join_capability_figures", M.DEFAULT_CAPTURE_DIR, check)
else:
    _covered = scs_wire.rederive("scs_join_capability_figures", M, _capdir, check)
    scs_wire.require_coverage("scs_join_capability_figures", M, _covered, check)

print(f"{checks} checks, {len(failures)} failures")
for f in failures:
    print("FAIL: " + f)
sys.exit(1 if failures else 0)
