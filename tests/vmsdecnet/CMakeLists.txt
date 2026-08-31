# tests/vmsdecnet - DECnet Phase IV codec unit tests (rd vms-851 / vms-6986).

add_executable(test_dnet_hello test_dnet_hello.c)
target_link_libraries(test_dnet_hello PRIVATE vmsdecnet_routing)
add_test(NAME vmsdecnet_hello_unit COMMAND test_dnet_hello)

add_executable(test_dnet_nsp test_dnet_nsp.c)
target_link_libraries(test_dnet_nsp PRIVATE vmsdecnet_nsp)
add_test(NAME vmsdecnet_nsp_unit COMMAND test_dnet_nsp)

# rd vms-b15, rung 3: routing adjacency state machine (hello cadence + listen
# timer + up/down). Warning-clean under -Wall -Wextra -Werror.
add_executable(test_dnet_adjacency test_dnet_adjacency.c)
target_link_libraries(test_dnet_adjacency PRIVATE vmsdecnet_routing)
target_compile_options(test_dnet_adjacency PRIVATE -Werror)
add_test(NAME vmsdecnet_adjacency_unit COMMAND test_dnet_adjacency)
