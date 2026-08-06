# tests/vmsscs/scs_449_bracket_shape_no_python3.cmake  --  vms-4847
#
# The BODY of the scs_449_bracket_shape ctest when CMake found no Python3
# interpreter at configure time. Run as `cmake -P`, it exits non-zero so the
# test REDS rather than silently vanishing. Same shape, and the same
# reasoning, as scs_credit_figures_no_python3.cmake -- read that file for why
# this is not an `if()` with a silent else and not a configure-time
# FATAL_ERROR.
#
# WHY THIS FILE APPEARS WITH vms-4847 AND NOT WITH THE GATE IT BACKS: the
# else() branch in tests/vmsscs/CMakeLists.txt named this path from the day
# scs_449_bracket_shape was registered (vms-449), but the file was never
# written -- and because the repo's blanket `*.cmake` .gitignore rule
# swallows any hand-written shim that isn't individually un-ignored, this one
# failed LOUD instead of silently (a bare "cmake -P could not find the file"
# error naming neither the gate nor the fix) until vms-4847 audited every
# *_no_python3.cmake reference against git and fixed the .gitignore pattern
# itself, not just this one instance.
message(FATAL_ERROR
    "scs_449_bracket_shape cannot run: no Python3 interpreter was found when "
    "this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_449_bracket_shape.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is THE MUTATION BATTERY FOR THE REJOIN BRACKET'S SHAPE (vms-449): it "
    "applies each mutant of check_449_bracket_shape() and "
    "check_449r_bracket_shape() alone and requires the check to RED, "
    "restoring and re-verifying between each. Needs no captures, no lab and "
    "no network -- it runs everywhere including CI.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
