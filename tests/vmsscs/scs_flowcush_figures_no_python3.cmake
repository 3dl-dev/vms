# tests/vmsscs/scs_flowcush_figures_no_python3.cmake  --  vms-f03
#
# BODY of the scs_flowcush_figures ctest when CMake found no Python3 interpreter
# at configure time. Run as `cmake -P`, exits non-zero, so the test REDS rather
# than silently vanishing. Same shape and same reasoning as
# scs_credit_figures_no_python3.cmake -- read that file for the full rationale
# (a silent else drops the gate; a configure-time FATAL_ERROR reds five CI jobs
# that never run a vmsscs test).
message(FATAL_ERROR
    "scs_flowcush_figures (vms-f03) cannot run: no Python3 interpreter was found "
    "when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_flowcush_figures.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is the only thing in ctest that holds docs/cluster-protocol-spec.md "
    "section 4(h)(1g) and docs/design-mscp-direction.md section 1.3 to the "
    "checked-in measurement in tools/scs_flowcush_measure.py -- the SCSFLOWCUSH "
    "dose-response that identifies SCA message type 8 as the special credit "
    "message. It also pins the three claims that keep that identification "
    "honest: the zero-type-8 negative control, the untested handle swap, and "
    "the refusal to name type 9. On a host with the lab-2 captures it "
    "re-derives all 15 figures from the packets.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
