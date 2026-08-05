# tests/vmsscs/scs_join_capability_figures_no_python3.cmake  --  vms-371
#
# The BODY of the scs_join_capability_figures ctest when CMake found no Python3
# interpreter at configure time. Run as `cmake -P`, it exits non-zero so the
# test REDS rather than silently vanishing. Same shape, and the same reasoning,
# as scs_credit_figures_no_python3.cmake -- read that file for why this is not
# an `if()` with a silent else and not a configure-time FATAL_ERROR.
#
# WHY THIS FILE APPEARS WITH vms-371 AND NOT WITH THE GATE IT BACKS: the
# else() branch in tests/vmsscs/CMakeLists.txt named this path from the day
# scs_join_capability_figures was registered, but the file was never written.
# On a host without Python3 the test still failed -- with a bare "cmake -P
# could not find the file", which names neither the gate nor the fix. vms-371
# found it while auditing which figures gates can actually run.
message(FATAL_ERROR
    "scs_join_capability_figures cannot run: no Python3 interpreter was found "
    "when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_join_capability_figures.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is the ONLY gate on the vms-578 acceptance bracket -- the three runs that "
    "JOIN, which is the evidence the whole SCA layer rests on -- and on the "
    "vms-70e2 bracket beside it. Until vms-371 those figures were pinned by no "
    "gate at all; a build that quietly drops this one puts them back there.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
