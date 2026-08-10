# tools/ - VMS login and help utilities

add_executable(vms_login
    vms_login.c
)

target_include_directories(vms_login PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_PROCESS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
)

target_compile_definitions(vms_login PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_link_libraries(vms_login PRIVATE vms vmsfs vmslnm vmsprocess pthread)

set_target_properties(vms_login PROPERTIES
    OUTPUT_NAME "LOGINOUT"
    SUFFIX ".EXE"
)
install(TARGETS vms_login RUNTIME DESTINATION ${VMS_SYSTEM_DIR})

add_executable(vms_help
    vms_help.c
)

target_include_directories(vms_help PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
)

target_compile_definitions(vms_help PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_link_libraries(vms_help PRIVATE vmsfs vmslnm)

set_target_properties(vms_help PROPERTIES
    OUTPUT_NAME "HELP"
    SUFFIX ".EXE"
)
install(TARGETS vms_help RUNTIME DESTINATION ${VMS_SYSTEM_DIR})

add_executable(vms_authorize
    vms_authorize.c
)

target_include_directories(vms_authorize PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_PROCESS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
)

target_compile_definitions(vms_authorize PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_compile_options(vms_authorize PRIVATE
    -Wall -Wextra
)

target_link_libraries(vms_authorize PRIVATE vms vmsfs vmslnm)

set_target_properties(vms_authorize PROPERTIES
    OUTPUT_NAME "AUTHORIZE"
    SUFFIX ".EXE"
)
install(TARGETS vms_authorize RUNTIME DESTINATION ${VMS_SYSTEM_DIR})

add_executable(vms_mail
    vms_mail.c
)

target_include_directories(vms_mail PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
)

target_compile_definitions(vms_mail PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_link_libraries(vms_mail PRIVATE vms vmsfs vmslnm)

set_target_properties(vms_mail PROPERTIES
    OUTPUT_NAME "MAIL"
    SUFFIX ".EXE"
)
install(TARGETS vms_mail RUNTIME DESTINATION ${VMS_SYSTEM_DIR})

add_executable(vms_monitor
    vms_monitor.c
)

target_include_directories(vms_monitor PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
)

target_compile_definitions(vms_monitor PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_compile_options(vms_monitor PRIVATE
    -Wall -Wextra
)

# vms-d34: pulls sysgen_params.h transitively via ovmx_identity.h, which now
# calls through to vmsfs for SYS$SYSTEM:OVMXVMSSYS.PAR version resolution.
target_link_libraries(vms_monitor PRIVATE vmsfs vmslnm)

set_target_properties(vms_monitor PROPERTIES
    OUTPUT_NAME "MONITOR"
    SUFFIX ".EXE"
)
install(TARGETS vms_monitor RUNTIME DESTINATION ${VMS_SYSTEM_DIR})

add_executable(vms_initialize
    vms_initialize.c
)

target_include_directories(vms_initialize PRIVATE
    ${CMAKE_SOURCE_DIR}/src/kernel/vmsfs
)

target_compile_definitions(vms_initialize PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_compile_options(vms_initialize PRIVATE
    -Wall -Wextra
)

set_target_properties(vms_initialize PROPERTIES
    OUTPUT_NAME "INITIALIZE"
    SUFFIX ".EXE"
)
install(TARGETS vms_initialize RUNTIME DESTINATION ${VMS_SYSTEM_DIR})

add_executable(vms_tcpip_config
    vms_tcpip_config.c
)

target_include_directories(vms_tcpip_config PRIVATE
    ${VMS_INCLUDE_DIR}
)

target_compile_definitions(vms_tcpip_config PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_compile_options(vms_tcpip_config PRIVATE
    -Wall -Wextra
)

set_target_properties(vms_tcpip_config PROPERTIES
    OUTPUT_NAME "TCPIP\$CONFIG"
    SUFFIX ".EXE"
)
install(TARGETS vms_tcpip_config RUNTIME DESTINATION ${VMS_SYSTEM_DIR})

add_executable(vms_sysgen
    vms_sysgen.c
)

target_include_directories(vms_sysgen PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
)

target_compile_definitions(vms_sysgen PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_compile_options(vms_sysgen PRIVATE
    -Wall -Wextra
)

# vms-d34: USE/WRITE CURRENT now resolve SYS$SYSTEM:OVMXVMSSYS.PAR through
# vmsfs (device/logical translation + real file-version creation), so the
# tool needs the vmsfs link it didn't before.
target_link_libraries(vms_sysgen PRIVATE vmsfs vmslnm)

set_target_properties(vms_sysgen PROPERTIES
    OUTPUT_NAME "SYSGEN"
    SUFFIX ".EXE"
)
install(TARGETS vms_sysgen RUNTIME DESTINATION ${VMS_SYSTEM_DIR})

add_executable(vms_sysman
    vms_sysman.c
)

target_include_directories(vms_sysman PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
)

target_compile_definitions(vms_sysman PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_compile_options(vms_sysman PRIVATE
    -Wall -Wextra
)

# vms-d34: includes sysgen_params.h directly, which now calls through to
# vmsfs for SYS$SYSTEM:OVMXVMSSYS.PAR version resolution.
target_link_libraries(vms_sysman PRIVATE vmsfs vmslnm)

set_target_properties(vms_sysman PROPERTIES
    OUTPUT_NAME "SYSMAN"
    SUFFIX ".EXE"
)
install(TARGETS vms_sysman RUNTIME DESTINATION ${VMS_SYSTEM_DIR})

add_executable(vms_analyze
    vms_analyze.c
)

target_include_directories(vms_analyze PRIVATE
    ${CMAKE_SOURCE_DIR}/src/kernel/vmsfs
)

target_compile_definitions(vms_analyze PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_compile_options(vms_analyze PRIVATE
    -Wall -Wextra
)

set_target_properties(vms_analyze PROPERTIES
    OUTPUT_NAME "ANALYZE"
    SUFFIX ".EXE"
)
install(TARGETS vms_analyze RUNTIME DESTINATION ${VMS_SYSTEM_DIR})

# vmsfs_master - VMSFS media-mastering build tool (host tooling).
#
# Populates a VMSFS/ODS-2 image from a source directory tree so vms-8ab can
# master dist/ovmx-distrib.img.  Per docs/design-vms-faithful-install.md §3.3
# this is FACTORY tooling: it is NOT installed onto the media, keeps a plain
# host name (no .EXE, no VMS name), and is never run at boot. Shares the single
# format description in src/kernel/vmsfs/vmsfs_ondisk.h with INITIALIZE.EXE and
# vmsfs.ko.
add_executable(vmsfs_master
    vmsfs_master.c
)

target_include_directories(vmsfs_master PRIVATE
    ${CMAKE_SOURCE_DIR}/src/kernel/vmsfs
)

target_compile_definitions(vmsfs_master PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_compile_options(vmsfs_master PRIVATE
    -Wall -Wextra
)

set_target_properties(vmsfs_master PROPERTIES
    OUTPUT_NAME "vmsfs_master"
)

# ovmx_kit_pack - OVMX product kit packer (vms-0b6, factory BUILD tooling).
#
# Packs a staged SYSEXE/SYSLIB/SYSMGR/SYSHLP tree into an OVMX kit file (the
# OVMX-invented, Rule-8-labeled container -- src/libvms/include/
# ovmx_kit_format.h) and inspects/extracts one. Per
# docs/design-vms-faithful-install.md sec 3.3 policy (same class as
# vmsfs_master above): never installed onto the media, plain host name (no
# .EXE, no VMS name), never run at boot. Consuming a kit at install time
# (PRODUCT INSTALL) is vms-df9's job.
add_executable(ovmx_kit_pack
    ovmx_kit_pack.c
)

target_include_directories(ovmx_kit_pack PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/src/product
)

target_compile_definitions(ovmx_kit_pack PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_compile_options(ovmx_kit_pack PRIVATE
    -Wall -Wextra
)

# vms-d34: pulls sysgen_params.h transitively via ovmx_identity.h, which now
# calls through to vmsfs for SYS$SYSTEM:OVMXVMSSYS.PAR version resolution.
# ovmx_kit_reader (vms-df9): the shared kit open/validate/read module list
# and extract now call instead of hand-rolling their own second parser.
target_link_libraries(ovmx_kit_pack PRIVATE vmsfs vmslnm ovmx_kit_reader)

set_target_properties(ovmx_kit_pack PROPERTIES
    OUTPUT_NAME "ovmx_kit_pack"
)

# vms_mount_helper - setuid-root privileged mount(2)/umount(2) helper for
# DCL's MOUNT/DISMOUNT (vms-651). See tools/vms_mount_helper.c's header for
# why this exists: LOGINOUT drops every VMS session to its SYSUAF UIC (no
# Linux capability survives that), and there is no exported kernel-module
# API that can graft a mount onto a path on a process's behalf, so this is
# the one Unix-classic escape hatch (setuid-root, re-derives its own
# authorization from the executive, never trusts its caller). NOT a VMS
# image: no ".EXE" suffix, not installed under SYS$SYSTEM:, invoked by an
# absolute Linux path DCL never shows a user.
add_executable(vms_mount_helper
    vms_mount_helper.c
)

target_include_directories(vms_mount_helper PRIVATE
    ${VMS_INCLUDE_DIR}
)

target_compile_definitions(vms_mount_helper PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_compile_options(vms_mount_helper PRIVATE
    -Wall -Wextra
)

target_link_libraries(vms_mount_helper PRIVATE vms)

set_target_properties(vms_mount_helper PROPERTIES
    OUTPUT_NAME "vms_mount_helper"
)
