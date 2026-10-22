# ovmx_job_control - JOB_CONTROL.EXE, the OVMX console-session process (vms-8d2)
#
# An ORDINARY image, exec'd nowhere: it is created DETACHED, by RUN/DETACHED
# from SYS$STARTUP:JOB_CONTROL_STARTUP.COM (itself invoked from SYS$MANAGER:
# SYSTARTUP_VMS.COM), never forked directly by STARTUP.EXE (PID 1). It is
# linked and installed the same way tools/vms_login.c (LOGINOUT.EXE) is --
# an ordinary dynamically-activatable image under SYS$SYSTEM:, not one of the
# *_static twins PID 1 itself needs.

add_executable(ovmx_job_control
    ovmx_job_control.c
)
# rd vms-394e: on Linux JOB_CONTROL hosts the post-startup kernel-log intake
# (PID 1's /dev/kmsg bridge ends with startup); NetBSD's PID-1 /dev/klog reader
# keeps running, so nothing is added there.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # provision_rms_bind.c: the same strong RMS anchors PROVISION.EXE carries, so
    # rms_textfile_append_line's weak seam binds in a static link (vms-aac).
    target_sources(ovmx_job_control PRIVATE
        ${CMAKE_SOURCE_DIR}/src/ovmx_init/opcom_kmsg.c
        ${CMAKE_SOURCE_DIR}/src/ovmx_provision/provision_rms_bind.c)
    target_include_directories(ovmx_job_control PRIVATE ${CMAKE_SOURCE_DIR}/src/ovmx_init)
    target_link_libraries(ovmx_job_control PRIVATE vmsrms)
endif()

target_include_directories(ovmx_job_control PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    # vms_kif.h: JOB_CONTROL's login child takes the session's channel to
    # the console and asks the executive to record it as the job's terminal
    # (vms-d0b), the same call PID 1's login loop made before this item.
    ${CMAKE_SOURCE_DIR}/src/libvmssys
)

target_compile_definitions(ovmx_job_control PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_link_libraries(ovmx_job_control PRIVATE vms vmsfs vmslnm vmsprocess pthread)

set_target_properties(ovmx_job_control PROPERTIES
    OUTPUT_NAME "JOB_CONTROL"
    SUFFIX ".EXE"
)
install(TARGETS ovmx_job_control RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
