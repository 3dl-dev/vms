# tests/vmsqueue - Unit tests for vmsqueue (Queue Manager)

add_executable(test_vmsqueue test_vmsqueue.c)

target_include_directories(test_vmsqueue PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsqueue
    ${VMS_INCLUDE_DIR}
)

target_link_libraries(test_vmsqueue PRIVATE vmsqueue)

add_test(NAME vmsqueue_unit COMMAND test_vmsqueue)
set_tests_properties(vmsqueue_unit PROPERTIES
    LABELS "unit;queue"
    TIMEOUT 30
)
