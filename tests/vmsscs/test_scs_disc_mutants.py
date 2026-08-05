#!/usr/bin/env python3
"""
test_scs_disc_mutants.py -- vms-591: THE MUTATION BATTERY for the gate above.

WHY IT EXISTS. `scs_disc_figures` is the only thing in ctest that (a) pins the
disconnect census, the REQ->RSP latency figures behind the 500 ms bounded
shutdown wait, and the [60:62] matching-flag partition to the checked-in
measurement, and (b) keeps the REFUTED "5 and 7 do not exist on our wire" claim
from coming back. A gate like that is worth exactly what it kills, and "it is
green" says nothing about that. The vms-6b3 precedent is the reason this file
exists at all: that item shipped a figures gate whose refuted-claim check could
not fail, with a comment saying it could, and only a mutation battery found it.

It also caught two real defects in THIS gate while it was being written:
  - the quarantine parser accepted NESTED markers, so opening a second
    REFUTED-QUOTE-BEGIN inside the real block shrank the span the parser saw
    while enlarging the text a reader sees as quarantined;
  - the same-sentence claim matcher keyed on "5 and 7" and missed
    "neither 5 nor 7".
Both are fixed and both are mutants below.

HOW IT WORKS. Copies scs_disc.h, scsd.c, cluster-protocol-spec.md and
scs_disc_measure.py into a scratch tree, proves the UNMUTATED copies are green
(the control), then applies each mutant on its own and requires
test_scs_disc_figures.py to exit non-zero, restoring and re-verifying between
each. A mutant that does not change the file is reported as a FAILURE, never
scored as a kill. Nothing under src/, docs/ or tools/ is written; the gate is
pointed at the scratch copies through the OVMX_SCS_DISC_{HEADER,DAEMON,SPEC,
MEASURE} environment overrides.

The NUMBER of mutants and how many died is PRINTED, deliberately not restated
in a comment: a second copy of a figure is how the vms-6b3 gate got rejected.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
GATE = os.path.join(HERE, "test_scs_disc_figures.py")

SRC = {
    "header": os.path.join(ROOT, "src/vmsscs/include/scs_disc.h"),
    "daemon": os.path.join(ROOT, "src/vmsscs/scsd.c"),
    "spec": os.path.join(ROOT, "docs/cluster-protocol-spec.md"),
    "measure": os.path.join(ROOT, "tools/cluster/scs_disc_measure.py"),
}
NAMES = {"header": "scs_disc.h", "daemon": "scsd.c", "spec": "spec.md",
         "measure": "scs_disc_measure.py"}

# (name, target, old, new). `old` must be present or the mutant FAILS to apply.
MUTANTS = [
    # --- digits in the header census ---------------------------------------
    ("HDR-pair-frames", "header",
     "DISC-CENSUS-PAIR: 62 6 -> 58 7 n=262 pcaps=25",
     "DISC-CENSUS-PAIR: 62 6 -> 58 7 n=263 pcaps=25"),
    ("HDR-pair-pcaps", "header",
     "DISC-CENSUS-PAIR: 62 6 -> 58 7 n=262 pcaps=25",
     "DISC-CENSUS-PAIR: 62 6 -> 58 7 n=262 pcaps=24"),
    ("HDR-pair-wrong-response", "header",
     "DISC-CENSUS-PAIR: 62 6 -> 58 7",
     "DISC-CENSUS-PAIR: 62 6 -> 58 5"),
    ("HDR-pair-row-deleted", "header",
     " * DISC-CENSUS-PAIR: 62 4 -> 58 5 n=696 pcaps=26\n", ""),
    ("HDR-pair-row-duplicated", "header",
     " * DISC-CENSUS-PAIR: 62 6 -> 58 7 n=262 pcaps=25",
     " * DISC-CENSUS-PAIR: 62 6 -> 58 7 n=262 pcaps=25\n"
     " * DISC-CENSUS-PAIR: 62 6 -> 58 7 n=999 pcaps=99"),
    ("HDR-population", "header",
     "DISC-CENSUS-POP: 62 6 n=220", "DISC-CENSUS-POP: 62 6 n=221"),
    ("HDR-match-frames", "header",
     "DISC-CENSUS-MATCH: rank=0 value=0x0000 n=131",
     "DISC-CENSUS-MATCH: rank=0 value=0x0000 n=132"),
    ("HDR-match-values-swapped", "header",
     "DISC-CENSUS-MATCH: rank=0 value=0x0000",
     "DISC-CENSUS-MATCH: rank=0 value=0x0001"),
    ("HDR-residuals", "header",
     "DISC-CENSUS-MATCH-RESIDUALS: 0", "DISC-CENSUS-MATCH-RESIDUALS: 1"),
    ("HDR-captures", "header",
     "DISC-CENSUS-CAPTURES: 47", "DISC-CENSUS-CAPTURES: 46"),

    # --- the latency figures that justify the shutdown bound ---------------
    ("DAEMON-latency-max", "daemon", "max=0.006919", "max=0.06919"),
    ("DAEMON-latency-unanswered", "daemon", "unanswered=0", "unanswered=3"),
    ("DAEMON-latency-line-deleted", "daemon",
     " * DISC-CENSUS-LAT: min=0.000006 p50=0.000286 p90=0.001041 p99=0.003459 max=0.006919\n",
     ""),
    ("DAEMON-shutdown-bound-below-the-margin", "daemon",
     "#define SCSD_SHUTDOWN_WAIT_MS 500", "#define SCSD_SHUTDOWN_WAIT_MS 2"),

    # --- the spec's two census tables --------------------------------------
    ("SPEC-census-D-frames", "spec",
     "| 62 B | `6` DISCONNECT_REQ | 58 B | `7` **DISCONNECT_RSP** | 262 | 25 |",
     "| 62 B | `6` DISCONNECT_REQ | 58 B | `7` **DISCONNECT_RSP** | 263 | 25 |"),
    ("SPEC-census-D-row-deleted", "spec",
     "| 62 B | `4` REJECT_REQ | 58 B | `5` **REJECT_RSP** | 696 | 26 |\n", ""),
    ("SPEC-census-D-marker-deleted", "spec", "<!-- CENSUS-D:", "<!-- CENSUS-X:"),
    ("SPEC-census-E-frames", "spec",
     "| `0x0001` | 89 | 18 |", "| `0x0001` | 90 | 18 |"),

    # --- EXPECTED itself drifts away from the documents --------------------
    ("EXPECTED-pair-frames", "measure",
     '"frames": 262, "pcaps": 25', '"frames": 264, "pcaps": 25'),
    ("EXPECTED-latency-max", "measure", '"max": 0.006919', '"max": 0.006920'),

    # --- the REFUTED claim comes back --------------------------------------
    ("REVIVE-verbatim-spec", "spec", "**A NEW GAP THIS OPENS",
     "Message types `5` and `7` do not exist on our wire.\n\n"
     "**A NEW GAP THIS OPENS"),
    ("REVIVE-reworded-spec", "spec", "**A NEW GAP THIS OPENS",
     "REJECT_RSP and DISCONNECT_RSP are absent from every capture we hold, so "
     "do not build one.\n\n**A NEW GAP THIS OPENS"),
    ("REVIVE-nor-spec", "spec", "**A NEW GAP THIS OPENS",
     "Neither 5 nor 7 appears in any capture we hold.\n\n"
     "**A NEW GAP THIS OPENS"),
    ("REVIVE-header", "header", " * THIS FILE'S SCOPE IS THE DISCONNECT PAIR.",
     " * `5` and `7` never appear on our wire.\n"
     " * THIS FILE'S SCOPE IS THE DISCONNECT PAIR."),

    # --- attacks on the quarantine mechanism itself ------------------------
    # Wrap a large region so the dead claim inside it is "quarantined".
    ("QUARANTINE-oversized", "spec", "**A COMPLETE TEARDOWN, frame by frame.**",
     "<!-- REFUTED-QUOTE-BEGIN -->\nMessage types 5 and 7 do not exist on our "
     "wire; do not build one.\n\n**A COMPLETE TEARDOWN, frame by frame.**"),
    # Open a second BEGIN inside the real block: the parsed span is the small
    # inner one, the text a reader sees as quarantined is the large outer one.
    ("QUARANTINE-nested", "spec", "<!-- REFUTED-QUOTE-BEGIN -->",
     "<!-- REFUTED-QUOTE-BEGIN -->\n<!-- REFUTED-QUOTE-BEGIN -->"),
    # Drop the markers and leave the quotation: the claim is then just asserted.
    ("QUARANTINE-markers-removed", "spec", "<!-- REFUTED-QUOTE-BEGIN -->\n", ""),

    # --- the NEW gap this item opened must not be quietly dropped ----------
    ("DROP-the-new-gap", "spec", "**nothing we hold identifies them**",
     "they are understood"),
]


def run_gate(paths):
    env = dict(os.environ)
    env["PYTHONDONTWRITEBYTECODE"] = "1"   # a same-size edit inside one clock
                                           # tick otherwise reuses a stale .pyc
    env["OVMX_SCS_DISC_HEADER"] = paths["header"]
    env["OVMX_SCS_DISC_DAEMON"] = paths["daemon"]
    env["OVMX_SCS_DISC_SPEC"] = paths["spec"]
    env["OVMX_SCS_DISC_MEASURE"] = paths["measure"]
    r = subprocess.run([sys.executable, GATE], env=env, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def main():
    tmp = tempfile.mkdtemp(prefix="scs_disc_mutants.")
    try:
        paths = {}
        for k, src in SRC.items():
            paths[k] = os.path.join(tmp, NAMES[k])
            shutil.copyfile(src, paths[k])
        # scs_disc_measure.py's lab1_only() (called directly by
        # test_scs_disc_figures.py's "THE LAB FENCE" section) lazily imports
        # tools/cluster/capture_manifest.py from its OWN directory -- which,
        # for the scratch copy, is `tmp`. Copy the manifest module alongside
        # it, the same way dissect_sca.py rides along for scs_reason_measure.
        shutil.copy2(os.path.join(os.path.dirname(SRC["measure"]),
                                  "capture_manifest.py"),
                     os.path.join(tmp, "capture_manifest.py"))

        def restore():
            for k, src in SRC.items():
                shutil.copyfile(src, paths[k])

        rc, out = run_gate(paths)
        if rc != 0:
            print("FAIL the CONTROL (unmutated scratch copy) is not green; no "
                  "kill below would mean anything.")
            print(out)
            return 1
        print("control: unmutated scratch copy is GREEN")

        killed = 0
        failed = []
        for name, target, old, new in MUTANTS:
            restore()
            with open(paths[target], "r", encoding="utf-8") as f:
                body = f.read()
            if old not in body:
                failed.append(f"{name}: anchor text not found in {NAMES[target]}. "
                              f"A mutant that does not change the file is not a "
                              f"mutant.")
                continue
            with open(paths[target], "w", encoding="utf-8") as f:
                f.write(body.replace(old, new, 1))
            rc, _ = run_gate(paths)
            if rc != 0:
                killed += 1
            else:
                failed.append(f"{name}: SURVIVED -- the gate stayed green with "
                              f"this mutation applied to {NAMES[target]}")
            restore()
            rc, _ = run_gate(paths)
            if rc != 0:
                failed.append(f"{name}: the control did not come back green "
                              f"after restoring; later results are unreliable")
                break

        for msg in failed:
            print(f"FAIL {msg}")
        print(f"{len(MUTANTS)} mutants, {killed} killed, {len(failed)} problem(s)")
        return 1 if failed else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
