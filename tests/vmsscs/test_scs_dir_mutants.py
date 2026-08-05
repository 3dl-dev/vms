#!/usr/bin/env python3
"""
test_scs_dir_mutants.py -- the MUTATION BATTERY for scs_dir_figures.

vms-66f, review round 2. The gate this battery attacks exists because two
claims in this epic were refuted by the capture they cited. Its FIRST draft
checked the dead sentences with a PROXIMITY WINDOW -- 40 lines around
"REFUTED"/"corrected"/the item id -- which is the exact artifact vms-6b3 was
rejected for: inside documents that are entirely ABOUT the refutation, the
excuse words are everywhere, so a re-assertion pasted three lines under the
correction passes. The gate now quarantines dead quotes in explicit
REFUTED-QUOTE-BEGIN/END blocks, and the coverage claim stops being a comment:
THIS TEST IS THE CLAIM, and it re-derives the number on every ctest run.

Method, the same discipline as test_scs_reason_mutants.py:

  1. copy every input the gate reads into a scratch tree that mirrors the repo
     layout (nothing under src/, docs/, tools/ or tests/ is ever written),
  2. assert the UNMUTATED copy is GREEN -- the control, without which no kill
     below means anything,
  3. apply each mutant alone, run the gate against the scratch tree via
     OVMX_SCS_DIR_ROOT, and require a NON-ZERO exit,
  4. restore and RE-VERIFY THE CONTROL after every mutant, so a wedged tree
     cannot score the rest of the battery as kills.

A mutant whose anchor text is absent, or whose edit does not change the file,
is reported as a FAILURE and never scored as a kill.
"""

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
GATE = os.path.join(HERE, "test_scs_dir_figures.py")

# Repo-relative paths the gate reads. Keys are the short names the mutants use.
SRC = {
    "spec": "docs/cluster-protocol-spec.md",
    "dir_c": "src/vmsscs/scs_dir.c",
    "scsd_c": "src/vmsscs/scsd.c",
    "dir_h": "src/vmsscs/include/scs_dir.h",
    "poll_h": "src/vmsscs/include/scs_poll.h",
    "poll_c": "src/vmsscs/scs_poll.c",
    "test_dir": "tests/vmsscs/test_scs_dir.c",
    "test_poll": "tests/vmsscs/test_scs_poll.c",
    "measure": "tools/cluster/scs_dir_role_measure.py",
    # imported at module scope by the measure script
    "dissect": "tools/cluster/dissect_sca.py",
}

# A bare re-assertion of each dead sentence, in the wording that was rejected.
RE_ROLE = "In the golden capture VAX2 only answers."
RE_POLL = "The joiner opens its connection without having polled anybody."
RE_FLAG = "[48:50] is a request; the response has 1."

