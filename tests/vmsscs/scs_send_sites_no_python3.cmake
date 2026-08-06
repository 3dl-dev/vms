# tests/vmsscs/scs_send_sites_no_python3.cmake  --  vms-4847
#
# The BODY of the scs_send_sites ctest when CMake found no Python3 interpreter
# at configure time. Run as `cmake -P`, it exits non-zero so the test REDS
# rather than silently vanishing. Same shape, and the same reasoning, as
# scs_credit_figures_no_python3.cmake -- read that file for why this is not an
# `if()` with a silent else and not a configure-time FATAL_ERROR.
#
# WHY THIS FILE APPEARS WITH vms-4847 AND NOT WITH THE GATE IT BACKS: the
# else() branch in tests/vmsscs/CMakeLists.txt named this path from the day
# scs_send_sites was registered (vms-abc), but the file was never written --
# and because the repo's blanket `*.cmake` .gitignore rule swallows any
# hand-written shim that isn't individually un-ignored, this went unnoticed
# until vms-4847 audited every *_no_python3.cmake reference against git.
message(FATAL_ERROR
    "scs_send_sites cannot run: no Python3 interpreter was found when this "
    "build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scsd_send_sites.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is THE SEND SITE CENSUS: it asserts every direct caller of the transport "
    "(send_frame_raw) in src/vmsscs/scsd.c is either named EXEMPT or goes "
    "through the send_frame_vc() choke point where flow control is enforced. "
    "It is a source gate specifically because the runtime test can only prove "
    "the send paths its captured frames happen to drive -- this one cannot be "
    "outflanked by adding a path; adding one reds it.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
