# vmsqueue - VMS Queue Manager Library
#
# Provides batch and print queue management with persistent
# binary storage in QMAN$MASTER.DAT format.
#
# Two configurations (mirrors src/vmslnm/CMakeLists.txt):
#   1. In-tree (default): driven by the top-level project(); builds the VMS
#      shareable image LIBVMSQUEUE$SHR.EXE for the Linux/aarch64/x86_64 runtime.
#      UNCHANGED by the netbsd-vax port.
#   2. Standalone netbsd-vax cross (vms-1cb2, epic vms-8e8, P4-C): a leaf
#      dependency of vmsdcl. Configured directly with `cmake -S src/vmsqueue`
#      under tools/cross-vax/toolchain-vax-netbsd.cmake; there is no enclosing
#      project(), so declare one. It builds a plain libvmsqueue.a of elf32-vax
#      objects. ssdef.h / ovmx_layout.h (libvms) and vmsfs/filespec.h (vmsfs)
#      are header-only compile deps; the elf32-vax vmsfs.a is supplied at
#      proof-time link with the rest of the stack.
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vmsqueue_standalone C)
    set(VMSQUEUE_STANDALONE ON)

    if(NOT DEFINED OVMX_LIB_TYPE)
        set(OVMX_LIB_TYPE STATIC)
    endif()
    if(NOT DEFINED VMS_INCLUDE_DIR)
        set(VMS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../libvms/include)
    endif()
    if(NOT DEFINED VMS_FS_INCLUDE_DIR)
        set(VMS_FS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vmsfs/include)
    endif()
endif()

add_library(vmsqueue ${OVMX_LIB_TYPE}
    vmsqueue.c
)

target_include_directories(vmsqueue PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VMS_INCLUDE_DIR}
)

if(VMSQUEUE_STANDALONE)
    # Link-libc netbsd-vax cross: a clean libvmsqueue.a to arch-check with
    # objdump/ar. vmsfs/filespec.h is a header-only compile dep (no vmsfs CMake
    # target exists standalone); the elf32-vax vmsfs.a is linked at proof time.
    target_include_directories(vmsqueue PRIVATE ${VMS_FS_INCLUDE_DIR})
    target_compile_options(vmsqueue PRIVATE -Wall -Wextra -O2)
else()
    target_link_libraries(vmsqueue PRIVATE vmsfs)

    set_target_properties(vmsqueue PROPERTIES
        PREFIX ""
        OUTPUT_NAME "LIBVMSQUEUE\$SHR"
        SUFFIX ".EXE"
    )
    install(TARGETS vmsqueue LIBRARY DESTINATION ${VMS_SHARE_DIR})
endif()
