# tests/vmsprocess - Unit tests for vmsprocess (PCB, AST, event flags)

add_executable(test_vmsprocess test_vmsprocess.c)

target_include_directories(test_vmsprocess PRIVATE
    ${VMS_PROCESS_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)

target_link_libraries(test_vmsprocess PRIVATE vmsprocess pthread)

add_test(NAME vmsprocess_unit COMMAND test_vmsprocess)
set_tests_properties(vmsprocess_unit PROPERTIES
    LABELS "unit;process"
    TIMEOUT 30
)
