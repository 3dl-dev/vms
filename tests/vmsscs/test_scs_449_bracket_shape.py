#!/usr/bin/env python3
"""vms-449 -- THE MUTATION BATTERY FOR THE REJOIN BRACKET'S SHAPE.

`tools/cluster/scs_join_capability_measure.py:check_449_bracket_shape()` is the
only part of the rejoin measurement that runs without the captures, and it is
the part that decides whether the recorded answer -- *a returning OVMX identity
is refused readmission* -- was arrived at by an experiment worth believing:

  * a first join that actually joined, opening the bracket;
  * at least three rejoins, so a single refusal is not mistaken for a rule;
  * a control BETWEEN every pair of test runs and one CLOSING the bracket
    (guardrail 20), never all the controls piled up at one end;
  * one subject identity across every test run and no control reusing it
    (guardrail 18 -- a control that shares the subject's identity IS a rejoin);
  * every control joining (guardrail 19 -- a failed control means the harness
    is broken, not that the theory is confirmed);
  * and the wire discriminator holding as a RELATION over the whole table.

Method debt 4 of `docs/design-scs-followup.md`: *"a gate that cannot fail is
worse than none"* -- four figures gates in this epic stayed green while the
tools they cited failed. So this file does not assert that the shape check
passes; it asserts that the shape check FAILS on each specific thing it claims
to gate, restoring and re-verifying between every mutant. A mutant that leaves
the check green is scored as a FAILURE of the gate, never as a pass here.

Runs on every host. No captures, no lab, no network.
"""
import copy
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools", "cluster"))

import scs_join_capability_measure as M  # noqa: E402

BASE_ORDER = list(M.ORDER_449)
BASE_RUNS = copy.deepcopy(M.EXPECTED_449["runs"])
SUBJECT = M.EXPECTED_449["subject"]

BASE_ORDER_R = list(M.ORDER_449R)
BASE_RUNS_R = copy.deepcopy(M.EXPECTED_449R["runs"])
BASE_META_R = {k: M.EXPECTED_449R[k] for k in ("pod", "ovmx_mac", "subject")}
SUBJECT_R = M.EXPECTED_449R["subject"]

failures = []


def restore():
    M.ORDER_449[:] = list(BASE_ORDER)
    M.EXPECTED_449["runs"].clear()
    M.EXPECTED_449["runs"].update(copy.deepcopy(BASE_RUNS))
    M.ORDER_449R[:] = list(BASE_ORDER_R)
    M.EXPECTED_449R["runs"].clear()
    M.EXPECTED_449R["runs"].update(copy.deepcopy(BASE_RUNS_R))
    M.EXPECTED_449R.update(copy.deepcopy(BASE_META_R))


MUTANTS = {}
MUTANTS_R = {}


def mutant(name):
    def deco(fn):
        MUTANTS[name] = fn
        return fn
    return deco


def mutant_r(name):
    """A mutant of the REPLICATION's shape check (check_449r_bracket_shape)."""
    def deco(fn):
        MUTANTS_R[name] = fn
        return fn
    return deco


# --- the guardrails, each attacked directly --------------------------------

@mutant("two rejoins adjacent -- no control between them (guardrail 20)")
def _():
    M.ORDER_449[:] = ["A1", "B1", "B2", "C1", "C2", "B3", "C3", "B4", "C4"]


@mutant("the bracket does not CLOSE with a control (guardrail 20)")
def _():
    M.ORDER_449[:] = ["A1", "B1", "C1", "B2", "C2", "C3", "C4", "B3", "B4"]


@mutant("the first join is not the opening run")
def _():
    M.ORDER_449[:] = ["C1", "A1", "B1", "B2", "C2", "B3", "C3", "B4", "C4"]


@mutant("only one rejoin was attempted")
def _():
    for t in ("B2", "B3", "B4"):
        M.EXPECTED_449["runs"][t]["role"] = "control"


@mutant("a control reuses the subject identity (guardrail 18)")
def _():
    M.EXPECTED_449["runs"]["C2"]["identity"] = [SUBJECT]


@mutant("a test run's wire identity is not the subject")
def _():
    M.EXPECTED_449["runs"]["B2"]["identity"] = ["OVMXZZ"]


# --- the verdicts ----------------------------------------------------------

@mutant("a rejoin is recorded as JOINED (the answer inverted)")
def _():
    M.EXPECTED_449["runs"]["B2"]["joined"] = True


@mutant("a control is recorded as NOT joined (guardrail 19 hidden)")
def _():
    M.EXPECTED_449["runs"]["C2"]["joined"] = False


@mutant("the first join is recorded as NOT joined")
def _():
    M.EXPECTED_449["runs"]["A1"]["joined"] = False


# --- the wire discriminator ------------------------------------------------

@mutant("a refused rejoin is given a peer DISCONNECT pair")
def _():
    M.EXPECTED_449["runs"]["B2"]["ctl_rx"] = {6: 3, 7: 3}


