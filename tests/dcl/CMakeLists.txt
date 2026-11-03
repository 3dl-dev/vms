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

# Hermetic unit test for the command parser's parameter loop (rd vms-a18): a
# '@' in a PARAMETER position -- `SPAWN @PROC.COM` -- must reach the parameter
# it belongs to instead of being dropped. Compiles the parser + lexer sources
# directly and calls dcl_parse_line(), the same shape as test_help_engine
# above: no DCL binary, no subprocess, no executive.
add_executable(test_parse_at_param
    test_parse_at_param.c
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/dcl_parser.c
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/dcl_lexer.c
    # str_upcase_copy (the parser upcases every verb through it) -- the real
    # STR$ routine, compiled in, not a stub: a stub could disagree with the
    # product's own upcasing and make this test assert something DCL does not do.
    ${CMAKE_SOURCE_DIR}/src/libvms/rtl/str_util.c
)
target_include_directories(test_parse_at_param PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/include
    ${VMS_INCLUDE_DIR}
)
target_compile_definitions(test_parse_at_param PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)
add_test(NAME dcl-parse-at-param COMMAND test_parse_at_param)
set_tests_properties(dcl-parse-at-param PROPERTIES
    TIMEOUT 60
    LABELS "dcl;parser;unit"
)

# A comma list longer than P1-P8 keeps every element (rd vms-465: GCC's ar
# hands LIBRARY a whole batch of objects in one command). Hermetic, the same
# shape as test_parse_at_param.
add_executable(test_parse_list_past_p8
    test_parse_list_past_p8.c
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/dcl_parser.c
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/dcl_lexer.c
    ${CMAKE_SOURCE_DIR}/src/libvms/rtl/str_util.c
)
target_include_directories(test_parse_list_past_p8 PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/include
    ${VMS_INCLUDE_DIR}
)
target_compile_definitions(test_parse_list_past_p8 PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)
add_test(NAME dcl-parse-list-past-p8 COMMAND test_parse_list_past_p8)
set_tests_properties(dcl-parse-list-past-p8 PROPERTIES
    TIMEOUT 60
    LABELS "dcl;parser;unit"
)

# A comma inside '[...]' is part of the word, so a UIC is one DCL value
# (rd vms-1fb7): SET FILE/OWNER_UIC=[144,144], SET UIC [1,4]. Hermetic, the
# same shape as test_parse_at_param.
add_executable(test_parse_uic_bracket
    test_parse_uic_bracket.c
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/dcl_parser.c
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/dcl_lexer.c
    ${CMAKE_SOURCE_DIR}/src/libvms/rtl/str_util.c
)
target_include_directories(test_parse_uic_bracket PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/include
    ${VMS_INCLUDE_DIR}
)
target_compile_definitions(test_parse_uic_bracket PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)
add_test(NAME dcl-parse-uic-bracket COMMAND test_parse_uic_bracket)
set_tests_properties(dcl-parse-uic-bracket PROPERTIES
    TIMEOUT 60
    LABELS "dcl;parser;unit"
)

# DCL parameters under the file-name parse style (rd vms-0b0): a '^' escape is
# part of a filespec under PARSE_STYLE=EXTENDED and refused (PARMDEL) under
# TRADITIONAL, as lab-Alpha V8.4 does (tests/lab/captures/ods5-alpha-20261022).
add_executable(test_parse_ext_names
    test_parse_ext_names.c
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/dcl_parser.c
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/dcl_lexer.c
    ${CMAKE_SOURCE_DIR}/src/libvms/rtl/str_util.c
)
target_include_directories(test_parse_ext_names PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/include
    ${VMS_INCLUDE_DIR}
)
target_compile_definitions(test_parse_ext_names PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)
add_test(NAME dcl-parse-ext-names COMMAND test_parse_ext_names)
set_tests_properties(dcl-parse-ext-names PROPERTIES
    TIMEOUT 60
    LABELS "dcl;parser;unit"
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
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/dcl_out.c
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/dcl_in.c
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


# Hermetic test for DCL's in-memory command-procedure stream (rd vms-97c2): the
# @-procedure staging must not need the filesystem to accept writes.
add_executable(test_memfile
    test_memfile.c
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/dcl_memfile.c
)
target_include_directories(test_memfile PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/include
    ${VMS_INCLUDE_DIR}
)
add_test(NAME dcl-memfile COMMAND test_memfile)
set_tests_properties(dcl-memfile PROPERTIES
    TIMEOUT 60
    LABELS "dcl;unit"
)

# SHOW DEVICE <prefix> matches past a node / allocation-class prefix, so a
# cluster's SHOW DEVICE D lists MSCP-served $2$DUA2: / VAX1$DUA2: (rd vms-582).
add_executable(test_show_device_prefix test_show_device_prefix.c)
target_include_directories(test_show_device_prefix PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/include
)
add_test(NAME dcl-show-device-prefix COMMAND test_show_device_prefix)
set_tests_properties(dcl-show-device-prefix PROPERTIES
    TIMEOUT 60
    LABELS "dcl;unit"
)

# The host harness's DCL output route (rd vms-a6ce): run_dcl_tests.sh gives each
# test dcl_rms_run as $VMSDCL, which starts DCL with SYS$OUTPUT/SYS$ERROR
# assigned to an RMS file and reads the records back with rms_read_records --
# RMS $OPEN/$GET (src/vmsrms), never stdio parsing of the file. Both live next
# to DCL.EXE so a test's `dirname $VMSDCL` still finds its sibling images.
add_executable(rms_read_records rms_read_records.c)
target_include_directories(rms_read_records PRIVATE
    ${VMS_RMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)
target_link_libraries(rms_read_records PRIVATE vmsrms vmsfs vmslnm pthread)
add_custom_command(
    OUTPUT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/dcl_rms_run
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/dcl_rms_run.sh
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/dcl_rms_run
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dcl_rms_run.sh
)
add_custom_target(dcl_rms_run ALL
    DEPENDS ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/dcl_rms_run rms_read_records)

# Negative control for that route (rd vms-a6ce): a sandbox DCL whose file
# SYS$OUTPUT is never bound to RMS must turn ported tests RED.
add_test(
    NAME dcl-rms-route-negctl
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/negctl_rms_route.sh ${CMAKE_SOURCE_DIR}
)
set_tests_properties(dcl-rms-route-negctl PROPERTIES
    ENVIRONMENT "DCL_RMS_RUN=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/dcl_rms_run"
    TIMEOUT 300
    LABELS "dcl;harness;negctl"
)
