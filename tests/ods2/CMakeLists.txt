# tests/ods2 - Unit tests for genuine ODS-2 (Files-11 level 2) structures + reader.

add_executable(test_ods2 test_ods2.c)
target_include_directories(test_ods2 PRIVATE ${VMS_FS_INCLUDE_DIR})
target_link_libraries(test_ods2 PRIVATE ods2)

add_test(NAME ods2_unit COMMAND test_ods2)
set_tests_properties(ods2_unit PROPERTIES
    LABELS "unit;ods2;vmsfs"
    TIMEOUT 30
)
