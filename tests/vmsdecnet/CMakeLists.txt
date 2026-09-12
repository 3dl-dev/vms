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
target_include_directories(test_dnet_cterm PRIVATE
    ${CMAKE_SOURCE_DIR}/src/libvms/include    # ovmx_identity.h (INV-1 banner SSOT)
    ${CMAKE_SOURCE_DIR}/src/vmsfs/include)     # transitive header of ovmx_identity.h
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

# rd vms-1e9: the NCP node database (the configuration surface's persisted node
# table) + name<->address resolution. Address parse/range, DEFINE/SET add +
# update, node-name uniqueness, case-insensitive resolution, SHOW ordering,
# persistence round-trip + corrupt-file handling. Warning-clean under -Werror.
add_executable(test_dnet_nodedb test_dnet_nodedb.c)
target_link_libraries(test_dnet_nodedb PRIVATE vmsdecnet_nodedb)
target_compile_options(test_dnet_nodedb PRIVATE -Werror)
add_test(NAME vmsdecnet_nodedb_unit COMMAND test_dnet_nodedb)

# rd vms-f52: the DECnet OBJECT database (SET/DEFINE/SHOW/CLEAR/PURGE OBJECT).
# Library unit test: number-keyed set/update, object-name uniqueness, name/number
# resolution, FILE bounds, ascending-number SHOW ordering, persistence round-trip
# + corrupt-file handling. Warning-clean under -Werror.
add_executable(test_dnet_objectdb test_dnet_objectdb.c)
target_link_libraries(test_dnet_objectdb PRIVATE vmsdecnet_objectdb)
target_compile_options(test_dnet_objectdb PRIVATE -Werror)
add_test(NAME vmsdecnet_objectdb_unit COMMAND test_dnet_objectdb)

# NCP.EXE OBJECT command dispatch, end-to-end against the shipped binary (rd
# vms-f52): the FIRST command-dispatch test for NCP.EXE -- SET/DEFINE/SHOW/CLEAR/
# PURGE OBJECT over a temp DB (OVMX_DECNET_OBJECTDB), asserting persistence across
# invocations + honest errors. No socket/privilege/dev-vms.
add_test(NAME vmsdecnet_ncp_object
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_dnet_ncp_object.sh)
set_tests_properties(vmsdecnet_ncp_object PROPERTIES
    ENVIRONMENT "NCP_EXE=$<TARGET_FILE:ncp_exe>")

# rd vms-ea8/vms-6a4: node-filespec splitter (NODE"user pw acct"::spec) that
# feeds the FAL/DAP COPY client. Pure logic -- compile the splitter TU directly
# (no FAL/RMS/auth link stack needed for a syntax-only unit).
add_executable(test_dnet_nodespec test_dnet_nodespec.c
    ${CMAKE_SOURCE_DIR}/src/vmsdecnet/fal/dnet_nodespec.c)
target_include_directories(test_dnet_nodespec PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsdecnet/fal/include
    ${CMAKE_SOURCE_DIR}/src/vmsdecnet/cterm/include)  # dnet_cterm.h: caps + codes
target_compile_options(test_dnet_nodespec PRIVATE -Werror)
add_test(NAME vmsdecnet_nodespec_unit COMMAND test_dnet_nodespec)

# rd vms-8c2: the DAP (Data Access Protocol) message codec + the FAL access-
# control credential decoder behind $ COPY node::file. Proves (1) every served
# DAP message round-trips encode->decode, (2) blocked messages walk in a single
# buffer, (3) the DAP decoder survives 200k fuzzed inputs + every truncated
# prefix without over-reading (ASan/UBSan), (4) the FAL access decoder pulls
# username=SYSTEM + a RETAINED password out of the EXACT oracle connect bytes
# and the builder re-emits them byte-identical, and (5) the FAL access decoder
# is fuzz-clean and leaks no credential on any malformed input. Attacker-facing
# decoders, so it is built WITH -fsanitize=address,undefined -- an over-read is
# a hard failure. Warning-clean under -Werror.
add_executable(test_dnet_dap test_dnet_dap.c)
target_link_libraries(test_dnet_dap PRIVATE vmsdecnet_dap vmsdecnet_cterm)
target_compile_options(test_dnet_dap PRIVATE -Werror)
# ASan/UBSan is where an over-read in the attacker-facing decoders becomes a
# HARD failure. Add it only where the toolchain can actually compile+LINK it
# (a musl/freestanding cross may lack the sanitizer runtime): the fuzz test's
# own bounds assertions (consumed <= n, no credential leak on failure) still
# run everywhere; the sanitizer deepens the check on the host gnu leg.
include(CheckCSourceCompiles)
set(_dap_san_save "${CMAKE_REQUIRED_FLAGS}")
set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
check_c_source_compiles("int main(void){return 0;}" OVMX_DAP_SANITIZE_OK)
set(CMAKE_REQUIRED_FLAGS "${_dap_san_save}")
if(OVMX_DAP_SANITIZE_OK)
    target_compile_options(test_dnet_dap PRIVATE -fsanitize=address,undefined)
    target_link_options(test_dnet_dap PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME vmsdecnet_dap_unit COMMAND test_dnet_dap)

