# tests/vmsscs/scs_dir_mutants_no_python3.cmake  --  vms-66f
#
# BODY of the scs_dir_mutants ctest when CMake found no Python3 interpreter at
# configure time. Run as `cmake -P`, exits non-zero, so the test REDS rather
# than vanishing (a skipped test is a failing test, OS rule 7).
message(FATAL_ERROR
    "scs_dir_mutants (vms-66f) cannot run: no Python3 interpreter was found when "
    "this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_dir_mutants.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is the MEASUREMENT behind scs_dir_figures: it re-derives on every run how "
    "many mutants that gate actually kills, so the coverage claim never lives "
    "in a comment. Its first-draft proximity-window predecessor let a "
    "re-assertion pasted under the correction pass, which is the defect this "
    "battery exists to keep dead.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
