# tools/ - VMS login and help utilities

add_executable(vms_login
    vms_login.c
)

target_include_directories(vms_login PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_PROCESS_INCLUDE_DIR}
)

target_compile_definitions(vms_login PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
    OVMX_BIN_DIR="${CMAKE_INSTALL_PREFIX}/bin"
)

target_link_libraries(vms_login PRIVATE vms vmsprocess pthread)

install(TARGETS vms_login RUNTIME DESTINATION bin)

add_executable(vms_help
    vms_help.c
)

target_include_directories(vms_help PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
)

target_compile_definitions(vms_help PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

install(TARGETS vms_help RUNTIME DESTINATION bin)

add_executable(vms_ssh_auth
    vms_ssh_auth.c
)

target_include_directories(vms_ssh_auth PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_PROCESS_INCLUDE_DIR}
)

target_compile_definitions(vms_ssh_auth PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_link_libraries(vms_ssh_auth PRIVATE vms)

install(TARGETS vms_ssh_auth RUNTIME DESTINATION bin)

add_executable(vms_authorize
    vms_authorize.c
)

target_include_directories(vms_authorize PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_PROCESS_INCLUDE_DIR}
)

target_compile_definitions(vms_authorize PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_compile_options(vms_authorize PRIVATE
    -Wall -Wextra
)

target_link_libraries(vms_authorize PRIVATE vms)

install(TARGETS vms_authorize RUNTIME DESTINATION bin)

add_executable(vms_mail
    vms_mail.c
)

target_include_directories(vms_mail PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VMS_INCLUDE_DIR}
)

target_compile_definitions(vms_mail PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

install(TARGETS vms_mail RUNTIME DESTINATION bin)

add_executable(vms_monitor
    vms_monitor.c
)

target_compile_definitions(vms_monitor PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_compile_options(vms_monitor PRIVATE
    -Wall -Wextra
)

install(TARGETS vms_monitor RUNTIME DESTINATION bin)
