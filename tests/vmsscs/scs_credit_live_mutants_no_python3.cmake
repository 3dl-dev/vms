# tests/vmsscs/scs_credit_live_mutants_no_python3.cmake  --  vms-aa1
#
# BODY of the scs_credit_live_mutants ctest when CMake found no Python3
# interpreter at configure time. Run as `cmake -P`, exits non-zero, so the test
# REDS rather than vanishing (a skipped test is a failing test, OS rule 7).
message(FATAL_ERROR
    "scs_credit_live_mutants (vms-aa1) cannot run: no Python3 interpreter was "
    "found when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_credit_live_mutants.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is the MEASUREMENT behind the claim that test_scsd_wire's four "
    "test_credit_* cases would notice if the daemon stopped doing flow control. "
    "vms-76e/vms-1d2 shipped a fully green credit account that NOTHING CALLED; "
    "green said nothing, because the tests called the account themselves. This "
    "battery breaks each live behaviour in scsd.c in turn and requires the C "
    "test to go red -- and it already caught two assertions of its own that "
    "could not.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
