# DCL Integration Tests
# Run all test_*.sh files through the test harness

# Main DCL test suite (runs all test_*.sh via harness), sharded so ctest -j runs
# the slices in parallel (vms-0944: one serial loop of ~200 scripts took 60s+
# under -j8 contention and hit its timeout). run_dcl_tests.sh partitions the
# scripts by DCL_TEST_SHARD: "sysdisk" = those touching the shared real SYSDISK,
# "K/N" = the rest, round-robin. Every script is in exactly one shard.
set(DCL_ENV "VMSDCL=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/DCL.EXE;PATH=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}:$ENV{PATH};OVMX_HELPLIB=${CMAKE_SOURCE_DIR}/distro/rootfs/vms/SYS0/SYSCOMMON/SYSHLP/HELPLIB.HLP")
set(DCL_SHARDS 8)
math(EXPR DCL_LAST_SHARD "${DCL_SHARDS} - 1")
foreach(k RANGE ${DCL_LAST_SHARD})
    add_test(
        NAME dcl-integration-${k}
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_dcl_tests.sh
    )
    set_tests_properties(dcl-integration-${k} PROPERTIES
        ENVIRONMENT "${DCL_ENV};DCL_TEST_SHARD=${k}/${DCL_SHARDS}"
        TIMEOUT 120
        LABELS "dcl;integration"
    )
endforeach()
add_test(
    NAME dcl-integration-sysdisk
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_dcl_tests.sh
)
set_tests_properties(dcl-integration-sysdisk PROPERTIES
    ENVIRONMENT "${DCL_ENV};DCL_TEST_SHARD=sysdisk"
    TIMEOUT 120
    LABELS "dcl;integration"
    # vms-ed4: these scripts (INSTALL/SYSMAN) write the real /vms/SYS0/SYSCOMMON
    # SYSDISK shared with install_utility -> same lock so ctest --parallel
    # serializes the two.
    RESOURCE_LOCK "sysdisk"
)

# Unit test for the hierarchical HELP engine (vms-01b). Compiles the engine
# directly and asserts the topic-tree navigation against library data -- no DCL
# binary. The engine reads library files only through RMS (dcl_help_acp.c,
# libc-drop vms-3381), so the test links the RMS stack the images do.
add_executable(test_help_engine
    test_help_engine.c
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/dcl_help.c
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/dcl_help_acp.c
)
target_include_directories(test_help_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/include
    ${VMS_INCLUDE_DIR}
    ${VMS_RMS_INCLUDE_DIR}
)
target_link_libraries(test_help_engine PRIVATE vms vmsrms vmsfs vmslnm vmsprocess pthread)
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

# DCL.EXE parses no "--" argv flags: LOGINOUT leaves its login data in the
# executive (VMS_IOCTL_LGI) and DCL reads it (rd vms-838b). Self-proving.
add_test(
    NAME dcl-no-argv-flags
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_dcl_no_argv_flags.sh ${CMAKE_SOURCE_DIR}
)
set_tests_properties(dcl-no-argv-flags PROPERTIES
    TIMEOUT 60
    LABELS "dcl;gate"
)

# Hermetic test for the /PAGE capture of DCL's output sink (rd vms-07ae): the
# pager gathers SYS$OUTPUT's records in the sink, never through fd 1.
add_executable(test_out_capture
    test_out_capture.c
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/dcl_out.c
)
target_include_directories(test_out_capture PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/include
    ${VMS_INCLUDE_DIR}
)
add_test(NAME dcl-out-capture COMMAND test_out_capture)
set_tests_properties(dcl-out-capture PROPERTIES
    TIMEOUT 60
    LABELS "dcl;unit"
)