MUTANTS = [
    # --- A. drifted figures: the prose no longer states the measurement -----
    ("FIG-spec-req-hist", "spec", "{0: 2, 1: 4}", "{0: 1, 1: 5}"),
    ("FIG-dirc-req-hist", "dir_c", "{0: 2, 1: 4}", "{0: 1, 1: 5}"),
    ("FIG-spec-rsp-hist", "spec", "{1: 6}", "{1: 5}"),
    ("FIG-dirc-rsp-hist", "dir_c", "{1: 6}", "{1: 5}"),
    # Drift EXPECTED instead of the prose: the gate reads EXPECTED, so the two
    # must move together or neither moves.
    ("FIG-expected-req-hist", "measure",
     '"request_credit_hist": {0: 2, 1: 4},', '"request_credit_hist": {0: 3, 1: 3},'),
    ("FIG-expected-length-hist", "measure",
     '"lookup_length_hist": {94: 12},', '"lookup_length_hist": {94: 11},'),
    ("FIG-expected-joiner-connects", "measure",
     '"vaxcluster_connect_req_from_joiner": 0,',
     '"vaxcluster_connect_req_from_joiner": 1,'),
    # The frame numbers are the load-bearing ones -- they are what inverts the
    # roles -- and the 0-based/1-based slip is how the original error happened.
    ("FIG-expected-joiner-poll-frame", "measure",
     '(1237, "VAX2", "VAX1")', '(1238, "VAX2", "VAX1")'),
    ("FIG-expected-vc-connect-frame", "measure",
     '"vaxcluster_connect_req_frames": [(47, "VAX1", "VAX2")],',
     '"vaxcluster_connect_req_frames": [(48, "VAX1", "VAX2")],'),
    ("FIG-expected-accept-frame", "measure",
     '"vaxcluster_accept_req_frames": [(50, "VAX2", "VAX1")],',
     '"vaxcluster_accept_req_frames": [(51, "VAX2", "VAX1")],'),

    # --- B. the dead sentences re-asserted at natural sites -----------------
    # B1 is the one the proximity window could not catch: three lines under the
    # correction itself. It is the regression pin for the quarantine rule.
    ("REASSERT-spec-under-the-correction", "spec",
     "**Both halves of that are false**, and the roles were inverted.",
     RE_ROLE + " **Both halves of that are false**, and the roles were inverted."),
    ("REASSERT-spec-elsewhere", "spec",
     "### 4(i) Joining an ALREADY-ESTABLISHED cluster",
     RE_POLL + "\n\n### 4(i) Joining an ALREADY-ESTABLISHED cluster"),
    ("REASSERT-dirc-template-comment", "dir_c",
     "/* SCA#29 LOOKUP-REQUEST (94 bytes)",
     "/* " + RE_FLAG + " */\n/* SCA#29 LOOKUP-REQUEST (94 bytes)"),
    ("REASSERT-scsd-elsewhere", "scsd_c",
     "static struct scsd_rx *scsd_poll_rx = NULL;",
     "/* " + RE_POLL + " */\nstatic struct scsd_rx *scsd_poll_rx = NULL;"),
    ("REASSERT-pollc", "poll_c",
     "#include \"scs_poll.h\"",
     "/* " + RE_ROLE + " */\n#include \"scs_poll.h\""),
    ("REASSERT-dirh", "dir_h",
     "#ifndef SCS_DIR_H",
     "/* " + RE_FLAG + " */\n#ifndef SCS_DIR_H"),
    ("REASSERT-test-poll", "test_poll",
     "#include \"scs_poll.h\"",
     "/* " + RE_ROLE + " */\n#include \"scs_poll.h\""),
    ("REASSERT-pollh", "poll_h",
     "#ifndef SCS_POLL_H",
     "/* " + RE_POLL + " */\n#ifndef SCS_POLL_H"),

    # --- C. attacks on the quarantine itself --------------------------------
    # Unbalance it: an open BEGIN with no END must yield NO span, not a licence
    # that runs to the end of the file.
    # ANCHORED to the LAST line of THIS item's block, not to a bare
    # "<!-- REFUTED-QUOTE-END -->". The spec now carries several quarantine
    # blocks (vms-591 added one ABOVE this one), and a bare anchor with count=1
    # deleted SOMEBODY ELSE'S end marker -- which this gate is not responsible
    # for and does not red on, so the mutant survived. Caught on the rebase onto
    # work/vms-187-closure; the lesson is that a mutant anchor must name the
    # construct it is attacking, not the first textual match of its type.
    ("QUARANTINE-spec-end-removed", "spec",
     "> \"opens its own `VMS$VAXcluster` connection without having polled "
     "anybody\".\n<!-- REFUTED-QUOTE-END -->\n",
     "> \"opens its own `VMS$VAXcluster` connection without having polled "
     "anybody\".\n"),
    ("QUARANTINE-scsd-end-removed", "scsd_c", " *      REFUTED-QUOTE-END\n", ""),
    # Remove the markers but keep the quotation: the dead claim is then simply
    # asserted in the document.
    ("QUARANTINE-spec-markers-removed", "spec",
     "<!-- REFUTED-QUOTE-BEGIN -->\n> (revision 1, quoted here only to kill it "
     "— every clause below is REFUTED)\n> the directory exchange",
     "> the directory exchange"),
    ("QUARANTINE-dirc-block-deleted", "dir_c",
     " * REFUTED-QUOTE-BEGIN\n *   \"flag = 0 (a request; the response has 1)\"\n"
     " * REFUTED-QUOTE-END\n",
     ""),

    # --- D. the correction's own load-bearing parts -------------------------
    ("CITE-spec-tool-erased", "spec",
     "tools/cluster/scs_dir_role_measure.py", "the measuring script", -1),
    ("CITE-scsd-tool-erased", "scsd_c",
     "tools/cluster/scs_dir_role_measure.py", "the measuring script", -1),
    ("CITE-dirc-tool-erased", "dir_c",
     "tools/cluster/scs_dir_role_measure.py", "the measuring script", -1),
    ("ATTRIB-dirc-sec4d-erased", "dir_c", "sec 4(d)", "sec 9(z)", -1),

    # --- E. a template relabelled back to the refuted reading ---------------
    ("RELABEL-dirc-credit-to-flag", "dir_c",
     "/* credit = 0 (sec 4d) */", "/* flag = 0 */"),
    ("RELABEL-dirc-sendcredits-to-flag", "dir_c",
     "/* Send Credits SCS$DIRECTORY extends = 3 (GROUNDED, sec 4d) */",
     "/* companion flag, replayed (inferred) */"),
]


