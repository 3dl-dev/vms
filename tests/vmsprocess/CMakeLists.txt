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

# Privilege bitmask -> name/description rendering (vms-pv1).
# Guards the defect where DCL's SHOW PROCESS/PRIVILEGES used its own bit
# numbering, unrelated to prvdef.h, and substituted a hard-coded
# TMPMBX/NETMBX set when the real mask was empty.
add_executable(test_priv_display test_priv_display.c)

target_include_directories(test_priv_display PRIVATE
    ${VMS_PROCESS_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)

target_link_libraries(test_priv_display PRIVATE vmsprocess pthread)

add_test(NAME priv_display_unit COMMAND test_priv_display)
set_tests_properties(priv_display_unit PROPERTIES
    LABELS "unit;process;security"
    TIMEOUT 30
)
