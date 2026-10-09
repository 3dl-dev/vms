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
# Linked at the default P0 base (0x10000). Not higher: a VAX P0 page table
# is one contiguous array from address 0, so text at 0x30000000 would need
# 3 MB of page table per process (the first status-gate boots stopped dead
# at RUN HELLO). A native image is mapped below it; nativeact.c refuses one
# that would reach the activator's own text.
install(TARGETS vms_nativeact RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
