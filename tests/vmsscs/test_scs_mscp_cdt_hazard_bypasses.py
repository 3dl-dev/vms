#!/usr/bin/env python3
"""
test_scs_mscp_cdt_hazard_bypasses.py -- vms-cf0: THE BYPASS BATTERY for
test_scs_mscp_cdt_hazard.py (the vms-73c UNDISCLOSED DOUBLE-GRANT CENSUS).

WHY IT EXISTS. The v1 gate matched a struct scs_mscp_params declaration and
its `.cdt =` / `->cdt =` write line by line, with the identifier required
immediately adjacent to the dot/arrow. Three independent, demonstrated PoCs
came back GREEN on an undisclosed live-CDT wire-up -- i.e. the gate the
comment in scs_mscp.c relies on silently passed the exact hazard it exists to
catch:

  B-1  SPLIT-ACROSS-LINES     -- declaration or assignment broken onto two
                                  physical lines defeats the per-line scanner.
  B-2  TYPEDEF ALIAS          -- a typedef'd alias of struct scs_mscp_params
                                  is never recognized as the type, so its
                                  declared identifiers are never collected.
  B-3  NON-ADJACENT LHS       -- an array subscript (`arr[0].cdt`) or a
                                  parenthesized dereference (`(*pmp).cdt`)
                                  breaks the "identifier immediately left of
                                  the dot/arrow" assumption.

test_scs_mscp_cdt_hazard.py was hardened to whole-file scanning (closes B-1),
typedef-alias folding (closes B-2), and a broadened assignment match plus a
dedicated paren-deref pattern (closes B-3). THIS SCRIPT IS THE PROOF: it
writes each PoC (and its acknowledged / decoy counterpart) as a throwaway
fixture under a tempdir -- nothing under src/, docs/, tools/ or the rest of
tests/ is ever touched -- points the hardened gate at it via its
argv-accepts-explicit-targets support, and asserts the gate's exit code and
the presence/absence of a FAIL line for that fixture.

A case whose expectation does not hold is a SURVIVOR (an unclosed bypass) or
a FALSE POSITIVE (a decoy or an acknowledged write wrongly flagged), both
reported by name so a re-introduced bypass has a name in the ctest log, not
just a bare non-zero exit.
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GATE = os.path.join(HERE, "test_scs_mscp_cdt_hazard.py")

ACK_COMMENT = (
    "/* CREDIT-HAZARD-ACKNOWLEDGED: reset modeled on "
    "take_pending_receive_credit() added below */"
)

# Each case: (id, source text, expect_red: bool)
# expect_red=True  -> the fixture carries an UNDISCLOSED live-CDT wire-up and
#                      the gate MUST exit non-zero (a bypass would exit 0).
# expect_red=False -> the fixture is either disclosed (ACK comment present)
#                      or a decoy (no struct scs_mscp_params involved at all)
#                      and the gate MUST exit 0 (a regression would exit
#                      non-zero -- a false positive is also a broken gate).
CASES = [
    # --- B-1: split across a newline -----------------------------------
    ("B1-decl-split-undisclosed", """\
void f(void) {
    struct scs_mscp_params
        mp;
    mp.cdt = &live_cdt;
}
""", True),
    ("B1-assign-split-undisclosed", """\
void f(void) {
    struct scs_mscp_params mp;
    mp
        .cdt = &live_cdt;
}
""", True),
    ("B1-both-split-acknowledged", """\
void f(void) {
    struct scs_mscp_params
        mp;
    %s
    mp
        .cdt = &live_cdt;
}
""" % ACK_COMMENT, False),

    # --- B-2: typedef alias ---------------------------------------------
    ("B2-typedef-alias-undisclosed", """\
typedef struct scs_mscp_params mp_params_t;
void f(void) {
    mp_params_t mp;
    mp.cdt = &live_cdt;
}
""", True),
    ("B2-typedef-alias-pointer-undisclosed", """\
typedef struct scs_mscp_params mp_params_t;
void f(void) {
    mp_params_t *mp;
    mp->cdt = &live_cdt;
}
""", True),
    ("B2-typedef-alias-acknowledged", """\
