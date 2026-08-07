# tests/vmsscs/scs_mscp_cdt_hazard_no_python3.cmake  --  vms-73c
#
# This is the BODY of the scs_mscp_cdt_hazard ctest when CMake could not find a
# Python3 interpreter at configure time. Run as `cmake -P` (CMake is by
# definition present -- it is what invoked us) and it exits non-zero, so the
# test REDS.
#
# Same shape as scs_send_sites_no_python3.cmake next to it, for the same
# reason: a silent else here would drop the UNDISCLOSED DOUBLE-GRANT CENSUS
# (the source scan that proves no `.cdt =` assignment lands on a struct
# scs_mscp_params in src/vmsscs/scsd.c or src/vmsscs/scs_mscp_srv.c without a
# CREDIT-HAZARD-ACKNOWLEDGED comment) while leaving the suite green, and a
# skipped test is a failing test (CLAUDE.md / OS rule 7). Configure-time
# FATAL_ERROR is wrong too, for the same CI-job reason documented next to that
# test's registration in CMakeLists.txt: configure always succeeds, the test
# is always registered under its real name and labels, and only a build that
# actually runs ctest without a Python3 interpreter sees this RED test.
message(FATAL_ERROR
    "scs_mscp_cdt_hazard (vms-73c) cannot run: no Python3 interpreter was "
    "found when this build tree was configured, so there is nothing to "
    "execute tests/vmsscs/test_scs_mscp_cdt_hazard.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. "
    "It is the source-level negative control for the LATENT DOUBLE-GRANT "
    "HAZARD documented in src/vmsscs/scs_mscp.c: an undisclosed `.cdt =` wire-"
    "up on a struct scs_mscp_params would silently make that hazard live. "
    "Without this gate, that wiring is invisible to a green run.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it "
    "up; delete CMakeCache.txt or re-configure from scratch).")
