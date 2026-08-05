#!/usr/bin/env python3
"""
test_scs_join_capability_mutants.py -- vms-70e2: THE MUTATION BATTERY for
test_scs_join_capability_figures.py.

WHY IT EXISTS. The figures gate is the only thing in ctest that (a) exercises
the decoder behind every number in docs/cluster-protocol-spec.md sec 4(O), (b)
pins those numbers to the prose, and (c) keeps the refuted "OVMX HAS NEVER
OBSERVED AN ACCEPT_RSP ADDRESSED TO ITSELF" claim quarantined. "It is green"
says nothing about whether it kills anything, and the vms-6b3 precedent in this
epic is a figures gate whose refuted-claim check could not fail while its
comment said it could.

It found two real weaknesses in the gate while it was being written:
  - the synthesized frames imported the message-type offset FROM the
    measurement, so a mutant that moved the offset moved the test's own frame
    with it and survived. The offset is now written out in the test;
  - the first "identity" mutant did not change behaviour at all, and a mutant
    that does not change the file is scored here as a FAILURE, never a kill.

HOW IT WORKS. Copies the measurement, the spec and scs_sdir.h into a scratch
tree, proves the unmutated copies are green (the control), then applies each
mutant alone and requires the gate to exit non-zero, restoring and re-verifying
between each. Nothing under src/, docs/ or tools/ is written -- the gate is
pointed at the scratch copies through OVMX_SCS_JOINCAP_{MEASURE,SPEC,HEADER}.

The captures are deliberately hidden from the gate during this run
(OVMX_LAB_CAPTURES points at a path that does not exist), so the battery scores
the part of the gate that runs on EVERY host, including CI. A mutant that only
dies because a lab host happens to hold the pcaps is not a gate.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
GATE = os.path.join(HERE, "test_scs_join_capability_figures.py")

SRC = {
    "measure": os.path.join(ROOT, "tools/cluster/scs_join_capability_measure.py"),
    "spec": os.path.join(ROOT, "docs/cluster-protocol-spec.md"),
    "header": os.path.join(ROOT, "src/vmsscs/include/scs_sdir.h"),
}
NAMES = {"measure": "scs_join_capability_measure.py", "spec": "spec.md",
         "header": "scs_sdir.h"}

# (name, target, old, new). `old` must be present or the mutant FAILS to apply.
MUTANTS = [
    # --- the decoder ------------------------------------------------------
    ("MEASURE-count-peer-to-peer", "measure",
     "        if not (tx or rx):\n            # Peer-to-peer traffic.",
     "        if False:\n            # Peer-to-peer traffic."),
    ("MEASURE-widen-ctl-classes-to-106", "measure",
     "CTL_CLASSES = (58, 62, 66, 110)",
     "CTL_CLASSES = (58, 62, 66, 106, 110)"),
    ("MEASURE-widen-ctl-classes-to-94", "measure",
     "CTL_CLASSES = (58, 62, 66, 110)",
     "CTL_CLASSES = (58, 62, 66, 94, 110)"),
    ("MEASURE-msgtype-offset-moved", "measure",
     "MSGTYPE_OFF = 46",
     "MSGTYPE_OFF = 50"),
    ("MEASURE-accept-rsp-counts-received", "measure",
     'out["accept_rsp_tx"] = out["ctl_tx"].get(3, 0)',
     'out["accept_rsp_tx"] = out["ctl_rx"].get(3, 0)'),
    ("MEASURE-cm-direction-swapped", "measure",
     'out["cm_190_tx" if tx else "cm_190_rx"] += 1',
     'out["cm_190_rx" if tx else "cm_190_tx"] += 1'),
    ("MEASURE-ctl-direction-swapped", "measure",
     'bucket = out["ctl_tx" if tx else "ctl_rx"]',
     'bucket = out["ctl_rx" if tx else "ctl_tx"]'),
    ("MEASURE-identity-not-harvested", "measure",
     '            out["identity"].add(name.decode("ascii"))',
     '            pass'),
    ("MEASURE-cm-class-changed", "measure",
     "CM_CLASS = 190", "CM_CLASS = 191"),
    ("MEASURE-short-frame-not-guarded", "measure",
     "if len(sca) in CTL_CLASSES and len(sca) >= MSGTYPE_OFF + 2:",
     "if len(sca) in CTL_CLASSES or len(sca) >= MSGTYPE_OFF + 2:"),

    # --- the figures in EXPECTED, which the prose must still quote ---------
    ("EXPECTED-A0-cm-rx", "measure", '"cm_190_rx": 583,', '"cm_190_rx": 584,'),
    ("EXPECTED-A1-accept-rsp", "measure",
     '"ctl_tx": {0: 1, 1: 1, 2: 1, 6: 2},\n            "accept_rsp_tx": 0,',
     '"ctl_tx": {0: 1, 1: 1, 2: 1, 6: 2},\n            "accept_rsp_tx": 2,'),
    ("EXPECTED-A0-identity", "measure", '"identity": ["OVMXA0"],',
     '"identity": ["OVMXQ9"],'),
    ("EXPECTED-pod", "measure", '"pod": "vaxlab-4",', '"pod": "vaxlab-9",'),

    # --- the prose -------------------------------------------------------
    ("SPEC-drop-the-non-claim", "spec",
     "does **not** establish", "does establish"),
    ("SPEC-change-A0-inbound-cm", "spec", "| **514** | **583** |",
     "| **514** | **999** |"),
    ("SPEC-drop-unemitted-counter", "spec",
     "actions-required-but-not-emitted=1", "actions-required-but-not-emitted"),
    ("SPEC-drop-the-4O-section", "spec", "### 4(O)", "### 4(OO)"),

    # --- the quarantine ---------------------------------------------------
    ("QUARANTINE-header-claim-restated", "header",
     " * 3. RETURN TO LISTEN",
     " * OVMX HAS NEVER OBSERVED AN ACCEPT_RSP addressed to itself.\n"
     " * 3. RETURN TO LISTEN"),
    ("QUARANTINE-header-markers-removed", "header",
     " *    REFUTED-QUOTE-BEGIN\n", ""),
    ("QUARANTINE-header-nested-begin", "header",
     " *    REFUTED-QUOTE-BEGIN\n",
     " *    REFUTED-QUOTE-BEGIN\n *    REFUTED-QUOTE-BEGIN\n"),
    ("QUARANTINE-spec-markers-removed", "spec",
     "<!-- REFUTED-QUOTE-BEGIN -->\n", ""),
    ("QUARANTINE-spec-nested-begin", "spec",
     "<!-- REFUTED-QUOTE-BEGIN -->\n",
     "<!-- REFUTED-QUOTE-BEGIN -->\n<!-- REFUTED-QUOTE-BEGIN -->\n"),
    ("QUARANTINE-header-loses-the-capture", "header",
     "vms70e2-A1-lab2-vaxlab4-20260805.pcap", "some-capture.pcap"),
    ("QUARANTINE-header-loses-the-conids", "header",
     "4F580007/B751000C", "an OVMX connection"),
]


def run_gate(paths):
    env = dict(os.environ)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["OVMX_SCS_JOINCAP_MEASURE"] = paths["measure"]
    env["OVMX_SCS_JOINCAP_SPEC"] = paths["spec"]
    env["OVMX_SCS_JOINCAP_HEADER"] = paths["header"]
    # Score only what runs on every host. See the module docstring.
    env["OVMX_LAB_CAPTURES"] = os.path.join(paths["measure"], "no-captures-here")
    r = subprocess.run([sys.executable, GATE], env=env, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def main():
    tmp = tempfile.mkdtemp(prefix="scs_joincap_mutants.")
    try:
        paths = {}
        for k, src in SRC.items():
            paths[k] = os.path.join(tmp, NAMES[k])
            shutil.copyfile(src, paths[k])

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
            body = open(paths[target], encoding="utf-8").read()
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
