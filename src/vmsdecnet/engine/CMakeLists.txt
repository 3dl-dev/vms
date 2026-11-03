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

# DATALINK BACKEND (rd vms-1f69): ${OVMX_DECNETD_DATALINK_LIB} is ovmx_datalink_exec
# (the executive L2 path, /dev/vms + PHY_IO, no CAP_NET_RAW) for the booted
# runtime build and ovmx_datalink (the AF_PACKET probe) for the host/lab build --
# see src/libdatalink/CMakeLists.txt OVMX_DATALINK_VIA_EXECUTIVE. Exactly one is
# linked; there is no runtime fallback between them.
# decnetd -- THE DAEMON. The DECnet analogue of scsd_exe: the ONLY place the raw
# datalink (scs_datalink_{open,send,recv}, the generic raw-L2 abstraction in
# src/libdatalink, written engine-agnostic for exactly this consumer) is
# touched. It ties the socketless engine core to the live datalink + a monotonic
# clock. Links ovmx_datalink for its socket/bpf open + MAC-get + send + recv
# primitives.
# decnetd_rms_bind.c: force-bind anchor (vms-8c2). DECNETD.EXE's FAL half reaches
# the RMS record services + the binary SYSUAF engine through LIBVMS's `#pragma
# weak` seam; under a static link a weak-only reference does not extract the
# vmsrms archive member, so --fal-accept-test's real auth + real file I/O failed
# with sys$open/ovmx_sysuaf_read_user NULL. The anchor takes their address so the
# static linker pulls rms_core.o + sysuaf_live.o (the same weak-seam trap
# provision_rms_bind.c / loginout_rms_bind.c / tests/qemu/rms_acp_bind.c close).
# sysoutput_rms.c (rd vms-c89): NETACP is created DETACHED with
# /OUTPUT=/ERROR=SYS$MANAGER:NETACP.LOG; this image-start routine $CREATEs that
# file through RMS and files each line DECNETD writes there (vmsrms below is in
# the link closure, decnetd_rms_bind.c binds it; Threads for its reader).
# NETACP.EXE (rd vms-0a7 S3): the network ancillary control process. NCP SET
# EXECUTOR STATE ON $CREPRCs it detached with no parameters; it takes no argv
# (decnetd.c NETACP_IMAGE entry). It writes SYS$MANAGER:NETACP.LOG through RMS
# itself (netacp_log in decnetd.c) -- no sysoutput_rms reader thread -- and
# links no Threads: the libc ratchet (tests/dcl/test_no_libc_io.sh) and the
# booted syscall audit hold it to no thread, no poll(), no pipe.
#
# decnetd.c is ONE translation unit compiled per image: NETACP_IMAGE for
# NETACP.EXE, DECNETD_SETHOST_IMAGE for the DECNETD.EXE $ SET HOST client
# (retired by RTPAD.EXE, rd vms-0a7 S2), and with neither by the test image
# tests/vmsdecnet/netacp_test.c, which holds every self-test.
#
# decnetd_rms_bind.c: force-bind anchor (vms-8c2) -- the RMS record services +
# the binary SYSUAF engine are reached through LIBVMS's `#pragma weak` seam; a
# weak-only reference does not extract the vmsrms archive member under a static
# link, so the anchor takes their address.
set(_netacp_includes
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/libdatalink/include       # scs_datalink.h
    ${CMAKE_SOURCE_DIR}/src/vmsdecnet/cterm/include   # dnet_cterm.h
    ${CMAKE_SOURCE_DIR}/src/vmsdecnet/dap/include     # dnet_dap.h
    ${CMAKE_SOURCE_DIR}/src/vmsdecnet/fal/include     # dnet_fal.h
    ${CMAKE_SOURCE_DIR}/src/vmsdecnet/broker/include  # dnet_broker.h
    ${CMAKE_SOURCE_DIR}/src/libvms/include            # ovmx_identity.h, rms_textfile.h, ssdef.h
    ${CMAKE_SOURCE_DIR}/src/vmsfs/include             # transitive header of ovmx_identity.h
    ${CMAKE_SOURCE_DIR}/src/libvmssys                 # vms_kif.h
    ${CMAKE_SOURCE_DIR}/src/vmsrms                    # rms_io.h
    ${CMAKE_SOURCE_DIR}/src/vmsrms/include            # rms/*.h, transitive of rms_io.h
    CACHE INTERNAL "")
set(_netacp_libs vmsdecnet_engine vmsdecnet_cterm vmsdecnet_cterm_host
                 vmsdecnet_fal vmsdecnet_mail vmsdecnet_dap vmsdecnet_broker
                 vmsdecnet_ncpstore ${OVMX_DECNETD_DATALINK_LIB} vmsrms
    CACHE INTERNAL "")

add_executable(netacp_exe decnetd.c decnetd_rms_bind.c)
target_compile_definitions(netacp_exe PRIVATE NETACP_IMAGE)
target_include_directories(netacp_exe PRIVATE ${_netacp_includes})
target_link_libraries(netacp_exe PRIVATE ${_netacp_libs})
set_target_properties(netacp_exe PROPERTIES OUTPUT_NAME "NETACP" SUFFIX ".EXE")
install(TARGETS netacp_exe RUNTIME DESTINATION ${VMS_SYSTEM_DIR})

# DECNETD.EXE: the $ SET HOST client only (--set-host/--user), until S2.
add_executable(decnetd_exe decnetd.c decnetd_rms_bind.c)
target_compile_definitions(decnetd_exe PRIVATE DECNETD_SETHOST_IMAGE)
target_include_directories(decnetd_exe PRIVATE ${_netacp_includes})
target_link_libraries(decnetd_exe PRIVATE ${_netacp_libs})
set_target_properties(decnetd_exe PROPERTIES OUTPUT_NAME "DECNETD" SUFFIX ".EXE")
install(TARGETS decnetd_exe RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
