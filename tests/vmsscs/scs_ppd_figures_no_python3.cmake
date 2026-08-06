# tests/vmsscs/scs_ppd_figures_no_python3.cmake  --  vms-0fe
#
# The BODY of the scs_ppd_figures ctest when CMake could not find a Python3
# interpreter at configure time. Run as `cmake -P` and exits non-zero, so the
# test REDS rather than vanishing (a skipped test is a failing test, OS rule 7).
# Same shape and reasoning as scs_disc_figures_no_python3.cmake.
message(FATAL_ERROR
    "scs_ppd_figures (vms-0fe) cannot run: no Python3 interpreter was found "
    "when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_ppd_figures.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is the only thing in ctest that (a) pins the port-layer decode of the "
    "'%PEA0, Inappropriate SCA Control Message' console line -- the PPD field "
    "names, SCS$S_PPD = 16, the SCS message-type enum ending at APPL_DG 11, and "
    "the PPD$B_PORT remote-node-number grounding -- one copy per figure, and "
    "(b) pins the NEGATIVE reproduction result plus its matched "
    "OVMX_NO_CLEAN_SHUTDOWN=1 control, and (c) keeps the two REFUTED readings "
    "of that console line ('the VAX is RECEIVING it and REFUSING it', 'a real "
    "VAX ANSWERS NO DISCONNECT_REQ OVMX SENDS') quarantined. Both were carried "
    "as live evidence for two items before they were measured away; without "
    "this gate they come straight back.\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
