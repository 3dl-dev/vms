# IMGACT.EXE — the OVMX image activator / ELF interpreter (bead vms-913.x).
#
# Built only in OVMX_IMGACT mode. IMGACT.EXE is a freestanding, static-PIE
# (ET_DYN) binary: it links against nothing, self-relocates via its own
# _DYNAMIC (RELATIVE relocs only), and exports no symbols. This mirrors the
# standalone src/imgact/Makefile recipe proven by the vms-913.2 harness.
#
# aarch64, x86_64, and alpha are supported (beads vms-913.2, vms-913.11, and
# vms-e11); each arch provides its entry stub, TLS resolver, TP setter, and
# syscall/reloc/TLS definitions under arch/<arch>/.

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(IMGACT_ARCH "aarch64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    set(IMGACT_ARCH "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "alpha")
    set(IMGACT_ARCH "alpha")
else()
    message(FATAL_ERROR
        "OVMX_IMGACT supports aarch64, x86_64, and alpha only "
        "(CMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}).")
endif()

set(IMGACT_ARCHDIR "${CMAKE_CURRENT_SOURCE_DIR}/arch/${IMGACT_ARCH}")

# Alpha-only: the VMS standard-call trampoline (imgact_vms_transfer, vms-f60d).
# Issues a genuine Alpha standard call to a VMS-standard image's transfer
# address; the Alpha calling standard is the only one that has this class, so it
# is not built for aarch64/x86_64 (there imgact.c fails honest on such an image).
set(IMGACT_ARCH_EXTRA_SRC "")
if(IMGACT_ARCH STREQUAL "alpha")
    list(APPEND IMGACT_ARCH_EXTRA_SRC ${IMGACT_ARCHDIR}/vms_transfer.S)
endif()

# known_images.c (bead vms-913.5) is compiled directly as an extra source of
# THIS freestanding target (not the hosted `known_images` CMake library) so
# it picks up the same -ffreestanding/-nostdlib flags below; imgact.c
# provides the libc shim (open/fstat/mmap/munmap/close/strncpy/strncmp) its
# calls need. Wires known_images_lookup() into the DT_NEEDED search path
# (bead vms-30d).
# imgact_acp.c (bead vms-3e8e) is compiled directly as an extra source of THIS
# freestanding target, the same way known_images.c is: it reads images over the
# executive Files-11 ACP (IO$_ACCESS + IO$_READVBLK on /dev/vms) instead of
# open()/pread() on a /vms POSIX path. imgact.c provides its three host
# primitives (imgact_acp_dev_open/close/ioctl) on the raw syscall layer.
# imgact_xfer.c (bead vms-f60d) is compiled directly as an extra freestanding
# translation unit here (like known_images.c): a PURE parse of an image's
# .vms$xfer section (SysV tail-jump vs. VMS-standard call), shared with the host
# unit test tests/imgact/test_xfer_parse.c.
add_executable(imgact
    imgact.c
    known_images.c
    imgact_acp.c
    imgact_xfer.c
    ${IMGACT_ARCHDIR}/start.S
    ${IMGACT_ARCH_EXTRA_SRC}
)

# Shared OVMX image + symbol-vector format/resolver (LINK.EXE contract), plus
# the libvms public headers for the resident-producer registry ABI
# (imgact_prodreg.h — struct imgact_prod_pub, imgact_publish_producers; vms-db2).
# Mirrors the standalone src/imgact/Makefile include path (-I../vmslink/include
# -I../libvms/include) so the CMake OVMX_IMGACT build sees the same headers.
target_include_directories(imgact PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include   # ovmx_activation.h (IMGACT<->C-RTL, vms-f60d)
    ${CMAKE_SOURCE_DIR}/src/vmslink/include
    ${CMAKE_SOURCE_DIR}/src/libvms/include
    ${CMAKE_SOURCE_DIR}/src/kernel)   # vms_ioctl.h + vms_acp.h (ACP arg structs)

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
