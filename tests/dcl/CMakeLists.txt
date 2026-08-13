# DCL Integration Tests
# Run all test_*.sh files through the test harness

# Main DCL test suite (runs all test_*.sh via harness)
add_test(
    NAME dcl-integration
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_dcl_tests.sh
)
set_tests_properties(dcl-integration PROPERTIES
    ENVIRONMENT "VMSDCL=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/DCL.EXE;PATH=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}:$ENV{PATH};OVMX_HELPLIB=${CMAKE_SOURCE_DIR}/distro/rootfs/vms/SYS0/SYSCOMMON/SYSHLP/HELPLIB.HLP"
    TIMEOUT 120
    LABELS "dcl;integration"
)

# Hermetic unit test for the hierarchical HELP engine (vms-01b). Compiles the
# reader source directly and asserts the topic-tree navigation against library
# data -- no DCL binary, no /vms, no on-disk library required.
add_executable(test_help_engine
    test_help_engine.c
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/dcl_help.c
)
target_include_directories(test_help_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/include
    ${VMS_INCLUDE_DIR}
)
target_compile_definitions(test_help_engine PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)
add_test(NAME dcl-help-engine COMMAND test_help_engine)
set_tests_properties(dcl-help-engine PROPERTIES
    TIMEOUT 60
    LABELS "dcl;help;unit"
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

