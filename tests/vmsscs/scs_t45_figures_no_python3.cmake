# tests/vmsscs/scs_t45_figures_no_python3.cmake  --  vms-754
#
# BODY of the scs_t45_figures ctest when CMake found no Python3 interpreter at
# configure time. Run as `cmake -P`, exits non-zero, so the test REDS rather
# than vanishing (a skipped test is a failing test, OS rule 7). Same shape and
# same reasoning as scs_dir_figures_no_python3.cmake.
message(FATAL_ERROR
    "scs_t45_figures (vms-754) cannot run: no Python3 interpreter was found "
    "when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_t45_figures.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is the only thing in ctest that pins the MTYPE 4/5 decode (REJECT_REQ/"
    "REJECT_RSP vs scs_dir.c's refuted MSCP connect-ACCEPT/CONFIRM reading) to "
    "the EXPECTED table in tools/cluster/scs_t45_measure.py, and the only thing "
    "that keeps the refuted reading from being re-asserted outside a "
    "REFUTED-QUOTE-BEGIN/END block. Without it a wrong recorded measurement is "
    "invisible to a green run.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
