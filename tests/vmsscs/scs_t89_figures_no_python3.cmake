# tests/vmsscs/scs_t89_figures_no_python3.cmake  --  vms-a58
#
# The BODY of the scs_t89_figures ctest when CMake could not find a Python3
# interpreter at configure time. Run as `cmake -P` and exits non-zero, so the
# test REDS rather than vanishing (a skipped test is a failing test, OS rule 7).
# Same shape and reasoning as scs_disc_figures_no_python3.cmake.
message(FATAL_ERROR
    "scs_t89_figures (vms-a58) cannot run: no Python3 interpreter was found "
    "when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_t89_figures.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is the only thing in ctest that (a) pins the length-unrestricted, "
    "OUI-split census of SCS message types 8 and 9 -- and the credit-ledger "
    "measurement that showed their constant 1 is a COUNT and not a version or "
    "a flag -- to the checked-in EXPECTED table in "
    "tools/cluster/scs_t89_measure.py and to docs/cluster-protocol-spec.md "
    "sec 4(h)(1e); (b) keeps the four ELIMINATED readings of types 8 and 9 "
    "from being written back down as identifications (they are NOT the "
    "p. 2-44 special credit message and they are NOT connection-control "
    "messages); and (c) keeps the emission ruling alive at the one place OVMX "
    "touches these frames, scs_reflect_credit() in src/vmsscs/scsd.c -- "
    "including the part OVMX does NOT yet honour, emitting a type 8 before a "
    "DISCONNECT_REQ it initiates.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
