# tests/vmsscs/scs_join_capability_mutants_no_python3.cmake  --  vms-371
#
# The BODY of the scs_join_capability_mutants ctest when CMake found no Python3
# interpreter at configure time. Run as `cmake -P`, it exits non-zero so the
# test REDS rather than silently vanishing. Same shape, and the same reasoning,
# as scs_credit_figures_no_python3.cmake -- read that file for why this is not
# an `if()` with a silent else and not a configure-time FATAL_ERROR.
#
# WHY THIS FILE APPEARS WITH vms-371: the else() branch in
# tests/vmsscs/CMakeLists.txt named this path from the day
# scs_join_capability_mutants was registered, but the file was never written,
# so a Python3-less host got "cmake -P could not find the file" instead of a
# message naming the gate or the fix. Found while auditing which figures gates
# can actually run.
message(FATAL_ERROR
    "scs_join_capability_mutants cannot run: no Python3 interpreter was found "
    "when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_join_capability_mutants.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is the mutation battery that proves scs_join_capability_figures can FAIL "
    "-- a gate whose battery is skipped is a gate nobody has shown to be a "
    "gate, which is the exact defect vms-371 exists to close.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
