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


def _lab2_capture_dir(default):
    """The lab-2 resolution order (vms-371 / vms-14f3), and why it exists.

    These are LAB-2 captures, living in a SIBLING of the lab-1 grounding
    library (vms-096), so the LAB-1 variable must never be the thing that
    LOCATES them:

      1. OVMX_LAB2_CAPTURES names the lab-2 library outright, and wins.
      2. OVMX_LAB_CAPTURES pointing at a path that DOES NOT EXIST still forces
         the no-captures arm: test_scs_join_capability_mutants.py hides the
         wire that way, and a "hide the captures" lever has to keep working.
      3. Otherwise `default` (the tool's own DEFAULT_CAPTURE_DIR). An
         OVMX_LAB_CAPTURES that names a REAL directory is a lab-1 library; it
         says nothing about where the lab-2 brackets are, and must not be
         read as "absent".

    Every lab-2 capture block in this gate (the vms-70e2/vms-578 wire arm in
    section 4, and the vms-449/vms-449R brackets in 2c/2d) MUST resolve
    through this one function -- vms-371 fixed the 70e2/578 arm and vms-14f3
    found the 449/449R blocks still doing their own ad hoc
    OVMX_LAB2_CAPTURES-or-OVMX_LAB_CAPTURES lookup, which let
    OVMX_LAB_CAPTURES=<a real lab-1 library> silently skip 72 checks with no
    banner and no failure (the exact LAB1_SHADOW shape vms-371 closed for
    70e2/578, left open here).
    """
    cap2 = os.environ.get("OVMX_LAB2_CAPTURES")
    cap1 = os.environ.get(scs_wire.ENV_CAPTURES)
    if cap2:
        return cap2
    if cap1 and not os.path.isdir(cap1):
        return cap1                     # deliberate hide -> counts as absent
    return default


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


# EVERY 4(O*) SLICE IS BOUNDED BY THE NEXT SUBSECTION, NOT BY `## 5.`.
# vms-449 added 4(O.2) and every one of these slices silently grew to swallow
# it: 4(O.1)'s table check started reading 4(O.2)'s rows and produced 19
# failures that were entirely an artefact of the slicing. That is the same
# loosening the comment above section 4(O) in the spec warns about, arriving
# from the other direction -- a slice that ends at `## 5.` is a slice that ends
# wherever the LAST subsection ends, so a loose `\b%d\b` figure search inside it
# can be satisfied by a number belonging to a different experiment.
def section(name):
    """The text of spec subsection `name`, up to the next 4(O*) heading."""
    if name not in spec:
        return ""
    body = spec.split(name, 1)[1]
    stop = len(body)
    for nxt in re.finditer(r"\n#{3,4} 4\(O", body):
        stop = min(stop, nxt.start())
        break
    tail = body.find("\n## 5.")
    if tail != -1:
        stop = min(stop, tail)
    return body[:stop]


sec = section("### 4(O)")

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
    sec1 = section("#### 4(O.1)")
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
# 2c. THE vms-449 REJOIN BRACKET -- EXPECTED_449 vs spec sec 4(O.2)
# ===========================================================================
# WHY THIS EXISTS. EXPECTED_449 records the answer to the question the whole
# vms-2f3 programme exists for -- can a returning identity rejoin? -- and the
# answer is NO, four times, bracketed by four controls that joined. A NEGATIVE
# result is exactly the kind that rots: nobody re-reads it, and the next session
# is tempted to "just try it again". This pins the figures AND the bracket's
# shape to the prose, so neither can drift with the suite green.
check(hasattr(M, "EXPECTED_449"),
      "the measurement no longer defines EXPECTED_449 -- the vms-449 rejoin "
      "bracket has lost its recorded figures")
EXPECTED_449 = getattr(M, "EXPECTED_449", None)
CAPTURES_449 = getattr(M, "CAPTURES_449", {})
ORDER_449 = getattr(M, "ORDER_449", [])

