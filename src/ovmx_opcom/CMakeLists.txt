# OPCOM.EXE -- the operator communication process (rd vms-798).
#
# Created by PID 1 as a DETACHED SYSTEM process named OPCOM: the $SNDOPR
# mailbox, OPERATOR.LOG, and the broadcast to the enabled operator terminal
# (OPA0:). On Linux it also carries the post-startup kernel-log intake
# (opcom_kmsg.c, rd vms-394e) and therefore the same strong RMS anchors
# PROVISION.EXE carries (provision_rms_bind.c, vms-aac) so rms_textfile's weak
# seam binds in a static link. NetBSD's PID-1 /dev/klog reader keeps filing the
# kernel log itself.
add_executable(ovmx_opcom
    ovmx_opcom.c
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(ovmx_opcom PRIVATE
        ${CMAKE_SOURCE_DIR}/src/ovmx_init/opcom_kmsg.c
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