def run_gate(root):
    env = dict(os.environ)
    env["OVMX_SCS_DIR_ROOT"] = root
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    # vms-371: force the HOST-INDEPENDENT arm. This battery scores the gate's
    # PROSE-vs-EXPECTED pinning; the wire arm is scored by
    # test_scs_figures_wire_mutants.py, which mutates the packets themselves.
    # Pointing OVMX_LAB_CAPTURES at a path that cannot exist makes every mutant
    # here run against the same, deterministic half of the gate -- and keeps a
    # scratch copy of a measure script (which has no dissector beside it) from
    # reddening the CONTROL for a reason that has nothing to do with the mutant.
    env["OVMX_LAB_CAPTURES"] = os.path.join(root, "no-such-capture-dir")
    env.pop("OVMX_SCS_REQUIRE_WIRE", None)
    p = subprocess.run([sys.executable, GATE], env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def main():
    ids = [m[0] for m in MUTANTS]
    dupes = sorted({i for i in ids if ids.count(i) > 1})
    if dupes:
        print("FAIL duplicate mutant ids: %r" % (dupes,))
        return 1

    tmp = tempfile.mkdtemp(prefix="scs_dir_mutants.")
    try:
        pristine = {}
        work = {}
        for key, rel in SRC.items():
            src = os.path.join(ROOT, rel)
            dst = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
            pristine[key] = open(src, "rb").read()
            work[key] = dst

        def restore():
            for k, path in work.items():
                with open(path, "wb") as f:
                    f.write(pristine[k])

        restore()
        rc, out = run_gate(tmp)
        if rc != 0:
            print("FAIL the CONTROL (unmutated scratch copy) is not green; no "
                  "kill below would mean anything.\n%s" % out)
            return 1
        print("control: unmutated scratch copy is GREEN (%s)"
              % out.strip().splitlines()[-1])

        killed, survivors, unapplied = [], [], []
        for entry in MUTANTS:
            mid, which, old, new = entry[:4]
            count = entry[4] if len(entry) > 4 else 1
            restore()
            text = pristine[which].decode("utf-8")
            if old not in text:
                unapplied.append((mid, which))
                continue
            mutated = text.replace(old, new, count) if count >= 0 else text.replace(old, new)
            if mutated == text:
                unapplied.append((mid, which))
                continue
            with open(work[which], "wb") as f:
                f.write(mutated.encode("utf-8"))
            rc, out = run_gate(tmp)
            if rc != 0:
                killed.append(mid)
            else:
                survivors.append((mid, out.strip().splitlines()[-1]))
            restore()
            rc2, out2 = run_gate(tmp)
            if rc2 != 0:
                print("FAIL control did not come back green after mutant %s -- "
                      "the battery is unsound from here.\n%s" % (mid, out2))
                return 1

        for mid, which in unapplied:
            print("FAIL mutant %s did not apply: its anchor text is not in %s. "
                  "A mutant that does not change the file is not a mutant."
                  % (mid, SRC[which]))
        for mid, out in survivors:
            print("FAIL mutant %s SURVIVED scs_dir_figures: %s" % (mid, out))

        print("%d mutants, %d killed, %d survived, %d failed to apply"
              % (len(MUTANTS), len(killed), len(survivors), len(unapplied)))
        return 1 if (survivors or unapplied) else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
