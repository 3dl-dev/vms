# ovmx_provision - PROVISION.EXE, the OVMX startup process (vms-9b7)
#
# An ORDINARY image linking the ORDINARY libraries. That is the point: the
# work in here used to live in PID 1, which is statically linked and therefore
# grew two hand-rolled SYSUAF parsers rather than link one. Here there is no
# such constraint, so it calls libvms's single reader and no second parse of
# SYSUAF exists anywhere in the boot path.
#
# Deliberately NOT linked with the *_static twins: this image is exec'd by
# PID 1 after the device table and the logical names are live, i.e. after
# IMGACT.EXE can activate a shareable. It has no reason to be static and
# making it so would re-create the constraint this item deleted.

# Boot-plumbing substrate seam (vms-28f), same selection ovmx_init's
# CMakeLists.txt already makes: PROVISION.EXE's fatal-halt path powers the
# machine off through ovmx_boot_power_off(), not a second direct reboot(2)
# call, so it needs the same per-substrate backend object ovmx_init links
# (vms-5d1, epic vms-8e8 -- netbsd-vax's reboot(2) has a different signature/
# flag set than Linux's, and the seam is exactly what exists to keep that
# ONE substrate difference out of the shared .c source).
if(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    set(OVMX_PROVISION_BOOT_BACKEND_SRC ${CMAKE_SOURCE_DIR}/src/ovmx_init/ovmx_boot_netbsd.c)
else()
    set(OVMX_PROVISION_BOOT_BACKEND_SRC ${CMAKE_SOURCE_DIR}/src/ovmx_init/ovmx_boot_linux.c ${CMAKE_SOURCE_DIR}/src/ovmx_init/opcom_kmsg.c)
endif()

# vms-aac: force the RMS record services (sys$open/$create/$put/...) into
# PROVISION.EXE's static link closure so rms_textfile.c's weak seam binds and
# opcom_kmsg_seed_operator_log() can $CREATE/$PUT SYS$MANAGER:OPERATOR.LOG over
# the ACP. Linux only -- opcom_kmsg.c (and the seed) compile in only there.
if(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    set(OVMX_PROVISION_RMS_BIND_SRC)
else()
    set(OVMX_PROVISION_RMS_BIND_SRC provision_rms_bind.c)
endif()

add_executable(ovmx_provision
    ovmx_provision.c
    ${OVMX_PROVISION_BOOT_BACKEND_SRC}
    ${OVMX_PROVISION_RMS_BIND_SRC}
)

target_include_directories(ovmx_provision PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_PROCESS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/src/libvmssys
    ${CMAKE_SOURCE_DIR}/src/ovmx_init
)

# Feature-test macros. On Linux (glibc) POSIX 2008 + _DEFAULT_SOURCE expose
# the BSD/sysv extensions ovmx_boot_linux.c uses. On NetBSD the same surface
# (u_int, S_IFCHR/mknod, devmajor_t/NODEVMAJOR/getdevmajor, mount(2) flags,
# reboot(2)/RB_*) that ovmx_boot_netbsd.c needs is gated behind
# _NETBSD_SOURCE, and defining _POSIX_C_SOURCE would TURN IT OFF -- same
# seam src/ovmx_init/CMakeLists.txt already carries (vms-f2e) for the SAME
# shared ovmx_boot_netbsd.c source this target also compiles (vms-64a: this
# target had its own hardcoded _POSIX_C_SOURCE, unlike ovmx_init's, so it
# was the one PROVISION.EXE-specific seam still blocking the vax cross build).
if(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    target_compile_definitions(ovmx_provision PRIVATE _NETBSD_SOURCE)
else()
    target_compile_definitions(ovmx_provision PRIVATE
        _POSIX_C_SOURCE=200809L
        _DEFAULT_SOURCE
    )
endif()

# vms-329: PROVISION's system-tree / account-home provisioning runs over the
# executive Files-11 ACP (IO$_MODIFY / IO$_CREATE by FID) on EVERY runtime
# substrate. Those arms used to be keyed on __linux__, which silently left the
# netbsd-vax image on POSIX lchown()/mkdir(); they are keyed on OVMX_HAVE_ACP
# now, so this target must define it (the netbsd-vax cross recipe,
# tools/cross-vax/build-provision-vax.sh, passes the same -D). PROVISION.EXE is
# a runtime image with no unit/standalone configuration, so it is unconditional
# -- exactly like src/ovmx_init/CMakeLists.txt.
target_compile_definitions(ovmx_provision PRIVATE OVMX_HAVE_ACP)

# vms-d92: PROVISION enumerates the binary $UAFDEF SYSUAF through the RMS
# Prolog-3 engine over the ODS-2 ACP (ovmx_sysuaf_enum, src/vmsrms/
# sysuaf_live.c), so it links LIBVMSRMS.
target_link_libraries(ovmx_provision PRIVATE vms vmsrms vmsfs vmslnm vmsprocess pthread)

set_target_properties(ovmx_provision PROPERTIES
    OUTPUT_NAME "PROVISION"
    SUFFIX ".EXE"
)

install(TARGETS ovmx_provision RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
