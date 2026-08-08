# tests/vmsscs/scs_mscp_cdt_hazard_bypasses_no_python3.cmake  --  vms-cf0
#
# This is the BODY of the scs_mscp_cdt_hazard_bypasses ctest when CMake could
# not find a Python3 interpreter at configure time. Run as `cmake -P` (CMake
# is by definition present -- it is what invoked us) and it exits non-zero,
# so the test REDS.
#
# Same shape as scs_mscp_cdt_hazard_no_python3.cmake next to it, for the same
# reason: a silent else here would drop the BYPASS BATTERY (the regression
# proof that the 3 vms-cf0 bypasses -- split-across-lines, typedef alias,
# non-adjacent LHS -- stay closed) while leaving the suite green, and a
# skipped test is a failing test (CLAUDE.md / OS rule 7). Configure-time
# FATAL_ERROR is wrong too, for the same CI-job reason documented next to
# that test's registration in CMakeLists.txt: configure always succeeds, the
# test is always registered under its real name and labels, and only a build
# that actually runs ctest without a Python3 interpreter sees this RED test.
message(FATAL_ERROR
    "scs_mscp_cdt_hazard_bypasses (vms-cf0) cannot run: no Python3 "
    "interpreter was found when this build tree was configured, so there is "
    "nothing to execute "
    "tests/vmsscs/test_scs_mscp_cdt_hazard_bypasses.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. "
    "It is the regression proof that the 3 demonstrated bypasses of "
    "scs_mscp_cdt_hazard (split-declaration/assignment across lines, a "
    "typedef alias of struct scs_mscp_params, and array-subscript / "
    "paren-dereference left-hand sides) stay closed. Without this gate, a "
    "future change could silently re-open any of the three.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it "
    "up; delete CMakeCache.txt or re-configure from scratch).")