if EXPECTED_449:
    # The bracket's SHAPE is asserted by the measurement itself so the tool and
    # the gate cannot disagree about what a bracket is. It runs on every host.
    for msg in M.check_449_bracket_shape():
        check(False, "449 bracket shape: " + msg)

    check("#### 4(O.2)" in spec, "spec section 4(O.2) is missing")
    sec2 = section("#### 4(O.2)")

    check(EXPECTED_449["pod"] in sec2, "the pod is not named in spec sec 4(O.2)")
    check(EXPECTED_449["date"] in sec2, "the run date is not in spec sec 4(O.2)")
    check(EXPECTED_449["lab"] in sec2,
          "spec sec 4(O.2) does not say which LAB the runs were taken on")
    # The per-pod OVMX MAC is the trap that would have zeroed every figure.
    check(EXPECTED_449["ovmx_mac"] in sec2,
          "spec sec 4(O.2) does not name the OVMX tap MAC this bracket was "
          "measured against -- it is per-POD and reusing another bracket's "
          "returns zero for every figure")
    check(EXPECTED_449["ovmx_mac"] != EXPECTED["ovmx_mac"],
          "EXPECTED_449 reuses the vaxlab-4 OVMX MAC; the two pods mint "
          "different taps and this would silently measure nothing")

    def rows449(text):
        rows = {}
        for line in text.splitlines():
            line = line.strip()
            if not (line.startswith("|") and line.endswith("|")):
                continue
            cells = [c.strip().strip("`").replace("**", "").strip()
                     for c in line[1:-1].split("|")]
            if len(cells) < 7 or not re.fullmatch(r"[ABC]\d", cells[0]):
                continue
            rows[cells[0]] = cells
        return rows

    tbl = rows449(sec2)
    check(set(tbl) == set(EXPECTED_449["runs"]),
          f"spec sec 4(O.2)'s table lists runs {sorted(tbl)}, EXPECTED_449 has "
          f"{sorted(EXPECTED_449['runs'])}")
    for tag, e in EXPECTED_449["runs"].items():
        row = tbl.get(tag)
        if not row:
            check(False, f"{tag} has no row in the spec sec 4(O.2) table")
            continue
        # | run | role | identity | CM 190 tx | CM 190 rx | peer DISC rx | verdict |
        check(row[2] in e["identity"],
              f"{tag}: the table's identity {row[2]!r} is not among the "
              f"identities measured on the wire {e['identity']!r}")
        for col, field in ((3, "cm_190_tx"), (4, "cm_190_rx")):
            check(row[col].isdigit() and int(row[col]) == e[field],
                  f"{tag} {field}: the spec sec 4(O.2) row says {row[col]!r}, "
                  f"the measurement says {e[field]}")
        # The peer-DISCONNECT column IS the discriminator; it is checked against
        # ctl_rx rather than trusted as prose.
        want = "%d / %d" % (e["ctl_rx"].get(6, 0), e["ctl_rx"].get(7, 0))
        check(row[5].replace(" ", "") == want.replace(" ", ""),
              f"{tag}: the table says the peer sent {row[5]!r} DISCONNECT "
              f"REQ/RSP, the measurement says {want!r}")
        check(("JOINED" in row[6]) is bool(e["joined"]),
              f"{tag}: the table verdict {row[6]!r} contradicts "
              f"joined={e['joined']}")
        check(e["ctl_tx"].get(3) == e["accept_rsp_tx"],
              f"{tag}: ctl_tx type-3 count {e['ctl_tx'].get(3)} disagrees with "
              f"accept_rsp_tx {e['accept_rsp_tx']}")

    # --- the claims the section must keep making ---------------------------
    check("The answer is NO" in sec2 or "answer is NO" in sec2,
          "spec sec 4(O.2) no longer states the answer; a bracket this "
          "expensive must say what it concluded in words, not only in a table")
    check("REFRESH" in sec2 and "refreshed=0" in sec2,
          "spec sec 4(O.2) lost the p. 2-21 REFRESH elimination -- that path "
          "was the standing candidate mechanism and its counter is the evidence")
    check("non-claim" in sec2.lower(),
          "spec sec 4(O.2) lost its explicit non-claims; the missing peer "
          "DISCONNECT pair is a correlation and must not be read as a cause")
    check("guardrail 20" in sec2.lower() or "between" in sec2.lower(),
          "spec sec 4(O.2) does not record that the controls sit BETWEEN the "
          "test runs")
    check(set(CAPTURES_449) == set(EXPECTED_449["runs"]),
          f"CAPTURES_449 {sorted(CAPTURES_449)} does not cover exactly the runs "
          f"in EXPECTED_449 {sorted(EXPECTED_449['runs'])}")

    cap449 = _lab2_capture_dir(M.DEFAULT_CAPTURE_DIR)
    have449 = os.path.isdir(cap449) and all(
        os.path.exists(os.path.join(cap449, fn)) for fn in CAPTURES_449.values())
    if have449:
        for tag, fn in CAPTURES_449.items():
            g = M.measure_capture(os.path.join(cap449, fn),
                                  EXPECTED_449["ovmx_mac"])
            e = EXPECTED_449["runs"][tag]
            for field in ("cm_190_tx", "cm_190_rx", "ctl_tx", "ctl_rx",
                          "accept_rsp_tx", "identity"):
                check(g[field] == e[field],
                      f"[captures-449] {tag} {field} {g[field]!r} != {e[field]!r}")
        print("[scs_join_capability_figures-449: the lab-2 captures are present "
              "-- EXPECTED_449 was re-derived from the packets]")
    else:
        scs_wire.announce_absent("scs_join_capability_figures-449", cap449, check)

