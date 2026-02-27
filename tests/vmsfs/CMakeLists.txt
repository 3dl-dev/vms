# tests/vmsfs - Unit tests for vmsfs (VMS filesystem semantics)

add_executable(test_vmsfs test_vmsfs.c)

target_include_directories(test_vmsfs PRIVATE
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)

target_link_libraries(test_vmsfs PRIVATE vmsfs vmslnm pthread)

add_test(NAME vmsfs_unit COMMAND test_vmsfs)
set_tests_properties(vmsfs_unit PROPERTIES
    LABELS "unit;vmsfs"
    TIMEOUT 30
)
