# ovmx_init - Boot orchestrator
# Now links vmsprocess for PCB initialization at boot.

add_executable(ovmx_init
    ovmx_init.c
)

target_include_directories(ovmx_init PRIVATE
    ${VMS_PROCESS_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)

target_compile_definitions(ovmx_init PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
    OVMX_BIN_DIR="${CMAKE_INSTALL_PREFIX}/bin"
    OVMX_SBIN_DIR="${CMAKE_INSTALL_PREFIX}/sbin"
)

target_link_libraries(ovmx_init PRIVATE vmsprocess pthread)

install(TARGETS ovmx_init RUNTIME DESTINATION bin)
