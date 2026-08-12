# libvmssys - VMS Syscall Layer (no glibc dependency)
#
# This library provides direct Linux syscall access, string primitives,
# formatted output, futex-based synchronization, buffered I/O, and math
# for building OVMX without any C library.
#
# Two build modes:
#   1. Static library (libvmssys.a) -- for linking freestanding binaries
#   2. Test programs that verify each layer works

cmake_minimum_required(VERSION 3.16)

# When configured standalone (the netbsd-vax cross build drives
# `cmake -S src/libvmssys` with a toolchain file, outside the top-level tree),
# there is no enclosing project(); declare one. C only -- the freestanding ASM
# backends are added per-substrate below, and the netbsd substrate has none.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vmssys_standalone C)
endif()

# ================================================================
# Substrate + architecture selection
# ================================================================
# TWO SYSKRNL substrates (docs/design-ovmx-netbsd-syskrnl.md):
#   * "linux"  -- raw-freestanding: hand-written per-arch crt0/syscall/sigreturn
#                 asm, no libc; x86_64 + aarch64. Today's default.
#   * "netbsd" -- link NetBSD libc (§4.1): NO OVMX asm, NetBSD csu/libc provides
#                 crt0/TLS/syscall plumbing; libvmssys contributes the VMS API
#                 surface + the /dev/vms kif transport. Initially VAX -- ILP32
#                 little-endian, a NEW width class (every other target is 64-bit)
#                 that the vms-9dc P3 ILP32/endian audit validates against.
if(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    set(VMSSYS_SUBSTRATE "netbsd")
else()
    set(VMSSYS_SUBSTRATE "linux")
endif()

if(VMSSYS_SUBSTRATE STREQUAL "netbsd")
    # ---- OVMX/NetBSD (link-libc) substrate ----
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "vax")
        set(VMSSYS_ARCH "vax")
    else()
        set(VMSSYS_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    message(STATUS "libvmssys: building for netbsd-${VMSSYS_ARCH} (link-libc)")

    # No OVMX asm on NetBSD -- libc's csu provides crt0/sigreturn/TLS.
    set(VMSSYS_ASM_SOURCES "")

    # Link-libc build set: substrate-agnostic VMS RTL + kif policy + NetBSD
    # transport leaf. Excluded on purpose (design §4.1):
    #   * vms_runtime_init.c    -- hand-rolled auxv/TLS; NetBSD csu/libc does it.
    #   * vms_stdio.c/vms_futex.c -- freestanding buffered-I/O + futex sync,
    #     superseded by NetBSD libc/libpthread; they also use VMS_O_*/futex
    #     wrappers whose per-substrate constant resolution is a deferred audit
    #     item (docs/design-ovmx-netbsd-syskrnl.md §4.2).
    #   * kif_transport_linux.c -- replaced by kif_transport_netbsd.c.
    set(VMSSYS_C_SOURCES
        vms_string.c
        vms_snprintf.c
        vms_math.c
        vms_kif.c
        kif_transport_netbsd.c
    )
else()
    # ---- OVMX/Linux (raw-freestanding) substrate ----
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
        set(VMSSYS_ARCH "x86_64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        set(VMSSYS_ARCH "aarch64")
    else()
        message(FATAL_ERROR "libvmssys: unsupported architecture ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    message(STATUS "libvmssys: building for ${VMSSYS_ARCH} (freestanding)")

    set(VMSSYS_ASM_SOURCES
        arch/${VMSSYS_ARCH}/syscall.S
        arch/${VMSSYS_ARCH}/crt0.S
        arch/${VMSSYS_ARCH}/sigreturn.S
    )
    set(VMSSYS_C_SOURCES
        vms_string.c
        vms_snprintf.c
        vms_futex.c
        vms_stdio.c
        vms_math.c
        vms_runtime_init.c
        vms_kif.c
        kif_transport_linux.c
    )
endif()

# ================================================================
# Static library: libvmssys.a
# ================================================================
add_library(vmssys STATIC
    ${VMSSYS_ASM_SOURCES}
    ${VMSSYS_C_SOURCES}
)

target_include_directories(vmssys PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# vmssys is always STATIC (freestanding binaries link it directly), but
# libvms (a SHARED library in dev builds) also links it now, to reach
# vms_kif_* (the /dev/vms ioctl wrappers used by sys_lock.c). A static
# archive linked into a shared object must be position-independent.
set_target_properties(vmssys PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(VMSSYS_SUBSTRATE STREQUAL "netbsd")
    # Link-libc: ordinary hosted compile against the NetBSD sysroot. No
    # -ffreestanding/-nostdlib -- the C sources include NetBSD libc headers
    # (unistd.h / sys/mman.h) on this path.
    target_compile_options(vmssys PRIVATE
        -fno-stack-protector
        -Wall
        -Wextra
        -O2
    )
else()
    # Freestanding build: no glibc, no builtins except what we explicitly use.
    # -fno-tree-vectorize: GCC vectorization generates SSE/AVX instructions
    # requiring 16-byte aligned stack which our custom CRT may not guarantee.
    target_compile_options(vmssys PRIVATE
        -ffreestanding
        -fno-builtin
        -fno-stack-protector
        -fno-tree-vectorize
        -Wall
        -Wextra
        -O2
    )
    # AArch64: disable outline atomics (they reference glibc symbols)
    if(VMSSYS_ARCH STREQUAL "aarch64")
        target_compile_options(vmssys PRIVATE -mno-outline-atomics)
    endif()
    # Assembly files need the C preprocessor
    set_source_files_properties(${VMSSYS_ASM_SOURCES} PROPERTIES
        LANGUAGE ASM
    )
endif()

# ================================================================
# Test programs
# ================================================================
# The unit tests are freestanding binaries (-nostdlib -static, calling raw
# syscalls through the asm CRT). They belong to the raw-freestanding (linux)
# substrate only. On the netbsd link-libc substrate there is no OVMX CRT to link
# them against and they are not part of the build set; the netbsd-vax gate is
# tools/cross-vax/build-libvmssys-vax.sh (library build + a link-libc smoke exe).
if(VMSSYS_SUBSTRATE STREQUAL "linux")

# Helper function: build a freestanding test binary
function(vmssys_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE vmssys)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${name} PRIVATE
        -ffreestanding
        -fno-builtin
        -fno-stack-protector
        -fno-tree-vectorize
        -O2
    )
    if(VMSSYS_ARCH STREQUAL "aarch64")
        target_compile_options(${name} PRIVATE -mno-outline-atomics)
    endif()
    target_link_options(${name} PRIVATE
        -nostdlib
        -nostartfiles
        -nodefaultlibs
        -static
    )
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# Test 1: Basic syscall (write "hello" to stdout)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/libvmssys/test_syscall.c)
    vmssys_add_test(test_vmssys_syscall
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/libvmssys/test_syscall.c)
endif()

# Test 2: String operations
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/libvmssys/test_string.c)
    vmssys_add_test(test_vmssys_string
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/libvmssys/test_string.c)
endif()

# Test 3: snprintf
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/libvmssys/test_snprintf.c)
    vmssys_add_test(test_vmssys_snprintf
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/libvmssys/test_snprintf.c)
endif()

# Test 4: Futex sync
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/libvmssys/test_futex.c)
    vmssys_add_test(test_vmssys_futex
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/libvmssys/test_futex.c)
endif()

# Test 5: Buffered I/O
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/libvmssys/test_stdio.c)
    vmssys_add_test(test_vmssys_stdio
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/libvmssys/test_stdio.c)
endif()

# Test 6: Math
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/libvmssys/test_math.c)
    vmssys_add_test(test_vmssys_math
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/libvmssys/test_math.c)
endif()

# Test 7: Full CRT bootstrap
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/libvmssys/test_crt.c)
    vmssys_add_test(test_vmssys_crt
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/libvmssys/test_crt.c)
endif()

endif()  # VMSSYS_SUBSTRATE STREQUAL "linux"
