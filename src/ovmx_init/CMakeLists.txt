# ovmx_init - Boot orchestrator
# Links vmsprocess (PCB), vmsfs (device table + path translation),
# and vmslnm (logical names) for VMS-native boot sequence.

add_executable(ovmx_init
    ovmx_init.c
)

target_include_directories(ovmx_init PRIVATE
    ${VMS_PROCESS_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
)

target_compile_definitions(ovmx_init PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_link_libraries(ovmx_init PRIVATE vmsprocess vmsfs vmslnm pthread)

set_target_properties(ovmx_init PROPERTIES
    OUTPUT_NAME "STARTUP"
    SUFFIX ".EXE"
)
install(TARGETS ovmx_init RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
