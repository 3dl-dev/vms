# tests/vmsscs - Unit tests for vmsscs (SCS datalink length classifier)

add_executable(test_scs_classify test_scs_classify.c)

target_include_directories(test_scs_classify PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_classify PRIVATE vmsscs_classify)

add_test(NAME vmsscs_classify_unit COMMAND test_scs_classify)
set_tests_properties(vmsscs_classify_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-b62 - HELLO builder byte-layout test.
add_executable(test_scs_hello test_scs_hello.c)

target_include_directories(test_scs_hello PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_hello PRIVATE vmsscs_hello)

add_test(NAME vmsscs_hello_unit COMMAND test_scs_hello)
set_tests_properties(vmsscs_hello_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-9f3 - padded directed-HELLO builder (NISCA channel packet-size
# verification, spec sec 4k): proves the padded frame is a genuine directed
# HELLO with only the SCA length field changed + a pure-zero pad tail, and that
# smaller probes are byte-identical prefixes of the 1500-byte frame.
add_executable(test_scs_hello_padded test_scs_hello_padded.c)

target_include_directories(test_scs_hello_padded PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_hello_padded PRIVATE vmsscs_hello)

add_test(NAME vmsscs_hello_padded_unit COMMAND test_scs_hello_padded)
set_tests_properties(vmsscs_hello_padded_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-d94 - directed-HELLO abs-30 channel-verify per-frame word (spec sec 4a
# offset-30): proves the builder can reach b4 (not just the fixed b3 vms-5fe
# held), the response rule b2->b3/b3->b4/b4->b3, and that changing the word
# changes ONLY abs30 (keep-everything-else invariant).
add_executable(test_scs_hello_pfw test_scs_hello_pfw.c)

target_include_directories(test_scs_hello_pfw PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_hello_pfw PRIVATE vmsscs_hello)

add_test(NAME vmsscs_hello_pfw_unit COMMAND test_scs_hello_pfw)
set_tests_properties(vmsscs_hello_pfw_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-5fe - directed HELLO + VMS$VAXcluster SCS connect handshake test.
add_executable(test_scs_connect test_scs_connect.c)

target_include_directories(test_scs_connect PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_connect PRIVATE vmsscs_hello vmsscs_connect)

add_test(NAME vmsscs_connect_unit COMMAND test_scs_connect)
set_tests_properties(vmsscs_connect_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-21e - phase-2 START/config (0x41) builder + SCS seq/ack state machine.
add_executable(test_scs_start test_scs_start.c)

target_include_directories(test_scs_start PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_start PRIVATE vmsscs_start)

add_test(NAME vmsscs_start_unit COMMAND test_scs_start)
set_tests_properties(vmsscs_start_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-691 - SCS VC engine: 0x48 credit-return builder + seq/ack + retransmit.
add_executable(test_scs_vc test_scs_vc.c)

target_include_directories(test_scs_vc PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_vc PRIVATE vmsscs_vc)

add_test(NAME vmsscs_vc_unit COMMAND test_scs_vc)
set_tests_properties(vmsscs_vc_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-246 - SCS$DIRECTORY connect + SCS$DIR_LOOKUP responder: CONNECT-RESPONSE
# Con.ID binding + lookup-response builder ("NOT PRESENT HERE" / affirmative).
add_executable(test_scs_dir test_scs_dir.c)

target_include_directories(test_scs_dir PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_dir PRIVATE vmsscs_dir)

add_test(NAME vmsscs_dir_unit COMMAND test_scs_dir)
set_tests_properties(vmsscs_dir_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-224 - VMS$VAXcluster connection-manager add-member transaction: op
# 0x14/0x01/0x02 config builders (byte-exact vs golden joiner frames), VOTES=0
# non-voting, the 0x81 response echoing the member's REAL captured (txn,checksum)
# token (spec sec 4j). THE MILESTONE builders.
add_executable(test_scs_member test_scs_member.c)

target_include_directories(test_scs_member PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_member PRIVATE vmsscs_member)

add_test(NAME vmsscs_member_unit COMMAND test_scs_member)
set_tests_properties(vmsscs_member_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-7be - SB/PB/PDT configuration queue: the architected replacement for the
# MAC-keyed peer struct. Asserts the documented queue transitions of VAXcluster
# Principles ch. 2 (pp. 2-11..2-21), including the p. 2-21 Note (an old SB with
# no other PB queued to it is REFRESHED from the formative SB on a rejoin).
add_executable(test_scs_config test_scs_config.c)

target_include_directories(test_scs_config PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_config PRIVATE vmsscs_config)

add_test(NAME vmsscs_config_unit COMMAND test_scs_config)
set_tests_properties(vmsscs_config_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)
