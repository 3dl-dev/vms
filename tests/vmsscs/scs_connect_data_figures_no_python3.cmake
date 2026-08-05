# tests/vmsscs/scs_connect_data_figures_no_python3.cmake  --  vms-fdd
#
# The BODY of the scs_connect_data_figures ctest when CMake could not find a
# Python3 interpreter at configure time. Run as `cmake -P` (CMake is by
# definition present -- it is what invoked us), and it exits non-zero, so the
# test REDS rather than vanishing. Same shape and same reasoning as
# scs_credit_figures_no_python3.cmake (vms-76e): a silent else would drop the
# gate while leaving the suite green, and a skipped test is a failing test
# (CLAUDE.md / OS rule 7); a configure-time FATAL_ERROR would instead turn red
# five CI jobs that never run a vmsscs test at all.
message(FATAL_ERROR
    "scs_connect_data_figures (vms-fdd) cannot run: no Python3 interpreter was "
    "found when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_connect_data_figures.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is the only thing in ctest that pins the SCA CONNECT DATA verdict in "
    "src/vmsscs/include/scs_connect.h and docs/cluster-protocol-spec.md section "
    "4(n) to the checked-in measurement in tools/scs_connect_data_measure.py, "
    "and that verdict is what justifies the 16 bytes OVMX puts on the wire as a "
    "VMS version claim a peer may reject on (VAXcluster Principles p. 2-25).\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
