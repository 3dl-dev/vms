add_executable(vmssshd
    vmssshd.c
    term_map.c
    cred_drop.c
    ssh_ident.c
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/dcl_terminal.c
)

target_include_directories(vmssshd PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_PROCESS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/include
    # kernel-interface client (vms-2b8): src/libvmssys/vms_kif.h, for
    # vms_kif_setident() -- see the comment at its call site in vmssshd.c.
    ${CMAKE_SOURCE_DIR}/src/libvmssys
)

target_link_libraries(vmssshd PRIVATE
    ssh       # libssh server-side SSH protocol
    vms       # VMS runtime (includes SYSUAF)
    vmsrms    # SYSUAF auth reads via RMS $OPEN/$GET over the ODS-2 ACP (vms-274)
    vmsfs     # filespec translation
    vmslnm    # logical name resolution
    vmsprocess
    pthread
    util      # openpty()
)

set_target_properties(vmssshd PROPERTIES
    OUTPUT_NAME "VMSSSHD"
    SUFFIX ".EXE"
)
install(TARGETS vmssshd RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
