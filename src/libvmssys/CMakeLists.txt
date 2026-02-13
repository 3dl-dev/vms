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

# ================================================================
# Detect architecture and select assembly sources
# ================================================================
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    set(VMSSYS_ARCH "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set(VMSSYS_ARCH "aarch64")
else()
    message(FATAL_ERROR "libvmssys: unsupported architecture ${CMAKE_SYSTEM_PROCESSOR}")
endif()

message(STATUS "libvmssys: building for ${VMSSYS_ARCH}")

set(VMSSYS_ASM_SOURCES
    arch/${VMSSYS_ARCH}/syscall.S
    arch/${VMSSYS_ARCH}/crt0.S
    arch/${VMSSYS_ARCH}/sigreturn.S
)

# ================================================================
# C sources
# ================================================================
set(VMSSYS_C_SOURCES
    vms_string.c
    vms_snprintf.c
    vms_futex.c
    vms_stdio.c
    vms_math.c
    vms_runtime_init.c
    vms_kif.c
)

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

# Freestanding build: no glibc, no builtins except what we explicitly use
target_compile_options(vmssys PRIVATE
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
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

# ================================================================
# Test programs
# ================================================================

# Helper function: build a freestanding test binary
function(vmssys_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE vmssys)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${name} PRIVATE
        -ffreestanding
        -fno-builtin
        -fno-stack-protector
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
