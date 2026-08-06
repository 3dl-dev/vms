# tests/vmsscs/scs_send_sites_no_python3.cmake  --  vms-9c6
#
# This is the BODY of the scs_send_sites ctest when CMake could not find a
# Python3 interpreter at configure time. It is run as `cmake -P` (CMake is by
# definition present -- it is what invoked us), and it exits non-zero, so the
# test REDS.
#
# Same shape as scs_credit_figures_no_python3.cmake next to it, for the same
# reason: a silent else here would drop the SEND SITE CENSUS gate (the source
# scan that proves every send_frame_raw() caller is either send_frame_vc() or
# an explicitly-named exemption) while leaving the suite green, and a skipped
# test is a failing test (CLAUDE.md / OS rule 7). Configure-time
# FATAL_ERROR is wrong too (see the comment above this block's registration in
# CMakeLists.txt): it would turn CI jobs red that configure this tree but never
# run ctest at all. So: configure always succeeds, the test is always
# registered under its real name and labels, and only a build that actually
# runs ctest without a Python3 interpreter sees this RED test.
message(FATAL_ERROR
    "scs_send_sites (vms-9c6) cannot run: no Python3 interpreter was found "
    "when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scsd_send_sites.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is the SEND SITE CENSUS: the only gate that proves every direct caller of "
    "send_frame_raw() in src/vmsscs/scsd.c is either send_frame_vc() (the p. "
    "2-31 choke point) or a name explicitly exempted in the census. Without it, "
    "a new sender that bypasses the choke point is invisible to a green run.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
