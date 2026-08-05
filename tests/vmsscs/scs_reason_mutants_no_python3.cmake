# tests/vmsscs/scs_reason_mutants_no_python3.cmake  --  vms-6b3
#
# This is the BODY of the scs_reason_mutants ctest when CMake could not find a
# Python3 interpreter at configure time. It is run as `cmake -P` (CMake is by
# definition present -- it is what invoked us), and it exits non-zero, so the
# test REDS.
#
# Round 3 registered scs_reason_mutants with this fallback path but never wrote
# the file. The safety property still held -- cmake reds when it cannot open the
# script it was told to run -- but the failure said "could not find -P script"
# instead of explaining which gate had gone missing and why that matters, so the
# reader would have looked for a build bug rather than for a lost gate. Same
# shape, and the same reasoning, as scs_reason_figures_no_python3.cmake next to
# it.
message(FATAL_ERROR
    "scs_reason_mutants (vms-6b3) cannot run: no Python3 interpreter was found "
    "when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_reason_mutants.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is the mutation battery that re-derives, on every run, whether "
    "scs_reason_figures can actually FAIL: it mutates scratch copies of "
    "src/vmsscs/include/scs_reason.h, docs/cluster-protocol-spec.md and "
    "tools/cluster/scs_reason_measure.py and requires the gate to red on each "
    "one. Without it the gate's coverage is back to being a claim in a comment "
    "-- which is the exact defect this item was rejected for twice, once for a "
    "refuted-claim check that could not fail and once for a rescue clause that "
    "a CENSUS table could satisfy on a re-assertion's behalf.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
