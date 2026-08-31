# tests/vmsdecnet - DECnet Phase IV codec unit tests (rd vms-851).

add_executable(test_dnet_hello test_dnet_hello.c)
target_link_libraries(test_dnet_hello PRIVATE vmsdecnet_routing)
add_test(NAME vmsdecnet_hello_unit COMMAND test_dnet_hello)
