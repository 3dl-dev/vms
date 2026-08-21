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
    # vms-ed4: run_dcl_tests.sh loops ~20 DCL scripts internally, several of
    # which (INSTALL/SYSMAN) write the real /vms/SYS0/SYSCOMMON SYSDISK shared
    # with install_utility -> same lock so ctest --parallel serializes the two.
    RESOURCE_LOCK "sysdisk"
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

# Hermetic unit test for the interactive prompt/echo race (vms-195). Compiles
# dcl_mbx.c with its test hooks (-DDCL_MBX_TEST_HOOKS) and drives the writer-
# thread + synchronous-prompt drain barrier against a plain-pipe stand-in for
# the mailbox far end -- so the ordering guarantee is proven with no /dev/vms.
add_executable(test_dcl_prompt_echo_race
    test_prompt_echo_race.c
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/dcl_mbx.c
)
target_include_directories(test_dcl_prompt_echo_race PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/include
    ${VMS_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/src/libvmssys
)
target_compile_definitions(test_dcl_prompt_echo_race PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
    DCL_MBX_TEST_HOOKS
)
target_link_libraries(test_dcl_prompt_echo_race PRIVATE vms vmssys pthread)
add_test(NAME dcl-prompt-echo-race COMMAND test_dcl_prompt_echo_race)
set_tests_properties(dcl-prompt-echo-race PROPERTIES
    TIMEOUT 60
    LABELS "dcl;mbx;unit"
)

