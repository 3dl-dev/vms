# tests/vmsscs/scs_type10_figures_no_python3.cmake  --  vms-4eb
#
# The BODY of the scs_type10_figures ctest when CMake could not find a Python3
# interpreter at configure time. Run as `cmake -P` and exits non-zero, so the
# test REDS rather than vanishing (a skipped test is a failing test, OS rule 7).
# Same shape and reasoning as scs_diskrun_figures_no_python3.cmake.
message(FATAL_ERROR
    "scs_type10_figures (vms-4eb) cannot run: no Python3 interpreter was found "
    "when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_type10_figures.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is the only thing in ctest that pins the SCA message-type-10 decode -- "
    "spec sec 4(h)(1e), the 110-content class identified as the MSCP GET UNIT "
    "STATUS end message, 2889 of 2889 frames paired to their command -- to the "
    "checked-in EXPECTED table in tools/cluster/scs_type10_measure.py, and it "
    "is what keeps that decode's two ADMITTED GAPS admitted: the four residue "
    "bytes at body[48:52] and the unit-flags bit Table A-5 does not define. "
    "This epic has already shipped one wire claim labelled grounded with no "
    "observation behind it, and it was wrong, not merely unmeasured. Without "
    "this gate either gap can be silently given a name.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