# rd vms-aac0: systematic ASan/UBSan fuzz + bound proof of the DECnet routing
# RECEIVE path -- the router-hello decoder (SPEC-DERIVED, never met real wire),
# the endnode-hello decoder, and the adjacency FSM. Before an OVMX node trades
# DECnet adjacency with a LIVE real OpenVMS VAX peer, this proves no frame a peer
# could send crashes / over-reads / drives UB in the receive path (>=200k fuzzed
# inputs per decoder + >=200k adversarial FSM sequences), and asserts the live
# two-way-HELLO -> UP path positively. Attacker-facing, so -- exactly like the
# DAP fuzz above -- it is built WITH -fsanitize=address,undefined so an over-read
# is a HARD failure. Warning-clean under -Werror.
add_executable(test_dnet_routing_fuzz test_dnet_routing_fuzz.c)
target_link_libraries(test_dnet_routing_fuzz PRIVATE vmsdecnet_routing)
target_compile_options(test_dnet_routing_fuzz PRIVATE -Werror)
# Add ASan/UBSan only where the toolchain can compile+LINK it (a musl/free-
# standing cross may lack the sanitizer runtime); the harness's own bound
# assertions still run everywhere, the sanitizer deepens the check on the host
# gnu leg. Mirrors the OVMX_DAP_SANITIZE_OK probe above.
if(NOT DEFINED OVMX_DAP_SANITIZE_OK)
    include(CheckCSourceCompiles)
    set(_rtfuzz_san_save "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
    check_c_source_compiles("int main(void){return 0;}" OVMX_DAP_SANITIZE_OK)
    set(CMAKE_REQUIRED_FLAGS "${_rtfuzz_san_save}")
endif()
if(OVMX_DAP_SANITIZE_OK)
    target_compile_options(test_dnet_routing_fuzz PRIVATE -fsanitize=address,undefined)
    target_link_options(test_dnet_routing_fuzz PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME vmsdecnet_routing_fuzz_unit COMMAND test_dnet_routing_fuzz)

# rd vms-0a9: isolation proof of the DECnet router run-mode EMIT path -- the
# spec-faithful router-hello DECNETD emits in --router mode. Complements the
# routing_fuzz RECEIVE-path proof above: it round-trips the emitted bytes,
# asserts the emitted node-type says "router", drives an OVMX endnode's
# DR-selection with those bytes (the endnode names the router in its rtr field),
# and fuzzes the ENCODER under adversarial field/cap values + the emitted frame
# arbitrarily mutated into the receive path (>=100k iters each). Attacker-facing
# emit path that will one day reach a real VAX endnode, so -- like the fuzz above
# -- it is built WITH -fsanitize=address,undefined and honours vms-abf's
# OVMX_FUZZ_REQUIRE_SANITIZERS teeth-enforcement. NO real node is touched (all
# in-process / OVMX<->OVMX). Links the ENGINE (build_router_hello_frame +
# rx_frame). Warning-clean under -Werror.
add_executable(test_dnet_router_emit_fuzz test_dnet_router_emit_fuzz.c)
target_link_libraries(test_dnet_router_emit_fuzz PRIVATE vmsdecnet_engine)
target_compile_options(test_dnet_router_emit_fuzz PRIVATE -Werror)
if(OVMX_DAP_SANITIZE_OK)
    target_compile_options(test_dnet_router_emit_fuzz PRIVATE -fsanitize=address,undefined)
    target_link_options(test_dnet_router_emit_fuzz PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME vmsdecnet_router_emit_fuzz_unit COMMAND test_dnet_router_emit_fuzz)

# The DECnet startup GATE + serve-default decision SYS$MANAGER:STARTNET.COM
# relies on (rd vms-a70 direction B): --show-executor self-sources the executor
# address and exits SUCCESS iff DECnet is configured (the gate STARTNET branches
# on), and the persistent endnode daemon serves inbound object 42 by default.
# Runs the shipped DECNETD.EXE on the host -- no socket, no /dev/vms, no privilege.
add_test(NAME vmsdecnet_startnet_gate
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_decnetd_startnet_gate.sh)
set_tests_properties(vmsdecnet_startnet_gate PROPERTIES
    ENVIRONMENT "DECNETD_EXE=$<TARGET_FILE:decnetd_exe>")
