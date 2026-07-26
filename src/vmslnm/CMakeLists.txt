# vmslnm - Logical Name Manager

set(VMSLNM_SOURCES
    lnm_table.c
    lnm_translate.c
    lnm_client.c
    lnm_defaults.c
)

add_library(vmslnm ${OVMX_LIB_TYPE} ${VMSLNM_SOURCES})

target_include_directories(vmslnm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VMS_INCLUDE_DIR}
)

target_link_libraries(vmslnm PUBLIC pthread)

set_target_properties(vmslnm PROPERTIES
    PREFIX ""
    OUTPUT_NAME "LIBVMSLNM\$SHR"
    SUFFIX ".EXE"
)
install(TARGETS vmslnm LIBRARY DESTINATION ${VMS_SHARE_DIR})

# Static twin for the static PID-1 bootstrap (STARTUP.EXE). IMGACT mode only.
if(OVMX_IMGACT)
    add_library(vmslnm_static STATIC ${VMSLNM_SOURCES})
    target_include_directories(vmslnm_static PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${VMS_INCLUDE_DIR}
    )
    target_link_libraries(vmslnm_static PUBLIC pthread)
endif()

# Daemon executable
add_executable(lnm_daemon_exe lnm_daemon.c)
target_include_directories(lnm_daemon_exe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
)
target_link_libraries(lnm_daemon_exe PRIVATE vmslnm vmsfs pthread)
target_compile_definitions(lnm_daemon_exe PRIVATE LNM_DAEMON_MAIN)
set_target_properties(lnm_daemon_exe PROPERTIES
    OUTPUT_NAME "VMSLNMD"
    SUFFIX ".EXE"
)

install(TARGETS lnm_daemon_exe RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
