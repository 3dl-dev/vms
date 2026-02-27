# tests/vmslnm - Unit tests for vmslnm (Logical Name Manager)

add_executable(test_vmslnm test_vmslnm.c)

target_include_directories(test_vmslnm PRIVATE
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)

target_link_libraries(test_vmslnm PRIVATE vmslnm pthread)

add_test(NAME vmslnm_unit COMMAND test_vmslnm)
set_tests_properties(vmslnm_unit PROPERTIES
    LABELS "unit;lnm"
    TIMEOUT 30
)
