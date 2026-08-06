# tests/vmsscs/scs_mscp_srv_mutants_no_python3.cmake  --  vms-291
#
# The BODY of the scs_mscp_srv_mutants ctest when CMake could not find a Python3
# interpreter at configure time. Run as `cmake -P` and exits non-zero, so the
# test REDS rather than vanishing (a skipped test is a failing test, OS rule 7).
message(FATAL_ERROR
    "scs_mscp_srv_mutants (vms-291) cannot run: no Python3 interpreter was found "
    "when this build tree was configured, so there is nothing to execute "
    "tests/vmsscs/test_scs_mscp_srv_mutants.py with.\n"
    "This test is NOT optional and is deliberately RED rather than skipped. It "
    "is what makes vmsscs_mscp_srv_unit worth having. Three of that test's "
    "end-message lengths were WRONG AND GREEN before the vms-291 serving "
    "capture -- GUS built at Table A-7's 48 where a real VAX emits 52, and "
    "WRITE assumed equal to READ's 32 where a real server declares 36 -- "
    "because a builder checked against the constant that built it is "
    "self-consistent at any constant. The battery mutates one behaviour of "
    "src/vmsscs/scs_mscp_srv.{c,h} at a time, rebuilds, and requires the C test "
    "to go NON-ZERO for every one. Its two load-bearing arms are the INV-6 "
    "boundary (a READ with no transfer hook must report Controller Error, never "
    "a Success for data that never moved -- rd vms-941) and the sec 3.4 "
    "Controller-Online gate (without which a stray frame on a half-open "
    "connection can mount a volume).\n"
    "FIX: install a Python 3 interpreter and re-run cmake on this build tree "
    "(the find_package result is cached, so a bare rebuild will not pick it up; "
    "delete CMakeCache.txt or re-configure from scratch).")
