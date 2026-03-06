add_executable(vmssshd
    vmssshd.c
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/dcl_terminal.c
)

target_include_directories(vmssshd PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_PROCESS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/include
)

target_link_libraries(vmssshd PRIVATE
    ssh       # libssh server-side SSH protocol
    vms       # VMS runtime (includes SYSUAF)
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
