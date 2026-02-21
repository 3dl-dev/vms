add_executable(vmssshd vmssshd.c)

target_include_directories(vmssshd PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_PROCESS_INCLUDE_DIR}
)

target_link_libraries(vmssshd PRIVATE
    ssh       # libssh server-side SSH protocol
    vms       # VMS runtime (includes SYSUAF)
    vmsprocess
    pthread
    util      # openpty()
)

install(TARGETS vmssshd RUNTIME DESTINATION bin)
