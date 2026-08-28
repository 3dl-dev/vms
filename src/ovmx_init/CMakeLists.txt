# ovmx_init - Boot orchestrator
# Links vmsprocess (PCB), vmsfs (device table + path translation),
# and vmslnm (logical names) for VMS-native boot sequence.

# Boot-plumbing substrate seam (vms-28f): pick the ovmx_boot.h backend for the
# substrate this build targets. ONE ovmx_init.c compiles against either backend
# with no #ifdef fork of the boot logic (INV-DRIFT). The Linux backend carries
# the /dev/kmsg -> OPERATOR.LOG bridge (opcom_kmsg.c); the NetBSD backend
# (vms-f2e) carries its own /dev/klog twin, so opcom_kmsg.c is Linux-only.
# On the host (Linux) this resolves to exactly the pre-seam file set.
if(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    set(OVMX_BOOT_BACKEND_SRC ovmx_boot_netbsd.c)
else()
    set(OVMX_BOOT_BACKEND_SRC ovmx_boot_linux.c opcom_kmsg.c)
endif()

# ACP-read bootstrap bridge (vms-5f0): PID 1 stages the first-hop execve'd
# images off the genuine ODS-2 boot volume THROUGH the executive ACP. The ACP
# file-access walk is the SAME imgact_acp.c IMGACT.EXE and the QEMU test use --
# ovmx_boot_acp_read.c only backs its three host primitives with libc.
#
# EVERY RUNTIME SUBSTRATE (vms-329, the coupled VAX cutover). This used to be
# Linux-only while the NetBSD-vax boot path still VFS-mounted SYS$DISK. It no
# longer does: ovmx_boot_mount_system_disk_native() ACP-mounts the boot unit on
# both substrates, which retires the vmsfs.ko VFS mount (NetBSD's spec_vnops
# allows exactly ONE open of the block device, so the two mounts cannot
# coexist), and with no VFS mount PID 1 has no POSIX way to reach the first-hop
# images at all -- the bridge IS the boot path here, not an enhancement.
# vms-8e8f proved this whole TU set cross-compiles ILP32-clean for elf32-vax
# and gates it standing (tools/cross-vax/build-acp-read-audit-vax.sh).
#
# Keep this set in lockstep with tools/cross-vax/build-ovmx-init-vax.sh, which
# builds the SHIPPED VAX PID 1 (this target is the ovmx-images parity artifact);
# the two recipes linking a different TU set is exactly the drift vms-8e8f's
# handoff flagged.
set(OVMX_BOOT_BRIDGE_SRC
    ovmx_boot_acp_read.c
    # vms-46c: the conversational SYSBOOT> path's OVMXVMSSYS.PAR read/write
    # over the executive Files-11 ACP (the strong ovmx_sysgen_acp_* seam for
    # PID 1, backed by imgact_acp.c -- no RMS pulled into the static image).
    ovmx_boot_sysgen_acp.c
    ${CMAKE_SOURCE_DIR}/src/imgact/imgact_acp.c)

add_executable(ovmx_init
    ovmx_init.c
    ${OVMX_BOOT_BACKEND_SRC}
    sysboot.c
    ${OVMX_BOOT_BRIDGE_SRC}
)

target_include_directories(ovmx_init PRIVATE
    ${VMS_PROCESS_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    # vms_kif.h: PID 1's identity is established THROUGH the executive
    # (vms-2b8), and the login child takes the session's channel to the
    # console and asks the executive to record it as the job's terminal
    # (vms-d0b).
    ${CMAKE_SOURCE_DIR}/src/libvmssys
    # imgact_acp.h (the ACP file-access walk) + vms_ioctl.h (the ACP ioctl arg
    # structs / request numbers) for the vms-5f0 boot-image staging bridge.
    ${CMAKE_SOURCE_DIR}/src/imgact
    ${CMAKE_SOURCE_DIR}/src/kernel
)

# Feature-test macros. On Linux (glibc) POSIX 2008 + _DEFAULT_SOURCE expose
# the BSD/sysv extensions ovmx_boot_linux.c/sysboot.c use. On NetBSD the same
# surface (u_int, S_IFCHR/mknod, devmajor_t/NODEVMAJOR/getdevmajor, mount(2)
# flags, reboot(2)/RB_*, sethostname) is gated behind _NETBSD_SOURCE, and
# defining _POSIX_C_SOURCE would TURN IT OFF -- so the netbsd-vax cross
# selects the native widest namespace instead, same pattern as
# src/vmsdcl/CMakeLists.txt (vms-1cb2).
if(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    target_compile_definitions(ovmx_init PRIVATE _NETBSD_SOURCE)
else()
    target_compile_definitions(ovmx_init PRIVATE
        _POSIX_C_SOURCE=200809L
        _DEFAULT_SOURCE
        # vms-5f0: names the SUBSTRATE, not the link set. Only two things still
        # key off it: the Linux-only /dev/kmsg OPCOM bridge (opcom_kmsg.c) and
        # the PT_INTERP staging of IMGACT.EXE, which exists only on the
        # IMGACT-activated Linux path (netbsd-vax uses ld.elf_so -- Decision A,
        # vms-42d -- and ships no IMGACT.EXE).
        OVMX_BOOT_LINUX=1
    )
endif()

# vms-d5d/vms-329: the boot code reaches SYS$DISK via the executive ACP
# (vms_kif_acp_*) on every runtime substrate, and the ACP-read bridge TUs
# (OVMX_BOOT_BRIDGE_SRC above) are in EVERY link of this target. ovmx_init has
# no standalone/unit build (only a substrate split), so both capability macros
# are unconditional here:
#   OVMX_HAVE_ACP        -- $MOUNT/read SYS$DISK over the executive ACP.
#   OVMX_BOOT_ACP_BRIDGE -- "the bridge TUs are in THIS link", which is what
#                           ovmx_init.c's stage_boot_images() and the
#                           installed-system probe key off (they are
#                           substrate-neutral C, so they must key off the LINK
#                           SET, never the substrate).
target_compile_definitions(ovmx_init PRIVATE OVMX_HAVE_ACP OVMX_BOOT_ACP_BRIDGE=1)

if(OVMX_IMGACT)
    # Static PID-1 bootstrap: link the static twins, not the shareable images.
    target_link_libraries(ovmx_init PRIVATE vmsprocess_static vmsfs_static vmslnm_static vmssys pthread)
else()
    target_link_libraries(ovmx_init PRIVATE vmsprocess vmsfs vmslnm vmssys pthread)
endif()

set_target_properties(ovmx_init PROPERTIES
    OUTPUT_NAME "STARTUP"
    SUFFIX ".EXE"
)

# STARTUP.EXE is PID 1: it must run before IMGACT.EXE exists in the process
# tree, so it is always statically linked — even in OVMX_IMGACT (dynamic) mode,
# where -static overrides the global PT_INTERP/dynamic-linker link flags.
if(OVMX_IMGACT)
    target_link_options(ovmx_init PRIVATE -static)
endif()
install(TARGETS ovmx_init RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
