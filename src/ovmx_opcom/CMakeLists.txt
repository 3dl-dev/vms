# OPCOM.EXE -- the operator communication process (rd vms-798).
#
# Created by PID 1 as a DETACHED SYSTEM process named OPCOM: the $SNDOPR
# mailbox, OPERATOR.LOG, and the broadcast to the enabled operator terminal
# (OPA0:). It writes OPERATOR.LOG through rms_textfile, so on Linux it carries
# the strong RMS anchors PROVISION.EXE carries (provision_rms_bind.c, vms-aac)
# for rms_textfile's weak seam to bind in a static link. The post-startup
# kernel-log intake (rd vms-394e) is not in OPCOM yet: held behind rd vms-3ea /
# vms-326 (see docs/design-opcom-executive-logging.md sec 9).
add_executable(ovmx_opcom
    ovmx_opcom.c
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(ovmx_opcom PRIVATE
        ${CMAKE_SOURCE_DIR}/src/ovmx_provision/provision_rms_bind.c)
endif()

target_include_directories(ovmx_opcom PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_PROCESS_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/src/libvmssys
    ${CMAKE_SOURCE_DIR}/src/ovmx_init
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    target_compile_definitions(ovmx_opcom PRIVATE _NETBSD_SOURCE)
else()
    target_compile_definitions(ovmx_opcom PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
endif()

target_link_libraries(ovmx_opcom PRIVATE vms vmsrms vmsfs vmslnm vmsprocess pthread)

set_target_properties(ovmx_opcom PROPERTIES
    OUTPUT_NAME "OPCOM"
    SUFFIX ".EXE"
)
install(TARGETS ovmx_opcom RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