@mutant("a joining run is given no peer DISCONNECT pair")
def _():
    M.EXPECTED_449["runs"]["C2"]["ctl_rx"] = {0: 9}


@mutant("an arm emits no ACCEPT_RSP (the vms-70e2 failing signature)")
def _():
    M.EXPECTED_449["runs"]["B2"]["accept_rsp_tx"] = 0


@mutant("ORDER_449 no longer covers EXPECTED_449")
def _():
    M.ORDER_449.pop()


# --- the REPLICATION's shape (check_449r_bracket_shape) --------------------
# Its job is different: it must NOT demand three rejoins (it has one), and it
# MUST demand that the replication was taken somewhere else. Those are the
# things attacked here.

@mutant_r("the replication names the same POD as the original")
def _():
    M.EXPECTED_449R["pod"] = M.EXPECTED_449["pod"]


@mutant_r("the replication reuses the original's OVMX tap MAC")
def _():
    M.EXPECTED_449R["ovmx_mac"] = M.EXPECTED_449["ovmx_mac"]


@mutant_r("the replication reuses the original's subject identity")
def _():
    M.EXPECTED_449R["subject"] = M.EXPECTED_449["subject"]


@mutant_r("the replication contains no rejoin at all")
def _():
    M.EXPECTED_449R["runs"]["B1"]["role"] = "control"


@mutant_r("the replication does not CLOSE with a control")
def _():
    M.ORDER_449R[:] = ["A1", "C1", "B1"]


@mutant_r("the replication's first join is not the opening run")
def _():
    M.ORDER_449R[:] = ["C1", "A1", "B1"]


@mutant_r("the replication's rejoin is recorded as JOINED")
def _():
    M.EXPECTED_449R["runs"]["B1"]["joined"] = True


@mutant_r("the replication's control is recorded as NOT joined")
def _():
    M.EXPECTED_449R["runs"]["C1"]["joined"] = False


@mutant_r("a control reuses the replication's subject identity")
def _():
    M.EXPECTED_449R["runs"]["C1"]["identity"] = [SUBJECT_R]


@mutant_r("the refused rejoin is given a peer DISCONNECT pair")
def _():
    M.EXPECTED_449R["runs"]["B1"]["ctl_rx"] = {6: 3, 7: 3}


@mutant_r("a joining run is given no peer DISCONNECT pair")
def _():
    M.EXPECTED_449R["runs"]["C1"]["ctl_rx"] = {0: 6}


@mutant_r("an arm emits no ACCEPT_RSP")
def _():
    M.EXPECTED_449R["runs"]["A1"]["accept_rsp_tx"] = 0


@mutant_r("ORDER_449R no longer covers EXPECTED_449R")
def _():
    M.ORDER_449R.pop()


def snapshot():
    """Everything a mutant is allowed to touch, so a no-op mutant is detected."""
    return (copy.deepcopy(M.EXPECTED_449["runs"]), list(M.ORDER_449),
            copy.deepcopy(M.EXPECTED_449R["runs"]), list(M.ORDER_449R),
            {k: M.EXPECTED_449R[k] for k in ("pod", "ovmx_mac", "subject")})


def score(label, check, mutants):
    """Apply each mutant alone and require `check` to RED. Returns kill count."""
    restore()
    green = check()
    if green:
        for m in green:
            failures.append("the UNMUTATED %s does not pass: %s" % (label, m))
        print("FATAL -- unmutated %s is already red; scoring is meaningless"
              % label)
        return 0

    print("unmutated %s: GREEN" % label)
    killed = 0
    for name, apply in mutants.items():
        restore()
        before = snapshot()
        apply()
        if before == snapshot():
            failures.append("mutant %r did not change anything -- it is not a "
                            "mutant and must not be scored as a kill" % name)
            print("%-62s -> NO-OP (scored as a FAILURE)" % name)
            continue
        if check():
            killed += 1
            print("%-62s -> KILLED" % name)
        else:
            failures.append("mutant SURVIVED: %s" % name)
            print("%-62s -> *** SURVIVED ***" % name)
        # restore and re-verify, so one mutant cannot mask the next
        restore()
        if check():
            failures.append("the %s did not return to green after mutant %r"
                            % (label, name))
    print("%d/%d mutants killed" % (killed, len(mutants)))
    return killed


def main():
    score("check_449_bracket_shape()", M.check_449_bracket_shape, MUTANTS)
    print()
    # The REPLICATION's shape check is scored SEPARATELY and against its own
    # mutants: it asserts a deliberately weaker bracket (one rejoin) plus the
    # cross-checks that make it a replication at all. Folding the two batteries
    # together would let a mutant of one be "killed" by the other's rules.
    score("check_449r_bracket_shape()", M.check_449r_bracket_shape, MUTANTS_R)

    for f in failures:
        print("FAIL: " + f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
