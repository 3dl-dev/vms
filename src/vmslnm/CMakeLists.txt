# vmslnm - Logical Name Manager

add_library(vmslnm ${OVMX_LIB_TYPE}
    lnm_table.c
    lnm_translate.c
    lnm_client.c
    lnm_defaults.c
)

target_include_directories(vmslnm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VMS_INCLUDE_DIR}
)

target_link_libraries(vmslnm PUBLIC pthread)

if(NOT OVMX_STATIC)
    set_target_properties(vmslnm PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION 0
    )
endif()

install(TARGETS vmslnm LIBRARY DESTINATION lib)

# Daemon executable
add_executable(lnm_daemon_exe lnm_daemon.c)
target_include_directories(lnm_daemon_exe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VMS_INCLUDE_DIR}
)
target_link_libraries(lnm_daemon_exe PRIVATE vmslnm pthread)
target_compile_definitions(lnm_daemon_exe PRIVATE LNM_DAEMON_MAIN)
set_target_properties(lnm_daemon_exe PROPERTIES OUTPUT_NAME vmslnmd)

install(TARGETS lnm_daemon_exe RUNTIME DESTINATION sbin)
