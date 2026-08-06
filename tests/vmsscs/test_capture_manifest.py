#!/usr/bin/env python3
"""
test_capture_manifest.py -- vms-beb: the capture corpus is a MANIFEST, not a
glob, and this is the test that proves the refusal.

WHY THIS EXISTS. Six 2026-08-05 lab-2 `vaxlab-4` captures were deposited into
`/data/training/vax/cluster/captures/`, the directory every lab-1-grounded
measurement tool globs and treats as the LAB-1 grounding library. lab-2
replicas reuse lab-1's SCSSYSTEMIDs and node MACs by design
(tests/lab/README.md), and lab-2's simh binary (aarch64) and message-type
vocabulary (a subset of lab-1's, x86_64) differ, so the deposit silently
mixed two labs and put checks in four measurement tools red against their own
EXPECTED tables (vms-096).

tools/cluster/capture_manifest.py is the fix: a checked-in MANIFEST naming
every capture and the lab that produced it, so a tool asking for a lab-1
population REFUSES a lab-2 capture -- and refuses an UNKNOWN one too, which a
filename-marker blocklist cannot do.

NEEDS NO LAB CAPTURES. Every scenario below uses empty placeholder files in a
scratch tmp directory: `capture_manifest`'s functions only ever look at
filenames (glob hits and basenames), never file contents, so a 0-byte file
with the right name is exactly as good a test subject as the real pcap.

    (1) the manifest itself: every entry is LAB1 or LAB2, basenames unique.
    (2) check_paths(): a clean lab-1 list passes through unchanged; an
        unknown capture reds; a REAL lab-2-declared capture requested as
        lab-1 reds and names itself -- this IS "point a lab-1 census at a
        lab-2 capture and confirm it reds", the item's own acceptance test.
    (3) scan(): the same three scenarios, but through the actual
        directory-globbing entry point every measurement tool calls.
    (4) check_named(): the named-list entry point scs_peer_silence_measure.py
        and scs_join_capability_measure.py use, including the directory-audit
        half (a foreign file sitting next to correctly-named ones must still
        red).
    (5) WIRING: every one of the five tools vms-beb names actually imports
        AND EXECUTES capture_manifest's gate as its real entry point, not
        merely a source-substring the tool happens to contain. Each tool is
        run as a subprocess: once against a fixture directory holding no
        capture at all (must NOT trip the gate) and once against a fixture
        holding one capture the manifest has never heard of (MUST trip the
        gate, by name). A grep for `capture_manifest.check_named(` cannot
        fail the way vms-992 failed -- MANIFEST silently missing 15 real
        lab-2 captures left every one of these tools' actual runs refusing,
        while the grep (and the two tests that entered via rederive()
        instead of main()) stayed green throughout. This subprocess pair
        is the check that CAN fail that way.
"""
import glob
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CLUSTER_DIR = os.path.join(ROOT, "tools", "cluster")

sys.path.insert(0, CLUSTER_DIR)
import capture_manifest as cm  # noqa: E402

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


def touch(path):
    open(path, "a").close()


# ===========================================================================
# 1. THE MANIFEST ITSELF
# ===========================================================================
check(len(cm.MANIFEST) > 0, "MANIFEST is empty")
check(set(cm.MANIFEST.values()) <= {cm.LAB1, cm.LAB2},
      "MANIFEST contains a lab label other than LAB1/LAB2: %r"
      % sorted(set(cm.MANIFEST.values()) - {cm.LAB1, cm.LAB2}))
lab1_entries = [k for k, v in cm.MANIFEST.items() if v == cm.LAB1]
lab2_entries = [k for k, v in cm.MANIFEST.items() if v == cm.LAB2]
check(len(lab1_entries) > 0, "MANIFEST declares no lab-1 capture")
check(len(lab2_entries) > 0, "MANIFEST declares no lab-2 capture")
# The six vms-096 offenders must be declared LAB2 by name, not absent.
VMS096_OFFENDERS = (
    "vms578-B1-lab2-vaxlab4-20260805.pcap",
    "vms578-B2-lab2-vaxlab4-20260805.pcap",
    "vms578-B3-lab2-vaxlab4-20260805.pcap",
    "vms70e2-A0-lab2-vaxlab4-20260805.pcap",
    "vms70e2-A1-lab2-vaxlab4-20260805.pcap",
    "vms70e2-A3-lab2-vaxlab4-20260805.pcap",
)
for fn in VMS096_OFFENDERS:
    check(cm.MANIFEST.get(fn) == cm.LAB2,
          "the vms-096 offender %s is not declared lab-2 in the manifest" % fn)

