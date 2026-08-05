# tests/vmsscs/scs_diskrun_figures_no_python3.cmake  --  vms-ebb
#
# The BODY of the scs_diskrun_figures ctest when CMake could not find a Python3
# interpreter at configure time. Run as `cmake -P` and exits non-zero, so the
# test REDS rather than vanishing (a skipped test is a failing test, OS rule 7).
# Same shape and reasoning as scs_disc_figures_no_python3.cmake.
message(FATAL_ERROR
    "scs_diskrun_figures (vms-ebb) cannot run: no Python3 interpreter was found "
    "when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_diskrun_figures.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is the only thing in ctest that pins the disk-discovery trigger ruling -- "
    "spec sec 4(O.2), the lab-2 bracket that decided disk discovery keeps ONE "
    "trigger -- to the checked-in EXPECTED table in "
    "tools/cluster/scs_diskrun_trigger_measure.py, and that keeps the REFUTED "
    "claim out: the peer DOES send the DISCONNECT_REQ an immediate trigger "
    "would fire on, twice per run in 3 of 3 arms, and the first draft of the "
    "ruling said it never does. Without this gate that correction can be "
    "silently undone.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
