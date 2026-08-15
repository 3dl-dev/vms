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

# --- Decision-A linkage (rd vms-42d) ----------------------------------------
# docs/design-unified-cross-build.md §2.1. The top-level CMakeLists.txt keys
# OVMX_LIB_TYPE=STATIC (RTL as archives, no global -static, no OVMX_IMGACT
# flags) off CMAKE_SYSTEM_NAME STREQUAL "NetBSD", which this file sets above
# -- nothing further is needed here for that part of Decision-A. What the
# toolchain file itself supplies is the substrate's common system link deps,
# once, so no per-image script has to repeat "-lpthread -lm -latomic":
#   pthread — the NetBSD libpthread the RTL's pthread_* calls need.
#   m       — libm (math), needed by RTL floating-point code.
#   atomic  — NetBSD/vax's gcc needs libatomic for the 64-bit/wide atomic
#             builtins the RTL uses (__atomic_*); the vax ISA has no native
#             wide compare-and-swap, so gcc calls out to libatomic instead of
#             inlining, matching what tools/cross-vax/build-*.sh already link
#             by hand on every image.
# CMAKE_C_STANDARD_LIBRARIES is appended once, at the very end of every link
# line CMake generates for this toolchain (the same slot the implicit libc
# occupies), so it naturally lands after every target's own
# target_link_libraries() -- no per-target repetition required.
set(CMAKE_C_STANDARD_LIBRARIES "-lpthread -lm -latomic" CACHE STRING
    "Decision-A (vms-42d): common NetBSD/vax system link deps, applied once" FORCE)

# Static-RTL archive cycle (design §2.1 point 3): under Decision-A the RTL
# libraries are STATIC archives (SHARED never applies on this substrate), so
# any circular archive dependency among them (e.g. libvms <-> libvmsfs, the
# case the design calls out explicitly) must be resolved with a linker
# --start-group/--end-group around the whole set, the way every
# tools/cross-vax/build-*.sh already links by hand. Rather than requiring
# every CMakeLists.txt target to declare the cycle bidirectionally (deferred
# to Rung B, vms-64a) or bumping CMAKE_MINIMUM_REQUIRED to 3.24 for
# $<LINK_GROUP:RESCAN,...> (an operator-reserved version-policy call, design
# §9 decision 2 -- recommendation was to stay on 3.16), wrap EVERY
# executable's LINK_LIBRARIES in a start/end group at the toolchain level.
# This is the "toolchain-level --start-group around the RTL set" fallback the
# design names explicitly, and it is a no-op (redundant but harmless) for
# link lines that happen not to need it.
set(CMAKE_C_LINK_EXECUTABLE
    "<CMAKE_C_COMPILER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> -Wl,--start-group <LINK_LIBRARIES> -Wl,--end-group")
