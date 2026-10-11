# src/libdatalink - the raw-L2 Ethernet datalink abstraction (rd vms-838).
#
# RELOCATED HERE BY FC-P3.9. This library was `vmsscs_datalink`, built inside
# src/vmsscs for SCSD, the userspace SCS cluster daemon. That daemon is
# RETIRED with the rest of the userspace SCS strawman (the operator's
# 2026-09-02 clustering reset): the cluster port, SCS and the connection
# manager are executive-resident, so nothing on the cluster path opens a raw
# socket from userspace any more.
#
# The library survives its original caller because it was deliberately written
# engine-agnostic for a SECOND consumer that now exists: DECnet Phase IV
# (src/vmsdecnet/engine/decnetd.c, rd vms-30e). Deleting it with the daemon
# would have taken DECnet's transport with it -- which is exactly the "verify
# no live code dependency first" check the retirement required.
#
# Builds unconditionally, with no dependency on the OVMX library chain: it is
# plain POSIX (Linux AF_PACKET) / NetBSD bpf(4), the same footing it had
# before.
add_library(ovmx_datalink STATIC ovmx_datalink.c)
target_include_directories(ovmx_datalink PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# THE EXECUTIVE BACKEND (rd vms-1f69). ovmx_datalink.c carries TWO Linux
# backends selected at COMPILE time by SCS_DATALINK_VIA_EXECUTIVE: the AF_PACKET
# PROBE (a userspace raw socket -- needs CAP_NET_RAW, a lab/RE instrument only)
# and the EXECUTIVE backend (the raw L2 wire goes through /dev/vms
# VMS_IOCTL_L2_*, the kernel owns the socket, the open is gated on the VMS PHY_IO
# privilege -- no Linux capability). A booted OVMX process has no CAP_NET_RAW,
# so the booted NETACP (DECNETD.EXE) MUST be built with the executive backend,
# and that binary then physically contains no AF_PACKET code: there is no
# runtime fallback between the two (Rule 9 / INV-6).
#
# Both are always BUILT on Linux (ovmx_datalink = the probe, ovmx_datalink_exec
# = the executive backend) so the default dev build compiles and host-tests
# both (tests/vmsdecnet/test_datalink_exec_backend.c). Which one DECNETD.EXE
# LINKS is the OVMX_DATALINK_VIA_EXECUTIVE option, defaulting ON for the
# booted-runtime build (OVMX_STATIC on Linux: distro/Dockerfile.bootable and
# tools/cross-alpha's build-static) and OFF for the host dev build, so a lab
# probe build of DECNETD keeps AF_PACKET. Override explicitly either way with
# -DOVMX_DATALINK_VIA_EXECUTIVE=ON|OFF. NetBSD (the VAX substrate) has no
# executive L2 binding yet (src/kernel-netbsd/vms_socket_netbsd.c exec_l2_* are
# contract-only), so it keeps the bpf(4) backend and the option is forced OFF.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(OVMX_STATIC)
        set(_ovmx_dl_exec_default ON)
    else()
        set(_ovmx_dl_exec_default OFF)
    endif()
    option(OVMX_DATALINK_VIA_EXECUTIVE
           "DECNETD.EXE's raw L2 datalink goes through the executive (/dev/vms VMS_IOCTL_L2_*, PHY_IO) instead of a userspace AF_PACKET socket"
           ${_ovmx_dl_exec_default})

    add_library(ovmx_datalink_exec STATIC ovmx_datalink.c)
    target_compile_definitions(ovmx_datalink_exec PUBLIC SCS_DATALINK_VIA_EXECUTIVE)
    target_include_directories(ovmx_datalink_exec PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_include_directories(ovmx_datalink_exec PRIVATE
        ${CMAKE_SOURCE_DIR}/src/kernel                    # vms_ioctl.h / vms_l2.h
        ${CMAKE_SOURCE_DIR}/src/libvmssys                 # kif_calls.h (rd vms-bbde)
    )
    # The executive is reached through libvmssys's transport seam only.
    target_link_libraries(ovmx_datalink_exec PUBLIC vmssys)
else()
    set(OVMX_DATALINK_VIA_EXECUTIVE OFF CACHE BOOL
        "DECNETD datalink via the executive (Linux only)" FORCE)
endif()

if(OVMX_DATALINK_VIA_EXECUTIVE)
    set(OVMX_DECNETD_DATALINK_LIB ovmx_datalink_exec CACHE INTERNAL "")
else()
    set(OVMX_DECNETD_DATALINK_LIB ovmx_datalink CACHE INTERNAL "")
endif()
message(STATUS "DECNETD datalink backend: ${OVMX_DECNETD_DATALINK_LIB}")
