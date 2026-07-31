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
    # vms_kif.h: PID 1's identity is established THROUGH the executive.
    ${CMAKE_SOURCE_DIR}/src/libvmssys
)

target_compile_definitions(ovmx_init PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

if(OVMX_IMGACT)
    # Static PID-1 bootstrap: link the static twins, not the shareable images.
    target_link_libraries(ovmx_init PRIVATE vmsprocess_static vmsfs_static vmslnm_static vmssys pthread)
else()
    target_link_libraries(ovmx_init PRIVATE vmsprocess vmsfs vmslnm vmssys pthread)
endif()

set_target_properties(ovmx_init PROPERTIES
    OUTPUT_NAME "STARTUP"
    SUFFIX ".EXE"
)

# STARTUP.EXE is PID 1: it must run before IMGACT.EXE exists in the process
# tree, so it is always statically linked — even in OVMX_IMGACT (dynamic) mode,
# where -static overrides the global PT_INTERP/dynamic-linker link flags.
if(OVMX_IMGACT)
    target_link_options(ovmx_init PRIVATE -static)
endif()
install(TARGETS ovmx_init RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
