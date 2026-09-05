# tests/cluster/sim - the R2 rung of the executive-resident VMScluster test
# ladder (design docs/design-faithful-cluster-executive.md sec 3.9, plan row
# FC-P1.4): N instances of the PURE stack wired to a virtual LAN with
# configurable loss/reorder/duplication/latency and a virtual clock.
#
# `ctest -R cluster_sim` selects every test in this directory.
#
# WHAT THIS DIRECTORY IS NOT. It is not cluster core. The gate
# tools/ci/cluster_core_includes_gate.sh scans src/kernel-core; a harness under
# tests/ is on exactly the same footing as tests/cluster/host and may use the C
# library. What it may NOT do is reimplement any protocol logic: it links the
# SHIPPING vms_pe_fsm.c (through cluster_host_pe, defined in
# tests/cluster/host/CMakeLists.txt) and drives it through the injected ops.

set(OVMX_CLUSTER_CORE_DIR ${CMAKE_SOURCE_DIR}/src/kernel-core)

add_library(cluster_sim_core STATIC
    sim_rng.c
    sim_clock.c
    sim_lan.c
    sim_node.c
    sim_engine.c
    sim_msg.c
    sim_dump.c
    sim_metric.c
    sim_scenario.c
    replay.c
)
target_include_directories(cluster_sim_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OVMX_CLUSTER_CORE_DIR}
)
# cluster_host_pe is the SHIPPING src/kernel-core/vms_pe_fsm.c built with
# -DOVMX_CLUSTER_HOST, and it carries cluster_host_codec with it. The simulator
# adds no source of its own from src/kernel-core: whatever the executive runs
# is what the simulator runs.
target_link_libraries(cluster_sim_core PUBLIC cluster_host_pe)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cluster_sim_core PRIVATE -Wall -Wextra -Werror)
endif()

# WHERE LATER PHASES PUT THEIR SCENARIOS. Plan rows FC-P2.5 and FC-P3.10 name
# tests/cluster/sim/scenarios/scs_*.c and cnxman_*.c. Add the file there, then
# ONE line here -- `cluster_sim_test(test_sim_scs scenarios/scs_connect.c)` --
# and it gets the whole harness (virtual LAN, virtual clock, the DSL, the
# SDA-like dump) with nothing else to wire. FC-P1.5's pcap replay driver
# (replay.c) joins cluster_sim_core's source list on the same terms.
function(cluster_sim_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE cluster_sim_core)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/tests/cluster/host   # cluster_test.h
    )
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME cluster_sim_${name} COMMAND ${name})
    set_tests_properties(cluster_sim_${name} PROPERTIES
        LABELS "unit;cluster;cluster_sim;pe"
        TIMEOUT 120
    )
endfunction()

# The harness's own model: the RNG, the clock/timer wheel and the virtual LAN.
# A harness whose wire model is untested cannot be evidence about anything
# else, so this rung tests itself before it tests the stack.
cluster_sim_test(test_sim_lan test_sim_lan.c)

# The FC-P1.4 done-condition: 3 simulated pe stacks form virtual circuits under
# 10 % loss, and the run is deterministic by seed.
cluster_sim_test(test_sim_vc test_sim_vc.c)

# FC-P1.5: the pcap replay driver's own done-condition -- replay a reference
# join capture through one simulated OVMX instance and assert shape +
# allowlist + seq contiguity on what it emits. Needs the R1 clean-room
# fixture loader (cluster_host_fixture) and its two path definitions, the
# same way tests/cluster/host/CMakeLists.txt's cluster_host_pe_test wires them.
# FC-P3.3's rung-R2 leg: replay the reference JOIN choreography at one
# simulated OVMX node -- the SHIPPING join FSM and the SHIPPING barrier FSM
# (cluster_host_cnxman), inbound frames from the manifest-hashed specimens
# (cluster_host_fixture), time from the simulator's own virtual clock. This is
# the "add the file, then ONE line here" path the comment above describes.
cluster_sim_test(test_sim_cnxman_join scenarios/cnxman_join.c)
target_link_libraries(test_sim_cnxman_join PRIVATE
    cluster_host_cnxman cluster_host_fixture)
