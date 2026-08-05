# tests/vmsscs/scs_figures_wire_mutants_no_python3.cmake  --  vms-371
#
# The BODY of the scs_figures_wire_mutants ctest when CMake found no Python3
# interpreter at configure time. Run as `cmake -P`, it exits non-zero so the
# test REDS rather than silently vanishing. Same shape, and the same reasoning,
# as scs_credit_figures_no_python3.cmake -- read that file for why this is not
# an `if()` with a silent else and not a configure-time FATAL_ERROR.
message(FATAL_ERROR
    "scs_figures_wire_mutants (vms-371) cannot run: no Python3 interpreter was "
    "found when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_figures_wire_mutants.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is the battery that proves the six SCS figures gates can actually FAIL: it "
    "mutates the packets under them, hollows out their re-derivation, adds a "
    "figure nothing measures, and performs the same-size same-second edit behind "
    "a primed __pycache__ that a stale .pyc would hide. Without it the gates are "
    "back to comparing a table against prose -- which is how `ctest -L scs` "
    "stayed 32/32 green while four measurement tools were red.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
