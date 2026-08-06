# tests/vmsscs/census_guard_no_python3.cmake  --  vms-69c
#
# The BODY of the census_guard ctest when CMake could not find a Python3
# interpreter at configure time. Run as `cmake -P` (CMake is by definition
# present -- it is what invoked us), and it exits non-zero, so the test REDS
# rather than vanishing. Same shape and same reasoning as
# capture_manifest_no_python3.cmake (vms-beb): a silent else would drop the
# gate while leaving the suite green, and a skipped test is a failing test
# (CLAUDE.md / OS rule 7).
message(FATAL_ERROR
    "census_guard (vms-69c) cannot run: no Python3 interpreter was found "
    "when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_census_guard.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. "
    "It is what proves tools/cluster/census_guard.py refuses a census that "
    "silently narrows its own SCA length population (the vms-c11 "
    "under-sampling failure) and one that spans classes that do not share "
    "the SCA envelope (the vms-c11 over-generalising mirror), in both "
    "directions, and that scs_reason_measure.py, scs_disc_measure.py and "
    "scs_connect_data_measure.py actually call it.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it "
    "up; delete CMakeCache.txt or re-configure from scratch).")
