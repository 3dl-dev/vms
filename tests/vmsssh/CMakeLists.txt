# tests/vmsssh/CMakeLists.txt
#
# vms-97d: the Docker-based SSH integration test (test_vmsssh.sh /
# vmsssh_integration) and its libssh test-client harness (test_ssh_client.c)
# are DELETED, not skipped. The VMS SSH server was ruled a show horse
# (vms-02d, cancelled 2026-07-30) and its only test exercised the dead
# Docker runtime (CLAUDE.md Rule 9). A skipping test reads as coverage it
# does not provide (CLAUDE.md Rule 10) — worse than no test at all.
#
# test_term_mapping is kept: it is a genuine unit test of TERM -> device
# type mapping with no libssh dependency, unrelated to the deleted
# integration test, and terminal fidelity remains a live Phase 3 concern
# (vms-d0b). Re-dispatch fix (veracity CONFIRMED, 2026-07-30): the test
# used to #include a hand-maintained COPY of the product function, which
# meant a drift in the real vmssshd.c left the test green — the exact
# mock-away-the-thing-under-test defect Rule 10/CLAUDE.md guards against.
# It now compiles and links the REAL product translation unit
# (src/vmsssh/term_map.c), which is dependency-free and lives outside the
# libssh gate on src/vmsssh so it, and this test, always build. The
# mapping itself is labelled an OVMX design choice (not VMS-authentic) in
# term_map.h/.c per Rule 8/Rule 10 — no VMS-fidelity claim is made or
# tested here.
add_executable(test_term_mapping
    test_term_mapping.c
    ${CMAKE_SOURCE_DIR}/src/vmsssh/term_map.c
)
target_include_directories(test_term_mapping PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsssh
)
add_test(NAME term_mapping
    COMMAND test_term_mapping
)
set_tests_properties(term_mapping PROPERTIES
    LABELS "unit;ssh"
    TIMEOUT 10
)

# vms-49e (Tier-0 security): the SSH session credential drop. Like
# test_term_mapping above, this links the REAL product translation unit
# (src/vmsssh/cred_drop.c) directly -- it is dependency-free (no libssh), so it
# builds and runs even where the vmssshd daemon does not. The test injects mock
# syscalls to assert the security-critical properties (UIC->uid/gid derivation,
# setgid-before-setuid, fail-closed on any error) WITHOUT the test process
# dropping its own credentials.
add_executable(test_cred_drop
    test_cred_drop.c
    ${CMAKE_SOURCE_DIR}/src/vmsssh/cred_drop.c
)
target_include_directories(test_cred_drop PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsssh
)
add_test(NAME ssh_cred_drop
    COMMAND test_cred_drop
)
set_tests_properties(ssh_cred_drop PROPERTIES
    LABELS "unit;ssh;security"
    TIMEOUT 10
)
