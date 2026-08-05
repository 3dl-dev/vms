# tests/vmsscs/scs_reason_figures_no_python3.cmake  --  vms-6b3
#
# This is the BODY of the scs_reason_figures ctest when CMake could not find a
# Python3 interpreter at configure time. It is run as `cmake -P` (CMake is by
# definition present -- it is what invoked us), and it exits non-zero, so the
# test REDS.
#
# Same shape, and the same reasoning, as scs_credit_figures_no_python3.cmake:
# a silent else drops the only gate that pins the reason-code census to its
# measurement and the suite stays green while it is gone (a skipped test is a
# failing test, OS rule 7), while message(FATAL_ERROR) at CONFIGURE time is
# over-reach -- tests/qemu/Dockerfile configures this whole tree on a musl
# builder image with no python3, builds two targets and never invokes ctest.
message(FATAL_ERROR
    "scs_reason_figures (vms-6b3) cannot run: no Python3 interpreter was found "
    "when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_reason_figures.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is the only thing in ctest that checks the REJECT/DISCONNECT reason-code "
    "census in src/vmsscs/include/scs_reason.h and "
    "docs/cluster-protocol-spec.md against the checked-in EXPECTED table in "
    "tools/cluster/scs_reason_measure.py. Without it, a wrong recorded "
    "measurement is invisible to a green run -- which is exactly how a refuted "
    "'the only slot zero in 100% of observed frames' rationale, and a spec "
    "paragraph contradicted by the tool's own output, both shipped.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
