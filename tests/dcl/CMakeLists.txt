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

