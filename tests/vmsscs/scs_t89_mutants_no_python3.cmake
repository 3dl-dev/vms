# tests/vmsscs/scs_t89_mutants_no_python3.cmake  --  vms-a58
#
# The BODY of the scs_t89_mutants ctest when CMake could not find a Python3
# interpreter at configure time. Run as `cmake -P` and exits non-zero, so the
# test REDS rather than vanishing (a skipped test is a failing test, OS rule 7).
message(FATAL_ERROR
    "scs_t89_mutants (vms-a58) cannot run: no Python3 interpreter was found "
    "when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_t89_mutants.py with.\n"
    "This test is what makes scs_t89_figures worth having: it mutates COPIES "
    "of the spec and of scsd.c and requires that gate to RED for every one -- "
    "a drifted census figure, a deleted honest limit, a credit-ledger residual "
    "that appears from nowhere, the p. 2-44 special credit message written "
    "back in as an identification, and the removal of the emission ruling. It "
    "has already found one real defect in that gate (a sentence splitter that "
    "cut 'p. 2-44' in half and so could not see any claim stated with a page "
    "cite).\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
