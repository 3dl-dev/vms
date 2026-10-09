# NATIVEACT.EXE -- the OpenVMS VAX native image activator on OVMX/VAX
# (rd vms-b869). An ordinary OVMX/VAX image (Decision A): the OVMX routines a
# native image calls through the P1 system service vector and the run-time
# library transfer vectors are linked into it, and it reads images through the
# executive ACP with the same imgact_acp.c IMGACT.EXE uses.
add_executable(vms_nativeact nativeact.c ${CMAKE_SOURCE_DIR}/src/imgact/imgact_acp.c)
target_include_directories(vms_nativeact PRIVATE
    ${CMAKE_SOURCE_DIR}/src/imgact
    ${CMAKE_SOURCE_DIR}/src/kernel
    ${VMS_INCLUDE_DIR}
)
target_compile_definitions(vms_nativeact PRIVATE _NETBSD_SOURCE _DEFAULT_SOURCE)
target_link_libraries(vms_nativeact PRIVATE vms vmsrms vmsfs vmslnm vmsprocess pthread)
set_target_properties(vms_nativeact PROPERTIES OUTPUT_NAME "NATIVEACT" SUFFIX ".EXE")
# The native image owns low P0 (a VAX image is linked at 0x200 upward and its
# shareables are mapped below 0x40000000): put the activator's own text high
# in P0, out of the way.
target_link_options(vms_nativeact PRIVATE -Wl,-Ttext-segment=0x30000000)
install(TARGETS vms_nativeact RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
