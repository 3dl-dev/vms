# tests/vmsscs/scs_dir_figures_no_python3.cmake  --  vms-66f
#
# BODY of the scs_dir_figures ctest when CMake found no Python3 interpreter at
# configure time. Run as `cmake -P`, exits non-zero, so the test REDS rather
# than vanishing (a skipped test is a failing test, OS rule 7). Same shape and
# same reasoning as scs_reason_figures_no_python3.cmake.
message(FATAL_ERROR
    "scs_dir_figures (vms-66f) cannot run: no Python3 interpreter was found when "
    "this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_dir_figures.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is the only thing in ctest that pins the SCS$DIRECTORY role census and the "
    "lookup [48:50] credit census -- in docs/cluster-protocol-spec.md sec "
    "4(h)(2a), src/vmsscs/scs_dir.c and src/vmsscs/scsd.c -- to the EXPECTED "
    "table in tools/cluster/scs_dir_role_measure.py, and the only thing that "
    "keeps two sentences REFUTED BY THEIR OWN CITED CAPTURE from being "
    "re-asserted. Without it a wrong recorded measurement is invisible to a "
    "green run, which is exactly how those two sentences shipped.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