target_compile_definitions(test_sim_cnxman_join PRIVATE
    OVMX_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/cluster/host/fixtures"
    OVMX_CLEANROOM_MANIFEST="${CMAKE_SOURCE_DIR}/docs/clean-room/reference-captures.sha256")
target_include_directories(test_sim_cnxman_join PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/cluster/host)

# FC-P6.5's rung-R2 leg: TWO simulated nodes complete an MSCP WRITE over the
# virtual LAN -- the client's class driver, the server's MSCP server, and BOTH
# ports' block-transfer services (the server's REQUEST DATA and the client
# port's automatic responder) are the shipping objects. Same "add the file, then
# ONE line here" path, plus the two MSCP libraries the scenario instantiates.
cluster_sim_test(test_sim_mscp_write scenarios/mscp_write.c)
target_link_libraries(test_sim_mscp_write PRIVATE
    cluster_host_mscp_cl cluster_host_mscp_srv)
target_include_directories(test_sim_mscp_write PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/cluster/host)

# FC-P4.3's rung-R2 leg: FOUR simulated systems each build their OWN copy of
# the Lock Directory Weight Vector through the SHIPPING Phase 2 commit, and the
# directory duty is exercised BETWEEN them -- which is the only place the
# "logically equivalent copies" property and the CSV-index-order self-check can
# be tested at all. Needs cluster_host_cnxman (the shipping phase2 + LDWV).
cluster_sim_test(test_sim_dlm_directory scenarios/dlm_directory.c)
target_link_libraries(test_sim_dlm_directory PRIVATE cluster_host_cnxman)

# FC-P4.6's rung-R2 leg: the REAL lock engine (vms_lock.c on FC-P4.9's host
# backend), the REAL requester FSM and the REAL Lock Directory Weight Vector in
# one host process, against SIMULATED master/directory systems. It proves the
# two things one node cannot: that a lookup resolved through OUR copy of the
# vector LANDS on a system whose OWN copy agrees (the CSV-order falsifier), and
# that a root name nobody has ever named on the wire puts ZERO FRAMES on the
# LAN -- not merely that it returns an error.
#
# The include order is load-bearing and is the SAME recipe
# tests/cluster/host/test_lock_dir.c uses: lock_shim/ FIRST (so
# `vms_internal.h` and `exec_kbackend.h` resolve to FC-P4.9's host backend),
# then kernel-core, then src/kernel for the portable vms_ioctl.h.
add_executable(test_sim_dlm_requester
    scenarios/dlm_requester.c
    ${CMAKE_SOURCE_DIR}/tests/cluster/host/vms_frame_compose.c
)
target_include_directories(test_sim_dlm_requester BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/cluster/host/lock_shim
)
target_include_directories(test_sim_dlm_requester PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OVMX_CLUSTER_CORE_DIR}
    ${CMAKE_SOURCE_DIR}/src/kernel
    ${CMAKE_SOURCE_DIR}/tests/cluster/host
)
target_compile_definitions(test_sim_dlm_requester PRIVATE OVMX_CLUSTER_HOST)
target_link_libraries(test_sim_dlm_requester PRIVATE
    cluster_host_dlm
    cluster_host_cnxman
    cluster_host_lock
    cluster_sim_core
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_sim_dlm_requester PRIVATE -Wall -Wextra)
endif()
add_test(NAME cluster_sim_test_sim_dlm_requester COMMAND test_sim_dlm_requester)
set_tests_properties(cluster_sim_test_sim_dlm_requester PROPERTIES
    LABELS "unit;cluster;cluster_sim;lock;dlm"
    TIMEOUT 120
)

cluster_sim_test(test_replay test_replay.c)
target_link_libraries(test_replay PRIVATE cluster_host_fixture)
target_compile_definitions(test_replay PRIVATE
    OVMX_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/cluster/host/fixtures"
    OVMX_CLEANROOM_MANIFEST="${CMAKE_SOURCE_DIR}/docs/clean-room/reference-captures.sha256"
)
