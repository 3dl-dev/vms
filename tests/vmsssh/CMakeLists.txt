# tests/vmsssh/CMakeLists.txt
#
# vms-97d: the Docker-based SSH integration test (test_vmsssh.sh /
# vmsssh_integration) and its libssh test-client harness (test_ssh_client.c)
# are DELETED, not skipped. The VMS SSH server was ruled a show horse
# (vms-02d, cancelled 2026-07-30) and its only test exercised the dead
# Docker runtime (CLAUDE.md Rule 9). A skipping test reads as coverage it
# does not provide (CLAUDE.md Rule 10) — worse than no test at all.
#
# test_term_mapping is kept: it is a genuine unit test of TERM -> VMS
# device type mapping with no libssh dependency, unrelated to the deleted
# integration test, and terminal fidelity remains a live Phase 3 concern.

# ------------------------------------------------------------------ #
# test_term_mapping — unit test for TERM → VMS device type mapping    #
# ------------------------------------------------------------------ #
add_executable(test_term_mapping test_term_mapping.c)
add_test(NAME term_mapping
    COMMAND test_term_mapping
)
set_tests_properties(term_mapping PROPERTIES
    LABELS "unit;ssh"
    TIMEOUT 10
)
