# tests/vmsscs/scs_diskrun_mutants_no_python3.cmake  --  vms-ebb
#
# The BODY of the scs_diskrun_mutants ctest when CMake could not find a Python3
# interpreter at configure time. Run as `cmake -P` and exits non-zero, so the
# test REDS rather than vanishing (a skipped test is a failing test, OS rule 7).
message(FATAL_ERROR
    "scs_diskrun_mutants (vms-ebb) cannot run: no Python3 interpreter was found "
    "when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_diskrun_mutants.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is what makes scs_diskrun_figures worth having: a documentation gate that "
    "passes proves nothing until something has been shown to make it fail. It "
    "applies six mutants to COPIES of the spec and of src/vmsscs/scsd.c -- a "
    "drifted lead figure, a control arm that stops recording its zero, the "
    "refuted 'the peer never sends it' claim revived, the correction deleted, "
    "the open rejoin case (vms-449) dropped, and the lead band removed from the "
    "daemon header -- and requires the gate to red for every one.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
