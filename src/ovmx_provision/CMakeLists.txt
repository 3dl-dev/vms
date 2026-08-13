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

add_executable(ovmx_provision
    ovmx_provision.c
    ${OVMX_PROVISION_BOOT_BACKEND_SRC}
)

target_include_directories(ovmx_provision PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_PROCESS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/src/libvmssys
    ${CMAKE_SOURCE_DIR}/src/ovmx_init
)

target_compile_definitions(ovmx_provision PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_link_libraries(ovmx_provision PRIVATE vms vmsfs vmslnm vmsprocess pthread)

set_target_properties(ovmx_provision PROPERTIES
    OUTPUT_NAME "PROVISION"
    SUFFIX ".EXE"
)

install(TARGETS ovmx_provision RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
