# vmsdecnet engine - DECnet Phase IV routing ENGINE (rd vms-449d, rung 1 of the
# engine lane under epic vms-30e).
#
# The engine that MOVES FRAMES using the three landed engine-agnostic codecs
# (vmsdecnet_routing: dnet_hello + dnet_adjacency). Operator ruling 2026-08-31
# (rd vms-a1c): Option B -- a userspace routing engine over AF_PACKET SOCK_RAW,
# forking the proven src/libdatalink raw-L2 datalink, NOT an in-kernel AF_DECnet
# forward-port. See docs/decnet-provenance-register.md sec 6.
#
# dnet_engine: the socketless engine core -- pure logic over frame buffers + an
# injected clock (same discipline as the codecs), so it is deterministically
# unit-testable and substrate-neutral. It binds the HELLO codec + the adjacency
# state machine into a live routing endnode and owns the VMS-faithful
# presentation surface (executor / circuit / SHOW ADJACENT NODES). It touches NO
# socket: Rule 1 -- the raw datalink is HIDDEN behind the engine, exactly as
# hides its SCA socket behind the SCS surface.
add_library(vmsdecnet_engine STATIC dnet_engine.c)
target_include_directories(vmsdecnet_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# The engine core consumes the rung-1 HELLO codec + the rung-3 adjacency SM
# (vmsdecnet_routing) and, for the rung-2 NSP logical-link connection service,
# the NSP transport codec + logical-link FSM (vmsdecnet_nsp).
target_link_libraries(vmsdecnet_engine PUBLIC vmsdecnet_routing vmsdecnet_nsp)

# decnetd -- THE DAEMON. The DECnet analogue of scsd_exe: the ONLY place the raw
# datalink (scs_datalink_{open,send,recv}, the generic raw-L2 abstraction in
# src/libdatalink, written engine-agnostic for exactly this consumer) is
# touched. It ties the socketless engine core to the live datalink + a monotonic
# clock. Links ovmx_datalink for its socket/bpf open + MAC-get + send + recv
# primitives.
add_executable(decnetd_exe decnetd.c)
target_include_directories(decnetd_exe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/libdatalink/include       # scs_datalink.h
    ${CMAKE_SOURCE_DIR}/src/vmsdecnet/cterm/include   # dnet_cterm.h (--set-host-selftest)
    ${CMAKE_SOURCE_DIR}/src/libvms/include            # ovmx_identity.h (INV-1 banner SSOT)
    ${CMAKE_SOURCE_DIR}/src/vmsfs/include             # transitive header of ovmx_identity.h
)
# vmsdecnet_cterm: the CTERM terminal-service protocol (rd vms-4d2) that rides on
# the logical link -- the daemon's --set-host-selftest proves a full $ SET HOST
# terminal session over a socketpair.
# vmsdecnet_cterm_host (rd vms-f40): the CTERM HOST SESSION -- an inbound
# object-42 connect mints an RTAn: in the executive and $CREPRCs LOGINOUT.EXE
# onto it, so a remote $ SET HOST is AUTHENTICATED. It pulls in libvms (the VMS
# system services) and libvmssys (the executive interface); the daemon itself
# still contains no fork/exec/openpty (gated by
# tests/integration/test_creprc_session_primitive.sh).
target_link_libraries(decnetd_exe PRIVATE vmsdecnet_engine vmsdecnet_cterm
                                          vmsdecnet_cterm_host ovmx_datalink)
target_include_directories(decnetd_exe PRIVATE
    ${CMAKE_SOURCE_DIR}/src/libvmssys                 # vms_kif.h ($GETDVI readback)
)
set_target_properties(decnetd_exe PROPERTIES
    OUTPUT_NAME "DECNETD"
    SUFFIX ".EXE"
)
install(TARGETS decnetd_exe RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
