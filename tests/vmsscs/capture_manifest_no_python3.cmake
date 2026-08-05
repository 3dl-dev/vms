# tests/vmsscs/capture_manifest_no_python3.cmake  --  vms-beb
#
# The BODY of the capture_manifest ctest when CMake could not find a Python3
# interpreter at configure time. Run as `cmake -P` (CMake is by definition
# present -- it is what invoked us), and it exits non-zero, so the test REDS
# rather than vanishing. Same shape and same reasoning as
# scs_connect_data_figures_no_python3.cmake (vms-fdd): a silent else would
# drop the gate while leaving the suite green, and a skipped test is a
# failing test (CLAUDE.md / OS rule 7).
message(FATAL_ERROR
    "capture_manifest (vms-beb) cannot run: no Python3 interpreter was found "
    "when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_capture_manifest.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. "
    "It is what proves tools/cluster/capture_manifest.py refuses a lab-2 "
    "capture deposited into a lab-1 census (and vice versa) and that every "
    "one of the five measurement tools vms-beb names actually calls into it.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it "
    "up; delete CMakeCache.txt or re-configure from scratch).")