typedef struct scs_mscp_params mp_params_t;
void f(void) {
    mp_params_t mp;
    %s
    mp.cdt = &live_cdt;
}
""" % ACK_COMMENT, False),

    # --- B-3: non-adjacent LHS -------------------------------------------
    ("B3-array-subscript-undisclosed", """\
void f(void) {
    struct scs_mscp_params arr[2];
    arr[0].cdt = &live_cdt;
}
""", True),
    ("B3-paren-deref-undisclosed", """\
void f(void) {
    struct scs_mscp_params mp;
    struct scs_mscp_params *pmp = &mp;
    (*pmp).cdt = &live_cdt;
}
""", True),
    ("B3-array-subscript-acknowledged", """\
void f(void) {
    struct scs_mscp_params arr[2];
    %s
    arr[0].cdt = &live_cdt;
}
""" % ACK_COMMENT, False),
    ("B3-paren-deref-acknowledged", """\
void f(void) {
    struct scs_mscp_params mp;
    struct scs_mscp_params *pmp = &mp;
    %s
    (*pmp).cdt = &live_cdt;
}
""" % ACK_COMMENT, False),

    # --- decoys: must stay green through the SAME broadened patterns ----
    ("DECOY-scs-poller-array-subscript", """\
struct scs_poller { struct scs_cdt *cdt; };
void f(void) {
    struct scs_poller pollers[2];
    pollers[0].cdt = 0;
}
""", False),
    ("DECOY-scs-poller-paren-deref", """\
struct scs_poller { struct scs_cdt *cdt; };
void f(void) {
    struct scs_poller p;
    struct scs_poller *w = &p;
    (*w).cdt = 0;
}
""", False),
    ("DECOY-unrelated-typedef", """\
typedef struct scs_poller { struct scs_cdt *cdt; } poller_t;
void f(void) {
    poller_t w;
    w.cdt = 0;
}
""", False),

    # --- M-1..M-4 (vms-73c) re-run against the hardened, whole-file
    # scanner, so hardening B-1/B-2/B-3 is proven not to have broken the
    # original battery.
    ("M1-undisclosed-baseline", """\
void f(void) {
    struct scs_mscp_params mp;
    mp.cdt = &live_cdt;
}
""", True),
    ("M2-acknowledged-same-line", """\
void f(void) {
    struct scs_mscp_params mp;
    mp.cdt = &live_cdt; %s
}
""" % ACK_COMMENT, False),
    ("M3-acknowledged-line-above", """\
void f(void) {
    struct scs_mscp_params mp;
    %s
    mp.cdt = &live_cdt;
}
""" % ACK_COMMENT, False),
    ("M4-decoy-scs-poller-arrow", """\
struct scs_poller { struct scs_cdt *cdt; };
void f(void) {
    struct scs_poller *poller;
    poller->cdt = 0;
}
""", False),
]


def run_gate(path):
    env = dict(os.environ)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    p = subprocess.run([sys.executable, GATE, path], env=env,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def main():
    ids = [c[0] for c in CASES]
    dupes = sorted({i for i in ids if ids.count(i) > 1})
    if dupes:
        print("FAIL duplicate case ids: %r" % (dupes,))
        return 1

    tmp = tempfile.mkdtemp(prefix="scs_mscp_cdt_hazard_bypasses.")
    try:
        survivors = []
        false_positives = []
        for case_id, src, expect_red in CASES:
            path = os.path.join(tmp, case_id + ".c")
            with open(path, "w", encoding="utf-8") as f:
                f.write(src)
            rc, out = run_gate(path)
            is_red = rc != 0
            if expect_red and not is_red:
                survivors.append((case_id, out.strip().splitlines()[-1] if out.strip() else "(no output)"))
            elif not expect_red and is_red:
                false_positives.append((case_id, out.strip().splitlines()[-1] if out.strip() else "(no output)"))

        for case_id, last in survivors:
            print("FAIL case %s is a SURVIVOR: the gate did not red an "
                  "undisclosed live-CDT wire-up. %s" % (case_id, last))
        for case_id, last in false_positives:
            print("FAIL case %s is a FALSE POSITIVE: the gate reddened a "
                  "disclosed write or an unrelated decoy. %s" % (case_id, last))

        print("%d cases, %d survived (undetected bypass), %d false positive(s)"
              % (len(CASES), len(survivors), len(false_positives)))
        return 1 if (survivors or false_positives) else 0
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