# ===========================================================================
# 2. check_paths() -- the direct list-in/list-out entry point
# ===========================================================================
CLEAN_LAB1 = ["/x/%s" % lab1_entries[0], "/y/%s" % lab1_entries[1]]
check(cm.check_paths(list(CLEAN_LAB1), cm.LAB1) == CLEAN_LAB1,
      "check_paths() rejected a clean lab-1 list")

try:
    cm.check_paths(["/x/definitely-not-a-real-capture.pcap"], cm.LAB1)
    check(False, "check_paths() accepted a capture unknown to the manifest")
except SystemExit as exc:
    check("definitely-not-a-real-capture.pcap" in str(exc),
          "check_paths() refused an unknown capture without naming it")

# --- THE ITEM'S OWN ACCEPTANCE TEST: a lab-1 census pointed at a lab-2 capture
LAB2_OFFENDER = lab2_entries[0]
try:
    cm.check_paths(CLEAN_LAB1 + ["/z/%s" % LAB2_OFFENDER], cm.LAB1)
    check(False, "check_paths() accepted a lab-2 capture in a lab-1 census "
                 "-- 'point a lab-1 census at a lab-2 capture' must RED")
except SystemExit as exc:
    check(LAB2_OFFENDER in str(exc),
          "check_paths() refused the lab-2 capture without naming it")
    check(cm.LAB2 in str(exc),
          "check_paths() did not say WHICH lab the offending capture belongs to")

# The mirror direction: a lab-2 census pointed at a lab-1 capture.
LAB1_OFFENDER = lab1_entries[0]
try:
    cm.check_paths(["/z/%s" % LAB1_OFFENDER], cm.LAB2)
    check(False, "check_paths() accepted a lab-1 capture in a lab-2 census")
except SystemExit as exc:
    check(LAB1_OFFENDER in str(exc),
          "check_paths() refused the lab-1-in-lab-2 case without naming it")

# ===========================================================================
# 3. scan() -- the glob-based entry point every measurement tool calls
# ===========================================================================
with tempfile.TemporaryDirectory(prefix="capture_manifest_scan.") as tmp:
    for fn in lab1_entries[:5]:
        touch(os.path.join(tmp, fn))
    got = cm.scan(tmp, cm.LAB1)
    check(sorted(os.path.basename(p) for p in got) == sorted(lab1_entries[:5]),
          "scan() did not return exactly the clean lab-1 files it was given")

    # Deposit a lab-2 capture into the lab-1 directory (the vms-096 scenario,
    # reproduced with a real directory scan instead of a hand-built list).
    touch(os.path.join(tmp, LAB2_OFFENDER))
    try:
        cm.scan(tmp, cm.LAB1)
        check(False, "scan() silently included a lab-2 capture deposited "
                     "into a lab-1 directory")
    except SystemExit as exc:
        check(LAB2_OFFENDER in str(exc),
              "scan() refused the mixed directory without naming the offender")
    os.remove(os.path.join(tmp, LAB2_OFFENDER))

    # An unknown capture -- never declared for EITHER lab.
    touch(os.path.join(tmp, "unknown-capture-not-in-manifest.pcap"))
    try:
        cm.scan(tmp, cm.LAB1)
        check(False, "scan() silently included a capture unknown to the manifest")
    except SystemExit as exc:
        check("unknown-capture-not-in-manifest.pcap" in str(exc),
              "scan() refused the unknown capture without naming it")

    # Recursive mode (scs_connect_data_measure.py globs **/*.pcap): a lab-2
    # capture nested in a subdirectory must be caught too.
    os.remove(os.path.join(tmp, "unknown-capture-not-in-manifest.pcap"))
    sub = os.path.join(tmp, "nested")
    os.mkdir(sub)
    touch(os.path.join(sub, LAB2_OFFENDER))
    try:
        cm.scan(tmp, cm.LAB1, recursive=True)
        check(False, "scan(recursive=True) missed a lab-2 capture nested in a subdirectory")
    except SystemExit as exc:
        check(LAB2_OFFENDER in str(exc),
              "scan(recursive=True) refused the nested lab-2 capture without naming it")

