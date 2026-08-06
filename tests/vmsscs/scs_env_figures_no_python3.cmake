# tests/vmsscs/scs_env_figures_no_python3.cmake  --  vms-ec7
#
# The BODY of the scs_env_figures ctest when CMake could not find a Python3
# interpreter at configure time. Run as `cmake -P` and exits non-zero, so the
# test REDS rather than vanishing (a skipped test is a failing test, OS rule 7).
# Same shape and reasoning as scs_disc_figures_no_python3.cmake.
message(FATAL_ERROR
    "scs_env_figures (vms-ec7) cannot run: no Python3 interpreter was found "
    "when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_env_figures.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is the only thing in ctest that (a) pins every offset and shape claim in "
    "src/vmsscs/include/scs_env.h to the checked-in EXPECTED table in "
    "tools/cluster/scs_env_measure.py, and, on a host with the lab captures, "
    "pins that table to the packets -- including the BUILD ROUND TRIP over all "
    "319,575 envelope-conformant frames in the corpus; and (b) enforces the "
    "structural claim the whole item rests on: the six envelope offsets are "
    "defined ONCE, scs_rx.h aliases them rather than restating them, no builder "
    "writes an envelope field by open-coded offset, and the non-envelope "
    "classes (the 106-content START in scs_start.c, the 120-content HELLO in "
    "scs_hello.c) are still kept OUT of the shared builder. Without this gate "
    "the envelope can quietly go back to being six copies.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
