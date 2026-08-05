# tests/vmsscs/scs_disc_figures_no_python3.cmake  --  vms-591
#
# The BODY of the scs_disc_figures ctest when CMake could not find a Python3
# interpreter at configure time. Run as `cmake -P` and exits non-zero, so the
# test REDS rather than vanishing (a skipped test is a failing test, OS rule 7).
# Same shape and reasoning as scs_reason_figures_no_python3.cmake.
message(FATAL_ERROR
    "scs_disc_figures (vms-591) cannot run: no Python3 interpreter was found "
    "when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_disc_figures.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is the only thing in ctest that (a) pins the DISCONNECT census, the "
    "REQ->RSP latency figures that justify the shutdown timeout, and the "
    "[60:62] matching-flag partition to the checked-in EXPECTED table in "
    "tools/cluster/scs_disc_measure.py, and (b) keeps the REFUTED spec claims "
    "-- '5 and 7 DO NOT EXIST ON OUR WIRE' and 'Do not build a 5 or 7 frame' -- "
    "from being re-asserted. Both message types DO exist on our wire; every "
    "census had been restricted to SCA length classes {62, 66, 110} and the two "
    "response classes are 58 bytes. Without this gate that correction can be "
    "silently undone.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