# ===========================================================================
# 2d. THE vms-449R REPLICATION -- EXPECTED_449R vs spec sec 4(O.3)
# ===========================================================================
# WHY THIS EXISTS SEPARATELY FROM 2c. The replication's whole value is that it
# was taken on a DIFFERENT pod; a later edit that "tidied" it to share
# EXPECTED_449's pod, MAC or subject would leave both dicts individually
# plausible and destroy the only thing the pair proves. Those cross-checks live
# in check_449r_bracket_shape() and are asserted here. The bracket is also
# deliberately WEAKER (one rejoin) and says so -- this gate pins that admission
# to the prose so nobody can quietly upgrade the claim.
check(hasattr(M, "EXPECTED_449R"),
      "the measurement no longer defines EXPECTED_449R -- the vms-449 "
      "replication has lost its recorded figures")
EXPECTED_449R = getattr(M, "EXPECTED_449R", None)
CAPTURES_449R = getattr(M, "CAPTURES_449R", {})
ORDER_449R = getattr(M, "ORDER_449R", [])

if EXPECTED_449R:
    for msg in M.check_449r_bracket_shape():
        check(False, "449R bracket shape: " + msg)

    check("#### 4(O.3)" in spec, "spec section 4(O.3) is missing")
    sec3 = section("#### 4(O.3)")

    check(EXPECTED_449R["pod"] in sec3, "the pod is not named in spec sec 4(O.3)")
    check(EXPECTED_449R["date"] in sec3, "the run date is not in spec sec 4(O.3)")
    check(EXPECTED_449R["ovmx_mac"] in sec3,
          "spec sec 4(O.3) does not name the OVMX tap MAC this replication was "
          "measured against")
    # The replication is only a replication if the prose says which OTHER pod
    # it is replicating, and they really are different pods.
    check(EXPECTED_449["pod"] in sec3,
          "spec sec 4(O.3) does not name the pod it is replicating "
          f"({EXPECTED_449['pod']})")
    check(EXPECTED_449R["pod"] != EXPECTED_449["pod"],
          "EXPECTED_449R and EXPECTED_449 name the same pod")
    check(EXPECTED_449R["ovmx_mac"] != EXPECTED_449["ovmx_mac"],
          "EXPECTED_449R and EXPECTED_449 share an OVMX tap MAC")

    tbl3 = rows449(sec3)
    check(set(tbl3) == set(EXPECTED_449R["runs"]),
          f"spec sec 4(O.3)'s table lists runs {sorted(tbl3)}, EXPECTED_449R "
          f"has {sorted(EXPECTED_449R['runs'])}")
    for tag, e in EXPECTED_449R["runs"].items():
        row = tbl3.get(tag)
        if not row:
            check(False, f"{tag} has no row in the spec sec 4(O.3) table")
            continue
        check(row[2] in e["identity"],
              f"449R {tag}: the table's identity {row[2]!r} is not among the "
              f"identities measured on the wire {e['identity']!r}")
        for col, field in ((3, "cm_190_tx"), (4, "cm_190_rx")):
            check(row[col].isdigit() and int(row[col]) == e[field],
                  f"449R {tag} {field}: the spec sec 4(O.3) row says "
                  f"{row[col]!r}, the measurement says {e[field]}")
        want = "%d / %d" % (e["ctl_rx"].get(6, 0), e["ctl_rx"].get(7, 0))
        check(row[5].replace(" ", "") == want.replace(" ", ""),
              f"449R {tag}: the table says the peer sent {row[5]!r} DISCONNECT "
              f"REQ/RSP, the measurement says {want!r}")
        check(("JOINED" in row[6]) is bool(e["joined"]),
              f"449R {tag}: the table verdict {row[6]!r} contradicts "
              f"joined={e['joined']}")

    # The refused census must be IDENTICAL across the two pods -- that identity
    # is the strongest single statement sec 4(O.3) makes, so it is checked
    # against the dicts and not merely asserted in prose.
    r6 = [EXPECTED_449["runs"][t] for t in M.ORDER_449
          if EXPECTED_449["runs"][t]["role"] == "rejoin"]
    r7 = [EXPECTED_449R["runs"][t] for t in ORDER_449R
          if EXPECTED_449R["runs"][t]["role"] == "rejoin"]
    for a in r7:
        for b in r6:
            for field in ("cm_190_tx", "cm_190_rx", "ctl_tx", "ctl_rx"):
                check(a[field] == b[field],
                      f"the refused-rejoin {field} differs between "
                      f"{EXPECTED_449R['pod']} ({a[field]!r}) and "
                      f"{EXPECTED_449['pod']} ({b[field]!r}), but spec sec "
                      f"4(O.3) claims the census is identical")

    # The admissions the section must keep making.
    check("VOID" in sec3 or "void" in sec3,
          "spec sec 4(O.3) no longer records that four started runs were "
          "discarded -- a bracket that hides its abandoned runs is not evidence")
    check("18:58:50" in sec3,
          "spec sec 4(O.3) lost the pod-restart timestamp, which is the "
          "HARNESS ground on which the void runs were discarded")
    check("one" in sec3.lower() and "fluke" in sec3.lower(),
          "spec sec 4(O.3) no longer admits that it contains a single rejoin "
          "and does not by itself establish the not-a-fluke property")
    check(set(CAPTURES_449R) == set(EXPECTED_449R["runs"]),
          f"CAPTURES_449R {sorted(CAPTURES_449R)} does not cover exactly the "
          f"runs in EXPECTED_449R {sorted(EXPECTED_449R['runs'])}")

    cap449r = _lab2_capture_dir(M.DEFAULT_CAPTURE_DIR)
    have449r = os.path.isdir(cap449r) and all(
        os.path.exists(os.path.join(cap449r, fn)) for fn in CAPTURES_449R.values())
    if have449r:
        for tag, fn in CAPTURES_449R.items():
            g = M.measure_capture(os.path.join(cap449r, fn),
                                  EXPECTED_449R["ovmx_mac"])
            e = EXPECTED_449R["runs"][tag]
            for field in ("cm_190_tx", "cm_190_rx", "ctl_tx", "ctl_rx",
                          "accept_rsp_tx", "identity"):
                check(g[field] == e[field],
                      f"[captures-449R] {tag} {field} {g[field]!r} != {e[field]!r}")
        print("[scs_join_capability_figures-449R: the lab-2 captures are "
              "present -- EXPECTED_449R was re-derived from the packets]")
    else:
        scs_wire.announce_absent("scs_join_capability_figures-449R", cap449r, check)

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
# RESOLUTION ORDER, and why it is NOT simply "the override wins".
#
# These are LAB-2 captures, living in a SIBLING of the lab-1 grounding library
# (vms-096), so the LAB-1 variable must never be the thing that LOCATES them.
# The first cut of this gate let OVMX_LAB_CAPTURES win outright, and the
# consequence was measured on workshop: `OVMX_LAB_CAPTURES=<lab-1 library>
# ctest -L scs` -- the natural way to point the other five gates at a library
# that is not at their compiled-in default -- pushed THIS gate onto its
# no-captures arm on a host that had the bracket captures sitting at
# DEFAULT_CAPTURE_DIR the whole time. The one gate on the vms-578 acceptance
# bracket printed its banner and ctest reported `Passed 0.03 sec`. That is the
# exact shape vms-371 exists to kill, reintroduced by a variable name.
#
#   1. OVMX_LAB2_CAPTURES names the lab-2 library outright, and wins.
#   2. OVMX_LAB_CAPTURES pointing at a path that DOES NOT EXIST still forces
#      the no-captures arm: test_scs_join_capability_mutants.py hides the wire
#      that way, and a "hide the captures" lever has to keep working.
#   3. Otherwise the tool's own DEFAULT_CAPTURE_DIR. An OVMX_LAB_CAPTURES that
#      names a REAL directory is a lab-1 library; it says nothing whatsoever
#      about where the lab-2 brackets are, and must not be read as "absent".
#
# test_scs_figures_wire_mutants.py::LAB1_SHADOW is the regression test: it runs
# this gate with OVMX_LAB_CAPTURES on a real lab-1 library and OVMX_LAB2_CAPTURES
# unset, and requires the wire arm to have RUN.
_where = _lab2_capture_dir(M.DEFAULT_CAPTURE_DIR)
os.environ[scs_wire.ENV_CAPTURES] = _where
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
