#!/usr/bin/env python3
"""
test_scs_env_figures.py -- vms-ec7 (MSCP epic Phase A).

THE GATE FOR THE SCS MESSAGE ENVELOPE. Two halves, and both are needed:

  (A) THE PROSE IS PINNED TO EXPECTED. Every offset, constant and shape claim
      src/vmsscs/include/scs_env.h states is read out of
      tools/cluster/scs_env_measure.py's EXPECTED table and must appear in the
      header. Editing one without the other reds.

  (B) EXPECTED IS PINNED TO THE PACKETS (vms-371, via scs_wire.rederive()). On
      a host with the lab captures this gate calls the measurement tool's own
      rederive() and reds on any figure the wire no longer supports -- including
      THE BUILD ROUND TRIP, which re-derives content[42:58] for every
      envelope-conformant frame in the corpus using scs_env_build()'s own rule
      and requires byte-identity with the capture. Without the captures it
      announces the gap with a banner and OVMX_SCS_REQUIRE_WIRE=1 makes the
      absence itself a failure.

  (C) THE OFFSETS EXIST EXACTLY ONCE. The whole point of the item is that the
      envelope layout stopped being copied. This gate greps the tree for a
      second numeric definition of the six offsets and reds if one comes back:
      scs_rx.h must alias scs_env.h's names, not restate their values.

  (D) THE NON-ENVELOPE CLASSES ARE STILL REFUSED. scs_start.c (106-content) and
      scs_hello.c (120-content) must NOT call the envelope builder -- their
      [44:46] and [46:48] carry a config-round counter and a SCSSYSTEMID, and
      routing them through this layer would be the sec 4(h)(1d) misread the
      design doc records as a self-caught confound.

Path overrides (used only by the mutation battery, which runs this gate against
a scratch copy): OVMX_SCS_ENV_ROOT relocates every input, OVMX_SCS_ENV_MEASURE
points at a scratch copy of the measurement script.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import scs_wire                                                    # noqa: E402

ROOT = (os.environ.get("OVMX_SCS_ENV_ROOT") or
        os.path.abspath(os.path.join(HERE, "..", "..")))
MEASURE = os.environ.get(
    "OVMX_SCS_ENV_MEASURE",
    os.path.join(ROOT, "tools", "cluster", "scs_env_measure.py"))
ENV_H = os.path.join(ROOT, "src", "vmsscs", "include", "scs_env.h")
ENV_C = os.path.join(ROOT, "src", "vmsscs", "scs_env.c")
RX_H = os.path.join(ROOT, "src", "vmsscs", "include", "scs_rx.h")
START_C = os.path.join(ROOT, "src", "vmsscs", "scs_start.c")
HELLO_C = os.path.join(ROOT, "src", "vmsscs", "scs_hello.c")
SRC_DIR = os.path.join(ROOT, "src", "vmsscs")

checks = 0
failures = []


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)
        print(f"  FAIL {what}")


def read(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return fh.read()
    except OSError:
        return ""


MOD = scs_wire.load_source(MEASURE, "scs_env_measure")

env_h = read(ENV_H)
env_c = read(ENV_C)
rx_h = read(RX_H)

check(bool(env_h), "src/vmsscs/include/scs_env.h exists")
check(bool(env_c), "src/vmsscs/scs_env.c exists")
check(os.path.exists(MEASURE), "the measuring tool is checked in")

# ---------------------------------------------------------------------------
# (A) THE PROSE IS PINNED TO EXPECTED
# ---------------------------------------------------------------------------

# The six offsets, the format word, the bias and the payload boundary, exactly
# as the measurement tool reads them. If the tool and the header ever disagree
# the tool is measuring a layout the code does not build.
for name, value in (("SCS_ENV_OFF_INNER_LEN", MOD.OFF_INNER_LEN),
                    ("SCS_ENV_OFF_FORMAT", MOD.OFF_FORMAT),
                    ("SCS_ENV_OFF_MTYPE", MOD.OFF_MTYPE),
                    ("SCS_ENV_OFF_CREDIT", MOD.OFF_CREDIT),
                    ("SCS_ENV_OFF_DEST_CONID", MOD.OFF_DEST_CONID),
                    ("SCS_ENV_OFF_SRC_CONID", MOD.OFF_SRC_CONID),
                    ("SCS_ENV_HDR_END", MOD.HDR_END),
                    ("SCS_ENV_INNER_LEN_BIAS", MOD.INNER_LEN_BIAS)):
    m = re.search(r"#define\s+%s\s+(\S+)" % name, env_h)
    check(m is not None and int(m.group(1), 0) == value,
          f"scs_env.h defines {name} == {value} "
          f"(the value scs_env_measure.py reads)")

m = re.search(r"#define\s+SCS_ENV_FORMAT_WORD\s+(\S+)", env_h)
check(m is not None and int(m.group(1).rstrip("uU"), 0) == MOD.FORMAT_WORD,
      f"scs_env.h defines SCS_ENV_FORMAT_WORD == 0x{MOD.FORMAT_WORD:04x}")

# The MTYPE namespace claim.
ns = MOD.EXPECTED["mtype_namespace"]
check(str(ns[0]) in env_h and str(ns[-1]) in env_h,
      "scs_env.h states the MTYPE namespace endpoints")
check("{0..10}" in env_h,
      "scs_env.h states the measured MTYPE namespace {0..10} verbatim")
m = re.search(r"#define\s+SCS_ENV_MTYPE_APP_MESSAGE\s+(\d+)", env_h)
check(m is not None and int(m.group(1)) == 10,
      "scs_env.h defines SCS_ENV_MTYPE_APP_MESSAGE == 10 (p. 4-13)")
m = re.search(r"#define\s+SCS_ENV_MTYPE_CONTROL_MAX\s+(\d+)", env_h)
check(m is not None and int(m.group(1)) == 9,
      "scs_env.h defines SCS_ENV_MTYPE_CONTROL_MAX == 9")

# 8 and 9 are observed and UNNAMED. A gate on this because naming them would be
# the guess-presented-as-decode this epic keeps rejecting, and Phase C
# (vms-f03) owns the identification.
check("vms-f03" in env_h and "UNIDENTIFIED" in env_h.upper(),
      "scs_env.h records that MTYPEs 8 and 9 are observed and UNIDENTIFIED, "
      "and names the item that owns them")
check('"type 8"' in env_c and '"type 9"' in env_c,
      "scs_env.c renders MTYPE 8/9 as bare type numbers, never as a guessed name")

# (D) THE BUILD ROUND TRIP, pinned to the header prose as well as to the wire.
# Without this the figure is only checked on a host that has the captures, and
# the pycache arm of test_scs_figures_wire_mutants.py proves that is not enough:
# a same-size, same-second edit of EXPECTED with the wire arm off must still red.
check(f"{MOD.EXPECTED['build_mismatches']} mismatch" in env_h,
      f"scs_env.h states the build round trip's result "
      f"({MOD.EXPECTED['build_mismatches']} mismatch(es))")
check(f"{MOD.EXPECTED['conformant_frames_floor']:,}" in env_h or
      str(MOD.EXPECTED["conformant_frames_floor"]) in env_h,
      f"scs_env.h states the population the round trip was measured over "
      f"({MOD.EXPECTED['conformant_frames_floor']:,} frames)")

# The build path derives the inner length rather than copying it -- the single
# property that makes one shared builder safer than six copies.
check("DERIVED, never copied" in env_c,
      "scs_env.c says the inner length is DERIVED, never copied")
check("sca_len - SCS_ENV_INNER_LEN_BIAS" in env_c,
      "scs_env.c derives the inner length from the class length")

# The header must carry the refusal list, or the next reader routes a START
# through the envelope.
for cls in ("106", "120", "70"):
    check(cls in env_h,
          f"scs_env.h names the {cls}-content class among those the envelope "
          f"test refuses")

# ---------------------------------------------------------------------------
# (C) THE OFFSETS EXIST EXACTLY ONCE
# ---------------------------------------------------------------------------
# scs_rx.h held the only copy before this item and must now alias.
for name, alias in (("SCS_RX_OFF_INNER_LEN", "SCS_ENV_OFF_INNER_LEN"),
                    ("SCS_RX_OFF_FORMAT", "SCS_ENV_OFF_FORMAT"),
                    ("SCS_RX_OFF_MTYPE", "SCS_ENV_OFF_MTYPE"),
                    ("SCS_RX_OFF_CREDIT", "SCS_ENV_OFF_CREDIT"),
                    ("SCS_RX_OFF_DEST_CONID", "SCS_ENV_OFF_DEST_CONID"),
                    ("SCS_RX_OFF_SRC_CONID", "SCS_ENV_OFF_SRC_CONID"),
                    ("SCS_RX_HDR_END", "SCS_ENV_HDR_END"),
                    ("SCS_RX_FORMAT_WORD", "SCS_ENV_FORMAT_WORD")):
    m = re.search(r"#define\s+%s\s+(\S+)" % name, rx_h)
    check(m is not None and m.group(1) == alias,
          f"scs_rx.h defines {name} as an ALIAS of {alias}, not a second copy "
          f"of the number")

# And no builder under src/vmsscs/ may write an envelope field by open-coded
# offset any more. The pattern is deliberately narrow -- a bare `44` is a common
# number, and the MSCP/member SYSAP bodies legitimately store at `body + 44` --
# so it matches only the two spellings the six builders actually used: a
# frame-relative `... + 14 + N` and the content-relative `pl + N`.
OPEN_CODED = re.compile(
    r"put_le(?:16|32)\s*\(\s*(?:\w+\s*\+\s*14|pl)\s*\+\s*"
    r"(?:42|46|48|50|54)\s*,")

# THE ONE EXEMPTION, and it is the reason check (D) exists next to it:
# scs_start.c builds the 106-content START / config class, which is NOT an
# envelope message (spec sec 4(h)(1d)). Its [44:46] is the config-round counter
# and its [46:48] is the SCSSYSTEMID -- the same byte positions carrying
# entirely different fields. Routing it through scs_env_build() would be the
# exact misread docs/design-mscp-direction.md sec 4 records as a self-caught
# method confound, so it must keep writing those two words itself.
NOT_AN_ENVELOPE = {"scs_start.c"}

for fname in sorted(os.listdir(SRC_DIR)):
    if not fname.endswith(".c") or fname == "scs_env.c":
        continue
    if fname in NOT_AN_ENVELOPE:
        continue
    txt = read(os.path.join(SRC_DIR, fname))
    hits = OPEN_CODED.findall(txt)
    check(not hits,
          f"src/vmsscs/{fname} writes no envelope field by open-coded offset "
          f"(found {len(hits)}) -- scs_env_build() is the one build path")

# The exemption is itself gated: scs_start.c must still be the 106-content class
# and must still say why it is not an envelope, so the exemption cannot quietly
# become a hole a new builder hides in.
_start = read(START_C)
check("106" in _start, "scs_start.c is still the 106-content class")
check("[44:46]" in _start and "[46:48]" in _start,
      "scs_start.c still names what its [44:46] and [46:48] carry instead of "
      "the envelope's fields")

# ---------------------------------------------------------------------------
# (D) THE NON-ENVELOPE CLASSES ARE STILL REFUSED
# ---------------------------------------------------------------------------
for label, path in (("scs_start.c (106-content START/config)", START_C),
                    ("scs_hello.c (120-content HELLO)", HELLO_C)):
    txt = read(path)
    check("scs_env_build" not in txt,
          f"{label} does NOT route through the envelope builder -- its "
          f"[44:46]/[46:48] are not an envelope")

# ---------------------------------------------------------------------------
# (B) THE WIRE ITSELF (vms-371)
# ---------------------------------------------------------------------------
_capdir = scs_wire.capture_dir(MOD.DEFAULT_CAPDIR)
if _capdir is None:
    scs_wire.require_coverage("scs_env_figures", MOD, None, check)
    scs_wire.announce_absent("scs_env_figures", MOD.DEFAULT_CAPDIR, check)
else:
    _cov = scs_wire.rederive("scs_env_figures", MOD, _capdir, check)
    scs_wire.require_coverage("scs_env_figures", MOD, _cov, check)

print(f"\ntest_scs_env_figures: {checks} checks, {len(failures)} failure(s)")
sys.exit(1 if failures else 0)
