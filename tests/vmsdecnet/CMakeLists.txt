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

# rd vms-985: NSP data flow -- receive window + link-service stop/resume, ACK /
# NAK, go-back-N retransmission. Replays the real VAX V7.3 PDUs of the two
# vms-0a7 S1 large-COPY failures (tests/lab/captures/vms-0a7-s1-20261102/), then
# moves 20000 records each way over a lossy channel with a slow reader.
add_executable(test_dnet_nsp_flow test_dnet_nsp_flow.c)
target_link_libraries(test_dnet_nsp_flow PRIVATE vmsdecnet_nsp)
target_compile_options(test_dnet_nsp_flow PRIVATE -Werror)
add_test(NAME vmsdecnet_nsp_flow_unit COMMAND test_dnet_nsp_flow)

# rd vms-985: sanitizer fuzz of the peer-reachable data-flow receive paths (codec
# DATA/LS/ACK/OTHACK decode + the link FSM under hostile acks, NAKs, link
# services and segment numbers). The codec and link sources are compiled into
# the target so ASan/UBSan instrument them.
add_executable(test_dnet_nsp_flow_fuzz test_dnet_nsp_flow_fuzz.c
    ${CMAKE_SOURCE_DIR}/src/vmsdecnet/nsp/dnet_nsp.c
    ${CMAKE_SOURCE_DIR}/src/vmsdecnet/nsp/dnet_link.c)
