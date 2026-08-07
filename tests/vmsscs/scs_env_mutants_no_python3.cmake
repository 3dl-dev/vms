# tests/vmsscs/scs_env_mutants_no_python3.cmake  --  vms-3f4
#
# The BODY of the scs_env_mutants ctest when CMake could not find a Python3
# interpreter at configure time. Run as `cmake -P` and exits non-zero, so the
# test REDS rather than vanishing (a skipped test is a failing test, OS rule 7).
message(FATAL_ERROR
    "scs_env_mutants (vms-3f4) cannot run: no Python3 interpreter was found "
    "when this build tree was configured.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. "
    "It is the MEASUREMENT behind scs_env_figures: it re-derives on every run "
    "how many guessed-name and stale-doc mutations that gate actually kills. "
    "This is the 5th generation of the same MTYPE 8/9 doc-gate failure class "
    "in this file (vms-182 -> vms-ab3 -> vms-c84 -> vms-3f4) and the only one "
    "of the eight comparable figures gates in tests/vmsscs/ that had no "
    "checked-in mutation battery -- a coverage claim that is never re-verified "
    "is exactly how the previous four rounds each found a new gap.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached; delete CMakeCache.txt or re-configure "
    "from scratch).")
