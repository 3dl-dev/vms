# tests/vmsscs/scs_credit_figures_no_python3.cmake  --  vms-76e
#
# This is the BODY of the scs_credit_figures ctest when CMake could not find a
# Python3 interpreter at configure time. It is run as `cmake -P` (CMake is by
# definition present -- it is what invoked us), and it exits non-zero, so the
# test REDS.
#
# WHY THIS EXISTS RATHER THAN `if(Python3_Interpreter_FOUND)` WITH A SILENT ELSE:
#
#   * A silent else drops the only gate that pins the SCS credit WIRE VERDICT
#     prose to its measurement, and the suite stays green while it is gone. A
#     skipped test is a failing test (CLAUDE.md / OS rule 7).
#
#   * message(FATAL_ERROR) at CONFIGURE time was the first attempt and it is
#     wrong in the other direction: it turned five CI jobs red that never run a
#     vmsscs test at all. tests/qemu/Dockerfile configures the whole tree with
#     -DOVMX_STATIC=ON -DBUILD_TESTS=ON on a musl builder image that has no
#     python3, then builds only `qemu_syssvc_tests vmsdcl` and never invokes
#     ctest. Demanding an interpreter to CONFIGURE that is over-reach.
#
# The shape below satisfies both: configure always succeeds, the test is always
# registered under its real name and labels, a build that never runs ctest is
# unaffected, and any build that DOES run ctest gets a RED test naming the
# missing interpreter. The gate cannot silently disappear from the suite.
message(FATAL_ERROR
    "scs_credit_figures (vms-76e) cannot run: no Python3 interpreter was found "
    "when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_credit_figures.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is the only thing in ctest that re-derives the SCS credit WIRE VERDICT "
    "figures in src/vmsscs/include/scs_credit.h and docs/cluster-protocol-spec.md "
    "from the checked-in EXPECTED table in tools/scs_credit_measure.py. Without "
    "it, a wrong recorded measurement is invisible to a green run -- which is "
    "exactly how a mis-scoped filter claim survived two review rounds.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