target_include_directories(test_dnet_nsp_flow_fuzz PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsdecnet/nsp/include)
target_compile_options(test_dnet_nsp_flow_fuzz PRIVATE -Werror)
if(NOT DEFINED OVMX_DAP_SANITIZE_OK)
    include(CheckCSourceCompiles)
    set(_nspfuzz_san_save "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
    check_c_source_compiles("int main(void){return 0;}" OVMX_DAP_SANITIZE_OK)
    set(CMAKE_REQUIRED_FLAGS "${_nspfuzz_san_save}")
endif()
if(OVMX_DAP_SANITIZE_OK)
    target_compile_options(test_dnet_nsp_flow_fuzz PRIVATE -fsanitize=address,undefined)
    target_link_options(test_dnet_nsp_flow_fuzz PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME vmsdecnet_nsp_flow_fuzz_unit COMMAND test_dnet_nsp_flow_fuzz)

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

# rd vms-1f69: the DECnet configuration STORE (SYS$SYSTEM:NETNODE_LOCAL.DAT /
# NETNODE_REMOTE.DAT / NETOBJECT.DAT through the VMS file layer) NCP.EXE and
# NETACP share. Links LIBVMS but deliberately NOT LIBVMSRMS, so the no-file-layer
# honest-failure path is exercised deterministically; the env hook round-trips.
add_executable(test_dnet_ncpstore test_dnet_ncpstore.c)
target_link_libraries(test_dnet_ncpstore PRIVATE vmsdecnet_ncpstore)
target_compile_options(test_dnet_ncpstore PRIVATE -Werror)
add_test(NAME vmsdecnet_ncpstore_unit COMMAND test_dnet_ncpstore)

# rd vms-1f69: NCP.EXE SET/DEFINE/SHOW EXECUTOR + NODE dispatch end to end over
# the host-test hook (the booted VMS-file path is the acceptance battery's).
add_test(NAME vmsdecnet_ncp_executor
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_dnet_ncp_executor.sh)
set_tests_properties(vmsdecnet_ncp_executor PROPERTIES
    ENVIRONMENT "NCP_EXE=$<TARGET_FILE:ncp_exe>")

# rd vms-1f69: the executive L2 station-address policy (src/kernel/
# vms_l2_station.h), the same header vms.ko enforces in VMS_IOCTL_L2_OPEN.
add_executable(test_l2_station test_l2_station.c)
target_include_directories(test_l2_station PRIVATE ${CMAKE_SOURCE_DIR}/src/kernel)
target_compile_options(test_l2_station PRIVATE -Werror)
add_test(NAME vmsdecnet_l2_station_unit COMMAND test_l2_station)

# rd vms-9394: the receive-ring bookkeeping of the NetBSD executive L2 binding
# (src/kernel-core/vms_l2_ring.h), the same header vms.kmod runs under its rx
# mutex -- FIFO order, drop-oldest on overflow, wrap-around, ethertype field.
add_executable(test_l2_ring test_l2_ring.c)
target_include_directories(test_l2_ring PRIVATE ${CMAKE_SOURCE_DIR}/src/kernel-core)
target_compile_options(test_l2_ring PRIVATE -Werror)
add_test(NAME vmsdecnet_l2_ring_unit COMMAND test_l2_ring)

# rd vms-1f69: the EXECUTIVE datalink backend the booted DECNETD links
# (ovmx_datalink_exec): names itself, and with no /dev/vms fails honestly with
# no AF_PACKET fallback compiled in.
if(TARGET ovmx_datalink_exec)
    add_executable(test_datalink_exec_backend test_datalink_exec_backend.c)
    target_link_libraries(test_datalink_exec_backend PRIVATE ovmx_datalink_exec)
    target_compile_options(test_datalink_exec_backend PRIVATE -Werror)
    add_test(NAME vmsdecnet_datalink_exec_backend COMMAND test_datalink_exec_backend)
endif()

# rd vms-ea8/vms-6a4: node-filespec splitter (NODE"user pw acct"::spec) that
# feeds the FAL/DAP COPY client. Pure logic -- compile the splitter TU directly
# (no FAL/RMS/auth link stack needed for a syntax-only unit).
add_executable(test_dnet_nodespec test_dnet_nodespec.c
    ${CMAKE_SOURCE_DIR}/src/vmsdecnet/fal/dnet_nodespec.c)
target_include_directories(test_dnet_nodespec PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsdecnet/fal/include
    ${CMAKE_SOURCE_DIR}/src/vmsdecnet/cterm/include)  # dnet_cterm.h: caps + codes
target_compile_options(test_dnet_nodespec PRIVATE -Werror)
# The splitter parses an untrusted COPY argument carrying an access-control
# password, so its fuzz is held to the same bar as the DAP decoder: build it
# WITH -fsanitize=address,undefined where the toolchain can (a musl/freestanding
# cross may lack the runtime -- the fuzz's own leak/bounds assertions still run
# everywhere; the sanitizer turns an over-read into a HARD failure on the host).
include(CheckCSourceCompiles)
set(_ns_san_save "${CMAKE_REQUIRED_FLAGS}")
set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
check_c_source_compiles("int main(void){return 0;}" OVMX_NODESPEC_SANITIZE_OK)
set(CMAKE_REQUIRED_FLAGS "${_ns_san_save}")
if(OVMX_NODESPEC_SANITIZE_OK)
    target_compile_options(test_dnet_nodespec PRIVATE -fsanitize=address,undefined)
    target_link_options(test_dnet_nodespec PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME vmsdecnet_nodespec_unit COMMAND test_dnet_nodespec)

# rd vms-a70 direction B: the CTERM HOST role (dnet_cterm_hostfsm) against the
# two real-VAX-host captures -- exact bytes where a VMS host's content is
# deterministic, every real client/host message decoded, the FSM replaying a
# real VAX client's session, and 200k mutated client segments fuzzed through
# every FSM state. Attacker-facing decoder: built WITH ASan/UBSan.
add_executable(test_dnet_cterm_host test_dnet_cterm_host.c)
target_link_libraries(test_dnet_cterm_host PRIVATE vmsdecnet_cterm)
target_include_directories(test_dnet_cterm_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_dnet_cterm_host PRIVATE -Werror)
if(OVMX_NODESPEC_SANITIZE_OK)
    target_compile_options(test_dnet_cterm_host PRIVATE -fsanitize=address,undefined)
    target_link_options(test_dnet_cterm_host PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME vmsdecnet_cterm_host_unit COMMAND test_dnet_cterm_host)

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
# rd vms-d85 inbound: the FAL SERVER replayed against the exact DAP segments a
# real OpenVMS VAX COPY sent it (PUT, DIRECTORY LIST, OPEN+GET, honest miss).
# File I/O is captured in memory by the test (it defines the RMS-facing hooks).
add_executable(test_dnet_fal_server test_dnet_fal_server.c)
target_link_libraries(test_dnet_fal_server PRIVATE vmsdecnet_fal vmsdecnet_dap vmsdecnet_cterm)
target_compile_options(test_dnet_fal_server PRIVATE -Werror)
# rd vms-277a: argv[1] = the committed real-VAX FAL verbs capture; the test
# replays its client segments and compares the replies (missing file = FAIL).
add_test(NAME vmsdecnet_fal_server_replay COMMAND test_dnet_fal_server
         ${CMAKE_SOURCE_DIR}/tests/lab/captures/decnet-fal-verbs-20261008/falverbs-wire.txt
         ${CMAKE_SOURCE_DIR}/tests/lab/captures/decnet-fal-verbs-20261008/live-bracket/falverbs-live-wire.txt
         ${CMAKE_SOURCE_DIR}/tests/lab/captures/decnet-fal-verbs-20261008/vax-to-vax-sys-login/falverbs-v2v-wire.txt
         ${CMAKE_SOURCE_DIR}/tests/lab/captures/decnet-fal-verbs-20261008/live-bracket)

# rd vms-36aa: the FAL's real RMS record path (dnet_fal_wput / dnet_fal_rget,
# not stubbed) stores a VAX's VFC records with the control in RAB$L_RHB, so a
# 290-block file is 290 blocks on OVMX, and reads them back byte-identical.
add_executable(test_dnet_fal_vfc test_dnet_fal_vfc.c)
target_include_directories(test_dnet_fal_vfc PRIVATE ${VMS_RMS_INCLUDE_DIR} ${VMS_INCLUDE_DIR})
target_link_libraries(test_dnet_fal_vfc PRIVATE vmsdecnet_fal vmsdecnet_dap vmsrms vms)
target_compile_options(test_dnet_fal_vfc PRIVATE -Werror -Wno-overflow)
add_test(NAME vmsdecnet_fal_vfc COMMAND test_dnet_fal_vfc)

# rd vms-47fd: INBOUND DECnet MAIL-11 (object 27). Reads the real VAX V7.3
# capture itself and replays every VAX1 segment through the OVMX receiver,
# asserting VAX2's replies byte for byte (accepted message + NOSUCHUSR), then the
# truncation/oversize/store-failure refusals and a mutation fuzz. No executive.
add_executable(test_dnet_mail11 test_dnet_mail11.c)
target_link_libraries(test_dnet_mail11 PRIVATE vmsdecnet_mail11)
target_compile_options(test_dnet_mail11 PRIVATE -Werror)
add_test(NAME vmsdecnet_mail11_replay
         COMMAND test_dnet_mail11
                 ${CMAKE_SOURCE_DIR}/tests/lab/captures/decnet-mail11-20261008/mail11-wire.txt)

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
# Runs the test image's --show-executor on the host (the same netacp_run the
# shipped NETACP.EXE runs) -- no socket, no /dev/vms, no privilege.
add_test(NAME vmsdecnet_startnet_gate
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_decnetd_startnet_gate.sh)
set_tests_properties(vmsdecnet_startnet_gate PROPERTIES
    ENVIRONMENT "DECNETD_EXE=$<TARGET_FILE:netacp_test_exe>")

# ---------------------------------------------------------------------------
# NETACP_TEST.EXE (rd vms-0a7 S3): every DECnet self-test and lab mode that
# DECNETD.EXE used to carry behind argv flags. It compiles the shipped
# src/vmsdecnet/engine/decnetd.c itself (netacp_test.c #includes it, with no
# image entry defined) and links the same libraries as NETACP.EXE, so each test
# drives the real NETACP code. The test-only peers are threads: Threads here,
# never in NETACP.EXE. Staged for the QEMU battery / boot e2e on a scratch test
# volume, not the system disk.
add_executable(netacp_test_exe netacp_test.c
               ${CMAKE_SOURCE_DIR}/src/vmsdecnet/engine/decnetd_rms_bind.c)
target_include_directories(netacp_test_exe PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsdecnet/engine ${_netacp_includes})
find_package(Threads REQUIRED)
target_link_libraries(netacp_test_exe PRIVATE ${_netacp_libs} Threads::Threads)
set_target_properties(netacp_test_exe PROPERTIES OUTPUT_NAME "NETACP_TEST" SUFFIX ".EXE")

add_test(NAME decnetd_sethost_src_codes
         COMMAND netacp_test_exe --set-host-src-codes-selftest)
# rd vms-ea8/vms-6a4: the OUTBOUND $ COPY command layer floor -- copy_plan
# direction/creds/spec derivation (GET + PUT) and the parsed creds driving a
# real object-17 connect refused without auth, both directions. No executive.
add_test(NAME decnetd_copy_selftest
         COMMAND netacp_test_exe --copy-selftest)
# rd vms-dda: the TASK-TO-TASK client host floor -- open a logical link by NAME
# (format-1 NAMED descriptor, NODE::"TASK=x"), the passive side decodes the
# name, a request + reply move BOTH directions byte-verified, then disconnect.
# The generic-object addressing + bidirectional pump the _NET: $QIO broker
# (Option 1, NETACP-brokered) will expose. No executive.
add_test(NAME decnetd_task_selftest
         COMMAND netacp_test_exe --task-selftest)
# rd vms-22c (a1-2): the exec<->NETACP T1 broker record codec floor -- request/
# response round-trip, bounds-validated decode (200k-mutation + truncated-prefix
# fuzz, ASan/UBSan-clean), and the correlation-id anti-cross-talk guard. No
# executive; the mailbox seam + qio_net_op + NETACP servicing build on it.
# rd vms-22c (a1-2): the NCB connect-block parser floor -- NODE::"TASK=name"
# -> {node, task/object}, bounds-safe refusal of malformed input, fuzzed. A
# spec-derived LOCAL API parser (never a wire field); no executive.
add_test(NAME decnetd_net_ncb_selftest
         COMMAND netacp_test_exe --net-ncb-selftest)
add_test(NAME decnetd_net_broker_selftest
         COMMAND netacp_test_exe --net-broker-selftest)
# rd vms-22c (a1-2 integration): the NETACP broker SERVICE DISPATCH floor --
# dnet_broker_serve driving OPEN/SEND/RECV/CLOSE against the NSP link engine
# over a socketpair, correlation-matched, with honest never-crash refusals
# (unknown op, too-short OPEN). No executive; the mailbox $CREMBX seam +
# qio_net_op marshalling + the IO$M_NOW serve loop build on it.
add_test(NAME decnetd_net_service_selftest
         COMMAND netacp_test_exe --net-service-selftest)
# rd vms-dda: OUTBOUND links brokered through NETACP -- the host floor. A real
# NETACP pool serves clients that reach it ONLY through broker records
# (dnet_broker_xfer, the marshalling libvms's qio_net_op runs) over an
# in-process queue standing in for the executive mailboxes; a second engine
# across a socketpair is the remote. COPY's FAL client end to end through
# NETACP, a remote access-control rejection (INVLOGIN), the local loopback
# (0::), per-process share + full-pool refusals (EXQUOTA), handle-ownership
# and correlation-id refusals, a silent NETACP (DEVOFFLINE). No executive;
# the booted $QIO _NET: proof is --net-loopback-accept-test in the battery.
add_test(NAME decnetd_netacp_broker_selftest
         COMMAND netacp_test_exe --netacp-broker-selftest)
set_tests_properties(decnetd_netacp_broker_selftest PROPERTIES TIMEOUT 120)
# rd vms-30e: NCP SHOW / DCL SHOW NETWORK read the RUNNING NETACP -- the host
# floor. The snapshot record codec (round-trip, every truncated prefix,
# over-bound counts/strings, control bytes, 200k mutations); a real NETACP
# pool serving IO$_ACPCONTROL queries through broker records from its live
# state (node database on disk, an adjacency from a real hello, one link that
# completed its NSP handshake and one connecting, the high-water counter,
# paging, BADPARAM / BUFFEROVF / DEVOFFLINE); and the formatters diffed line
# for line against the real OpenVMS VAX V7.3 transcripts in
# docs/oracle/vax-ncp-show (timestamps normalized). No executive.
add_test(NAME decnetd_netacp_show_selftest
         COMMAND netacp_test_exe --netacp-show-selftest
                 --oracle-dir ${CMAKE_SOURCE_DIR}/docs/oracle/vax-ncp-show)
# rd vms-ea8: the OUTBOUND $ COPY client transport PUMP floor -- copy_client_run
# (the same client the live $ COPY drives) brings up an object-17 link + NSP
# link-service + a DAP session over the copy_wire transport against a threaded
# test-double server; an honest miss round-trips end to end. No executive (the
# authenticated byte-verified transfer is --copy-accept-test in the battery).
add_test(NAME decnetd_copy_transport_selftest
         COMMAND netacp_test_exe --copy-transport-selftest)

# rd vms-0a7 S3 gates. (a) the libc thread/wait/pipe ratchet: src/vmsdecnet has
# no pthread_create/poll/pipe, DCL no more than its baseline, NETACP.EXE links no
# Threads -- with planted-call negative controls. (b) no shipped src/vmsdecnet
# image parses argv "--" beyond the (now empty) baseline, NETACP.EXE is main(void) -- with planted-parser negative controls.
add_test(NAME vmsdecnet_netacp_no_libc_wait
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_netacp_no_libc_wait.sh)
set_tests_properties(vmsdecnet_netacp_no_libc_wait PROPERTIES
    PASS_REGULAR_EXPRESSION "NETACP_NO_LIBC_WAIT_OK")
add_test(NAME vmsdecnet_no_argv_flags
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_no_argv_flags.sh)
set_tests_properties(vmsdecnet_no_argv_flags PROPERTIES
    PASS_REGULAR_EXPRESSION "NO_ARGV_FLAGS_OK")

# rd vms-0a7 S2: RTPAD.EXE's CTERM terminal side (rtpad_session) replayed against
# real VAX V7.3 host sessions (byte-identical client segments, no-echo Password:,
# %REM-S-END), a file SYS$INPUT, a refused send, and 300k mutated host segments
# (peer-receive fuzz) under ASan/UBSan where available.
add_executable(test_rtpad_session test_rtpad_session.c)
target_link_libraries(test_rtpad_session PRIVATE rtpad_session)
target_include_directories(test_rtpad_session PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_rtpad_session PRIVATE -Werror)
if(OVMX_NODESPEC_SANITIZE_OK)
    target_compile_options(test_rtpad_session PRIVATE -fsanitize=address,undefined)
    target_link_options(test_rtpad_session PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME vmsdecnet_rtpad_session COMMAND test_rtpad_session)
# rd vms-0a7 S2: RTPAD.EXE and its CTERM codec reach nothing but VMS services (no
# libc I/O, stdio, isatty/termios, poll, pipe, thread, sleep) and take no argv;
# the negative control plants each violation and must see it caught.
add_test(NAME vmsdecnet_rtpad_no_libc
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_rtpad_no_libc.sh)