# ===========================================================================
# 4. check_named() -- the fixed-list entry point (peer_silence, join_capability)
# ===========================================================================
with tempfile.TemporaryDirectory(prefix="capture_manifest_named.") as tmp:
    names = lab2_entries[:2]
    for fn in names:
        touch(os.path.join(tmp, fn))
    check(cm.check_named(tmp, names, cm.LAB2) == names,
          "check_named() rejected a clean lab-2 named list")

    # A name the caller asks for that the manifest declares under the WRONG lab.
    try:
        cm.check_named(tmp, [lab1_entries[0]], cm.LAB2)
        check(False, "check_named() accepted a lab-1 name under a lab-2 request")
    except SystemExit as exc:
        check(lab1_entries[0] in str(exc),
              "check_named() refused the wrong-lab name without naming it")

    # The directory-audit half: the NAMED files are fine, but a foreign file
    # (a lab-1 capture) sits in the same directory. Must still red.
    touch(os.path.join(tmp, lab1_entries[1]))
    try:
        cm.check_named(tmp, names, cm.LAB2)
        check(False, "check_named() ignored a foreign lab-1 capture sitting "
                     "next to the correctly-named lab-2 files")
    except SystemExit as exc:
        check(lab1_entries[1] in str(exc),
              "check_named()'s directory audit did not name the foreign file")

# ===========================================================================
# 5. WIRING -- the five tools vms-beb names must actually EXECUTE the
#    manifest gate as part of their real entry point (main(), via
#    subprocess), not merely import capture_manifest and never reach the
#    call. A source-substring grep is satisfied by dead code; this is not.
# ===========================================================================
# rel path -> the call site's own source-substring, kept as a cheap first
# assertion (a missing import/call is a clearer failure than a subprocess
# traceback), PLUS now actually run below.
WIRED = {
    "tools/scs_connect_data_measure.py": "capture_manifest.check_paths(",
    "tools/cluster/scs_disc_measure.py": "capture_manifest.check_paths(",
    "tools/cluster/scs_reason_measure.py": "capture_manifest.check_paths(",
    "tools/scs_peer_silence_measure.py": "capture_manifest.check_named(",
    "tools/cluster/scs_join_capability_measure.py": "capture_manifest.check_named(",
}
for rel, wiring in WIRED.items():
    path = os.path.join(ROOT, rel)
    check(os.path.exists(path), "%s is missing" % rel)
    if not os.path.exists(path):
        continue
    src = read(path)
    check("import capture_manifest" in src,
          "%s does not import capture_manifest" % rel)
    check(wiring in src,
          "%s imports capture_manifest but never calls %s -- a manifest "
          "nobody calls is not a fence" % (rel, wiring))

UNKNOWN_NAME = "unknown-capture-not-in-manifest.pcap"

for rel in WIRED:
    path = os.path.join(ROOT, rel)
    if not os.path.exists(path):
        continue

    # (a) POSITIVE CONTROL: an empty --captures directory must run past the
    # gate untouched. If this trips, the gate (or the tool's own argument
    # handling around it) is broken independent of any manifest content.
    with tempfile.TemporaryDirectory(prefix="wiring_clean.") as tmp:
        proc = subprocess.run(
            [sys.executable, path, "--captures", tmp, "--print"],
            capture_output=True, text=True, timeout=60)
        out = proc.stdout + proc.stderr
        check("capture manifest disagreement" not in out,
              "%s refused a --captures directory holding no capture at "
              "all -- output:\n%s" % (rel, out))

    # (b) THE vms-992 REPRODUCTION: a capture physically present in
    # --captures that the checked-in MANIFEST has never heard of MUST make
    # the tool's actual run refuse, by name. This is the one grep could
    # never fail: vms-992's MANIFEST omitted 15 real lab-2 captures and
    # every one of these five tools' real runs was already refusing before
    # this test could have caught it -- the grep, and the two callers that
    # enter via rederive() rather than main(), all stayed green.
    with tempfile.TemporaryDirectory(prefix="wiring_dirty.") as tmp:
        touch(os.path.join(tmp, UNKNOWN_NAME))
        proc = subprocess.run(
            [sys.executable, path, "--captures", tmp, "--print"],
            capture_output=True, text=True, timeout=60)
        out = proc.stdout + proc.stderr
        check(proc.returncode != 0,
              "%s exited 0 with an unmanifested capture sitting in "
              "--captures -- the manifest gate did not actually run" % rel)
        check("capture manifest disagreement" in out and UNKNOWN_NAME in out,
              "%s did not name the unmanifested capture when it refused -- "
              "output:\n%s" % (rel, out))

print("%s: %d checks, %d failure(s)" % ("FAIL" if failures else "PASS",
                                        checks, len(failures)))
for f in failures:
    print("  FAIL %s" % f)
sys.exit(1 if failures else 0)
