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

# DECC$SHR.EXE — the OVMX C run-time library shareable (bead vms-61f.1) — is NOT
# a host CMake target: it links the aarch64 musl libc.a + libgcc.a via LINK.EXE,
# so it only builds inside the arm64 musl container (this host is glibc). The
# build recipe is `mk_decc_shr.sh` (reusable by vms-61f.2 runtime init); the
# producer proof is `test/run_decc_shr.sh`, gated in CI by the `decc-shr` job.
