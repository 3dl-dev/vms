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
# ovmx_boot_acp_read.c only backs its three host primitives with libc. LINUX
# SUBSTRATE ONLY: the atomic flip of SYS$DISK to a genuine ODS-2 ACP volume is
# the Linux flip; the NetBSD-vax boot path (ovmx_boot_netbsd.c) is flipped
# separately by vms-d5d and is deliberately left untouched here, so the bridge
# sources compile in only on the Linux backend and their call sites in
# ovmx_init.c are guarded by OVMX_BOOT_LINUX (set below).
if(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    set(OVMX_BOOT_BRIDGE_SRC)
else()
    set(OVMX_BOOT_BRIDGE_SRC
        ovmx_boot_acp_read.c
        ${CMAKE_SOURCE_DIR}/src/imgact/imgact_acp.c)
endif()

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
        # vms-5f0: the ACP-read bootstrap bridge (genuine ODS-2 SYS$DISK flip)
        # is the LINUX flip. Its call sites in ovmx_init.c are guarded by this
        # macro so the shared ovmx_init.c keeps compiling on the NetBSD-vax
        # backend with its current boot behaviour, which vms-d5d flips.
        OVMX_BOOT_LINUX=1
    )
endif()

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
