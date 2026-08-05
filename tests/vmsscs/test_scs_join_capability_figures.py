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
    and requires it to PASS. On a machine without them this part is reported as
    not-run, and the three checks above still gate.

WHAT THIS TEST DOES NOT CLAIM: nothing here says the missing ACCEPT_RSP causes
the failed join. Section 4(O) states the non-claim; this test only keeps the
measured numbers and the refutation from drifting.
"""
import importlib.util
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
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
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


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
# 4. THE REAL MEASUREMENT, WHEN THE CAPTURES ARE HERE
# ===========================================================================
cap_dir = os.environ.get("OVMX_LAB_CAPTURES", M.DEFAULT_CAPTURE_DIR)
have = all(os.path.exists(os.path.join(cap_dir, fn)) for fn in M.CAPTURES.values())
if have:
    got = {t: M.measure_capture(os.path.join(cap_dir, fn), EXPECTED["ovmx_mac"])
           for t, fn in M.CAPTURES.items()}
    for tag, e in EXPECTED["runs"].items():
        g = got[tag]
        for field in ("cm_190_tx", "cm_190_rx", "ctl_tx", "accept_rsp_tx", "identity"):
            check(g[field] == e[field],
                  f"[captures] {tag} {field} {g[field]!r} != {e[field]!r}")
    print("[the lab captures are present -- EXPECTED was re-derived from them]")
else:
    print(f"[no lab captures under {cap_dir} -- the decoder, the prose and the "
          f"quarantine were gated; the re-derivation was not]")

print(f"{checks} checks, {len(failures)} failures")
for f in failures:
    print("FAIL: " + f)
sys.exit(1 if failures else 0)
