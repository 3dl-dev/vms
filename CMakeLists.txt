cmake_minimum_required(VERSION 3.16)
project(OVMX
    VERSION 0.1.0
    DESCRIPTION "OpenVMS-Compatible Linux Distribution"
    LANGUAGES C ASM
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Always generate compile_commands.json (used by clang-tidy static analysis)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Expose POSIX and GNU APIs (sigaction, timer_t, FNM_CASEFOLD, etc.)
add_compile_definitions(_GNU_SOURCE)

# Common compile options
add_compile_options(-Wall -Wextra)

# Build type defaults
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# VMS directory layout
set(VMS_SHARE_DIR "vms/SYS0/SYSCOMMON/SYSLIB")
set(VMS_SYSTEM_DIR "vms/SYS0/SYSCOMMON/SYSEXE")

# Runtime library search path — all binaries find shareable images in SYS$SHARE
set(CMAKE_INSTALL_RPATH "/vms/SYS0/SYSCOMMON/SYSLIB")
set(CMAKE_BUILD_WITH_INSTALL_RPATH OFF)
set(CMAKE_BUILD_RPATH "${CMAKE_BINARY_DIR}/lib")

# Global VMS include path (for #include <descrip.h> etc.)
set(VMS_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/src/libvms/include)
set(VMS_RMS_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/src/vmsrms/include)
set(VMS_FS_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/src/vmsfs/include)
set(VMS_LNM_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/src/vmslnm/include)
set(VMS_PROCESS_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/src/vmsprocess/include)

# Options
option(BUILD_TESTS "Build test programs" ON)
option(BUILD_TOOLS "Build VMS tools" ON)
option(BUILD_FUSE "Build FUSE ODS-2 driver (requires libfuse)" OFF)
option(OVMX_STATIC "Build all libraries as static and link statically" OFF)

if(OVMX_STATIC)
    set(OVMX_LIB_TYPE STATIC)
    # Produce fully static executables (no dynamic linker dependency)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
else()
    set(OVMX_LIB_TYPE SHARED)
endif()

# Find required packages
find_package(Threads REQUIRED)

if(BUILD_TESTS)
    enable_testing()
endif()

# Subdirectories (order matters for dependencies)
add_subdirectory(src/libvmssys)
add_subdirectory(src/vmsprocess)
add_subdirectory(src/vmslnm)
add_subdirectory(src/vmsfs)
add_subdirectory(src/libvms)
add_subdirectory(src/vmsrms)
add_subdirectory(src/vmsqueue)
add_subdirectory(src/vmsdcl)

if(BUILD_TESTS)
    add_subdirectory(tests/libvms)
    add_subdirectory(tests/vmsprocess)
    add_subdirectory(tests/vmslnm)
    add_subdirectory(tests/vmsfs)
    add_subdirectory(tests/vmsrms)
    add_subdirectory(tests/vmsqueue)
    add_subdirectory(tests/dcl)
endif()

# vmssshd requires libssh — skip if headers not available
find_path(LIBSSH_INCLUDE_DIR NAMES libssh/libssh.h)
find_library(LIBSSH_LIBRARY NAMES ssh)
if(LIBSSH_INCLUDE_DIR AND LIBSSH_LIBRARY)
    add_subdirectory(src/vmsssh)
    if(BUILD_TESTS)
        add_subdirectory(tests/vmsssh)
    endif()
else()
    message(STATUS "libssh not found — vmssshd will not be built")
endif()

if(BUILD_TOOLS)
    add_subdirectory(tools)
    add_subdirectory(src/ovmx_init)
endif()

# Install VMS system disk (ODS-2 directory structure)
install(DIRECTORY distro/rootfs/vms/ DESTINATION /vms
    DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                         GROUP_READ GROUP_EXECUTE
                         WORLD_READ WORLD_EXECUTE)
