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
