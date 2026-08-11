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

# vms-240 guard: the dead lnm_translate_iterative() must stay gone from C sources
# (the only surviving mention is the retired symbol-vector slot in the .sh build
# recipe, which this guard excludes by file extension).
add_test(NAME vmslnm_no_dead_iterative
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/check_no_dead_iterative.sh)
set_tests_properties(vmslnm_no_dead_iterative PROPERTIES
    LABELS "unit;lnm;guard"
    TIMEOUT 30
)
