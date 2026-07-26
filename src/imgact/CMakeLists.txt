# IMGACT.EXE — the OVMX image activator / ELF interpreter (bead vms-913.x).
#
# Built only in OVMX_IMGACT mode. IMGACT.EXE is a freestanding, static-PIE
# (ET_DYN) binary: it links against nothing, self-relocates via its own
# _DYNAMIC (RELATIVE relocs only), and exports no symbols. This mirrors the
# standalone src/imgact/Makefile recipe proven by the vms-913.2 harness.
#
# aarch64 only until bead vms-913.11 adds the x86_64 entry stub.

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    message(FATAL_ERROR
        "OVMX_IMGACT is aarch64-only (CMAKE_SYSTEM_PROCESSOR="
        "${CMAKE_SYSTEM_PROCESSOR}). x86_64 support is bead vms-913.11.")
endif()

set(IMGACT_ARCHDIR "${CMAKE_CURRENT_SOURCE_DIR}/arch/aarch64")

add_executable(imgact
    imgact.c
    ${IMGACT_ARCHDIR}/start.S
)

target_compile_options(imgact PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
    -O2 -Wall -Wextra
    -fPIC -fvisibility=hidden -ffreestanding
    -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables
)

# Freestanding static-PIE link: no libc, no crt, custom entry, self-relocating.
# -shared yields ET_DYN (a position-independent image the kernel can load as
# PT_INTERP); the globally injected --dynamic-linker/--hash-style flags are
# harmless here (a -nostdlib -shared object gets no PT_INTERP).
target_link_options(imgact PRIVATE
    -nostdlib -nostartfiles -shared
    -Wl,-e,_start -Wl,-z,norelro -Wl,--build-id=none
)

set_target_properties(imgact PROPERTIES
    PREFIX ""
    OUTPUT_NAME "IMGACT"
    SUFFIX ".EXE"
)

install(TARGETS imgact RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
