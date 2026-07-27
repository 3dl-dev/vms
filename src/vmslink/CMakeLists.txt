# vmslink — LINK.EXE (the OVMX/VMS linker) + OVMXDUMP (image inspector).
# Bead vms-9dd, pillar vms-ade. These are host tools built by the bootstrap
# toolchain: they read ELF .o and write OVMX shareable images (ELF ET_DYN +
# .vms$sv symbol vector). They are not themselves VMS images.

add_executable(vmslink link.c)
target_include_directories(vmslink PRIVATE include)
set_target_properties(vmslink PROPERTIES OUTPUT_NAME "LINK" SUFFIX ".EXE")
install(TARGETS vmslink RUNTIME DESTINATION ${VMS_SYSTEM_DIR})

add_executable(vmsdump dump_image.c)
target_include_directories(vmsdump PRIVATE include)
set_target_properties(vmsdump PROPERTIES OUTPUT_NAME "OVMXDUMP" SUFFIX "")
install(TARGETS vmsdump RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
