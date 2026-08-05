# tests/vmsscs/scs_disc_mutants_no_python3.cmake  --  vms-591
#
# The BODY of the scs_disc_mutants ctest when CMake could not find a Python3
# interpreter at configure time. Run as `cmake -P` and exits non-zero, so the
# test REDS rather than vanishing (a skipped test is a failing test, OS rule 7).
message(FATAL_ERROR
    "scs_disc_mutants (vms-591) cannot run: no Python3 interpreter was found "
    "when this build tree was configured.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is what makes scs_disc_figures worth anything: it re-derives, every run, "
    "how many mutations that gate actually kills. Two REAL defects in the gate "
    "were found by this battery and by nothing else -- a quarantine parser that "
    "accepted NESTED markers, and a claim matcher that missed 'neither 5 nor "
    "7'. A figures gate with no mutation battery is how vms-6b3 shipped a "
    "refuted-claim check that could not fail.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached; delete CMakeCache.txt or re-configure "
    "from scratch).")
