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

add_executable(ovmx_init
    ovmx_init.c
    ${OVMX_BOOT_BACKEND_SRC}
    sysboot.c
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
    )
endif()

# vmsfs_volume (vms-351, epic vms-5eb "B2"): the userspace mounted-volume table
# PID 1 registers the boot device into at boot. A STATIC module (links the ods2
# reader sublibrary) in BOTH modes -- ovmx_init/STARTUP.EXE is always statically
# linked (PID 1 runs before IMGACT.EXE exists), so it links this the same way
# regardless of OVMX_IMGACT. Kept out of LIBVMSFS$SHR on purpose (see
# src/vmsfs/CMakeLists.txt): folding ods2_bdev_* into the shareable's strict
# symbol-vector link would drag the ODS-2-flip deps in prematurely.
if(OVMX_IMGACT)
    # Static PID-1 bootstrap: link the static twins, not the shareable images.
    target_link_libraries(ovmx_init PRIVATE vmsprocess_static vmsfs_static vmslnm_static vmssys vmsfs_volume pthread)
else()
    target_link_libraries(ovmx_init PRIVATE vmsprocess vmsfs vmslnm vmssys vmsfs_volume pthread)
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
