# CMake toolchain file for cross-compiling to netbsd-vax (vax--netbsdelf).
#
# Used INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), which
# installs the cross gcc/ld under /opt/cross and a NetBSD/vax sysroot at
# /opt/cross/sysroot. Selecting this toolchain makes src/libvmssys/CMakeLists.txt
# take its VMSSYS_SUBSTRATE=netbsd branch (link NetBSD libc, no VAX asm) --
# docs/design-ovmx-netbsd-syskrnl.md §4.1.

set(CMAKE_SYSTEM_NAME NetBSD)
set(CMAKE_SYSTEM_PROCESSOR vax)

set(CROSS_PREFIX "$ENV{PREFIX}")
if(NOT CROSS_PREFIX)
    set(CROSS_PREFIX "/opt/cross")
endif()
set(CROSS_SYSROOT "$ENV{SYSROOT}")
if(NOT CROSS_SYSROOT)
    set(CROSS_SYSROOT "${CROSS_PREFIX}/sysroot")
endif()
set(CROSS_TARGET "vax--netbsdelf")

set(CMAKE_C_COMPILER   "${CROSS_PREFIX}/bin/${CROSS_TARGET}-gcc")
set(CMAKE_AR           "${CROSS_PREFIX}/bin/${CROSS_TARGET}-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB       "${CROSS_PREFIX}/bin/${CROSS_TARGET}-ranlib" CACHE FILEPATH "")

set(CMAKE_SYSROOT "${CROSS_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${CROSS_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Compiler works but cannot execute VAX binaries on the (amd64) build host.
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CROSSCOMPILING 1)
