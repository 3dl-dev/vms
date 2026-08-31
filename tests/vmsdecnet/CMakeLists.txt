# tests/vmsdecnet - DECnet Phase IV codec unit tests (rd vms-851 / vms-6986).

add_executable(test_dnet_hello test_dnet_hello.c)
target_link_libraries(test_dnet_hello PRIVATE vmsdecnet_routing)
add_test(NAME vmsdecnet_hello_unit COMMAND test_dnet_hello)

add_executable(test_dnet_nsp test_dnet_nsp.c)
target_link_libraries(test_dnet_nsp PRIVATE vmsdecnet_nsp)
add_test(NAME vmsdecnet_nsp_unit COMMAND test_dnet_nsp)

# rd vms-c23, engine rung 2: the NSP LOGICAL-LINK connection service. Two links
# establish (CI/CC), run (data segment + ack), and tear down (DI/DC); a fresh CI
# retransmits and abandons as UNREACHABLE after the oracle-informed budget; and
# two ENGINES open a link + move a data segment over a real socketpair(2)
# exercising the full build_data_frame -> wire -> link_rx path DECNETD uses.
add_executable(test_dnet_link test_dnet_link.c)
target_link_libraries(test_dnet_link PRIVATE vmsdecnet_engine)
target_compile_options(test_dnet_link PRIVATE -Werror)
add_test(NAME vmsdecnet_link_unit COMMAND test_dnet_link)

# rd vms-4d2, engine rung 3: the CTERM (Command Terminal) protocol behind
# $ SET HOST. Proves (1) every CTERM message type round-trips encode->decode,
# (2) a terminal + host session bind/negotiate/carry-I/O/unbind on raw PDUs, and
# (3) two ENGINES carry a WHOLE $ SET HOST terminal session -- Bind, terminal-
# characteristics, host screen output (Write), terminal keystrokes (Read Data) +
# an out-of-band ^Y, and Unbind -- as real on-wire NSP data frames over a
# socketpair, every CTERM payload byte-identical. SPEC-DERIVED (no oracle
# specimen; register sec 4.7). Warning-clean under -Werror.
add_executable(test_dnet_cterm test_dnet_cterm.c)
target_link_libraries(test_dnet_cterm PRIVATE vmsdecnet_engine vmsdecnet_cterm)
target_compile_options(test_dnet_cterm PRIVATE -Werror)
add_test(NAME vmsdecnet_cterm_unit COMMAND test_dnet_cterm)

# rd vms-b15, rung 3: routing adjacency state machine (hello cadence + listen
# timer + up/down). Warning-clean under -Wall -Wextra -Werror.
add_executable(test_dnet_adjacency test_dnet_adjacency.c)
target_link_libraries(test_dnet_adjacency PRIVATE vmsdecnet_routing)
target_compile_options(test_dnet_adjacency PRIVATE -Werror)
add_test(NAME vmsdecnet_adjacency_unit COMMAND test_dnet_adjacency)

# rd vms-0aba: Ethernet Router Hello codec, sibling of the endnode-hello
# codec. SPEC-DERIVED (no committed oracle specimen) -- self round-trip test.
add_executable(test_dnet_router_hello test_dnet_router_hello.c)
target_link_libraries(test_dnet_router_hello PRIVATE vmsdecnet_routing)
target_compile_options(test_dnet_router_hello PRIVATE -Werror)
add_test(NAME vmsdecnet_router_hello_unit COMMAND test_dnet_router_hello)

# rd vms-449d, engine rung 1: the routing ENGINE that MOVES FRAMES. Proves the
# HELLO the engine builds is byte-identical to the vms-3be VAX capture (Rule-8
# oracle), then shuttles built HELLO frames between two engine instances over a
# real socketpair(2) to drive the adjacency SM DOWN->INITIALIZING->UP->DOWN --
# the datalink + HELLO tx/rx + adjacency-drive end to end, no CAP_NET_RAW.
add_executable(test_dnet_engine test_dnet_engine.c)
target_link_libraries(test_dnet_engine PRIVATE vmsdecnet_engine)
target_compile_options(test_dnet_engine PRIVATE -Werror)
add_test(NAME vmsdecnet_engine_unit COMMAND test_dnet_engine)
