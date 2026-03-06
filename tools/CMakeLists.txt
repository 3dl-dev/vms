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

target_link_libraries(vms_mail PRIVATE vmsfs vmslnm)

set_target_properties(vms_mail PROPERTIES
    OUTPUT_NAME "MAIL"
    SUFFIX ".EXE"
)
install(TARGETS vms_mail RUNTIME DESTINATION ${VMS_SYSTEM_DIR})

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
)

target_compile_definitions(vms_sysgen PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_compile_options(vms_sysgen PRIVATE
    -Wall -Wextra
)

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
)

target_compile_definitions(vms_sysman PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_compile_options(vms_sysman PRIVATE
    -Wall -Wextra
)

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
