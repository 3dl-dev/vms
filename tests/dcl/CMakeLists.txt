# DCL Integration Tests
# Run all test_*.sh files through the test harness

# Main DCL test suite (runs all test_*.sh via harness)
add_test(
    NAME dcl-integration
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_dcl_tests.sh
)
set_tests_properties(dcl-integration PROPERTIES
    ENVIRONMENT "VMSDCL=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/DCL.EXE;PATH=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}:$ENV{PATH}"
    TIMEOUT 120
    LABELS "dcl;integration"
)

# Self-test of the harness above (vms-6a7). run_dcl_tests.sh once evaluated
# dash-leading needles as grep OPTIONS, which made every EXPECT_NOT whose
# needle starts with a dash pass unconditionally. No product test can detect
# that — they all pass either way — so the harness is driven against synthetic
# cases with known verdicts here.
add_test(
    NAME dcl-harness-selftest
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/selftest_harness.sh
)
set_tests_properties(dcl-harness-selftest PROPERTIES
    TIMEOUT 120
    LABELS "dcl;harness"
)

