# vmsprocess - VMS process control (PCB, ASTs, access modes, privileges)
#
# Next layer above libvmssys in the OVMX library graph (CLAUDE.md "Library Build
# Order"): libvmssys -> vmsprocess (+pthread) -> libvms -> ...
#
# Two configurations:
#   1. In-tree (default): driven by the top-level project(); builds the VMS
#      shareable image LIBVMSPROCESS$SHR.EXE (or a static twin under OVMX_IMGACT)
#      for the Linux/aarch64/x86_64 runtime. UNCHANGED by the netbsd-vax port.
#   2. Standalone netbsd-vax cross (vms-84b): configured directly with
#      `cmake -S src/vmsprocess` under tools/cross-vax/toolchain-vax-netbsd.cmake,
#      the same way libvmssys's netbsd-vax gate configures src/libvmssys. There is
#      no enclosing project(), so declare one; it builds a plain libvmsprocess.a
#      of elf32-vax objects (link-libc substrate: NetBSD libc + libpthread supply
#      pthread/signal/stdio -- no OVMX freestanding CRT, no Linux-only wait
#      primitive; AST delivery is POSIX signals, event flags live in the executive).

cmake_minimum_required(VERSION 3.16)

# ----------------------------------------------------------------------------
# Standalone detection (mirrors src/libvmssys/CMakeLists.txt). When configured
# on its own -- the netbsd-vax cross gate -- there is no top-level project() and
# none of the parent's cache variables (OVMX_LIB_TYPE, VMS_INCLUDE_DIR, ...) are
# set, so declare a project and supply link-libc defaults.
# ----------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vmsprocess_standalone C)
    set(VMSPROCESS_STANDALONE ON)

    # A plain static archive is the cross gate's artifact; the VMS image naming
    # (LIBVMSPROCESS$SHR.EXE) below is skipped for it.
    if(NOT DEFINED OVMX_LIB_TYPE)
        set(OVMX_LIB_TYPE STATIC)
    endif()
    # ssdef.h / prvdef.h live in libvms's public include dir. Let the caller
    # override (-DVMS_INCLUDE_DIR=...); otherwise resolve it relative to here.
    if(NOT DEFINED VMS_INCLUDE_DIR)
        set(VMS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../libvms/include)
    endif()
endif()

set(VMSPROCESS_SOURCES
    vms_pcb.c
    vms_process.c
    ast.c
    access_modes.c
)
add_library(vmsprocess ${OVMX_LIB_TYPE} ${VMSPROCESS_SOURCES})
target_include_directories(vmsprocess PUBLIC include ${VMS_INCLUDE_DIR})
target_link_libraries(vmsprocess PUBLIC pthread)

if(VMSPROCESS_STANDALONE)
    # Link-libc substrate: ordinary hosted compile against the NetBSD sysroot
    # (the toolchain file provides CMAKE_SYSROOT). No VMS image naming -- the
    # gate wants a clean libvmsprocess.a to arch-check with objdump/ar.
    target_compile_options(vmsprocess PRIVATE -Wall -Wextra -O2)
else()
    set_target_properties(vmsprocess PROPERTIES
        PREFIX ""
        OUTPUT_NAME "LIBVMSPROCESS\$SHR"
        SUFFIX ".EXE"
    )
    install(TARGETS vmsprocess LIBRARY DESTINATION ${VMS_SHARE_DIR})

    # Static twin for the static PID-1 bootstrap (STARTUP.EXE), which cannot link
    # a shareable (.EXE) image. Built only in OVMX_IMGACT (dynamic) mode.
    if(OVMX_IMGACT)
        add_library(vmsprocess_static STATIC ${VMSPROCESS_SOURCES})
        target_include_directories(vmsprocess_static PUBLIC include ${VMS_INCLUDE_DIR})
        target_link_libraries(vmsprocess_static PUBLIC pthread)
    endif()
endif()
