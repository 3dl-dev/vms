# vmslnm - Logical Name Manager
#
# Two configurations (mirrors src/vmsprocess/CMakeLists.txt):
#   1. In-tree (default): driven by the top-level project(); builds the VMS
#      shareable image LIBVMSLNM$SHR.EXE for the Linux/aarch64/x86_64 runtime.
#      UNCHANGED by the netbsd-vax port.
#   2. Standalone netbsd-vax cross (vms-271, epic vms-8e8, P4-C): configured
#      directly with `cmake -S src/vmslnm` under
#      tools/cross-vax/toolchain-vax-netbsd.cmake -- the same way the
#      libvmssys / vmsprocess / libvms gates configure their sources. There is
#      no enclosing project(), so declare one; it builds a plain libvmslnm.a of
#      elf32-vax objects. vms_kif.h (libvmssys) is a header-only compile dep;
#      the elf32-vax libvmssys.a is supplied at proof-time link.
cmake_minimum_required(VERSION 3.16)

# ----------------------------------------------------------------------------
# Standalone detection (mirrors src/vmsprocess/CMakeLists.txt). When configured
# on its own -- the netbsd-vax cross gate -- there is no top-level project() and
# none of the parent's cache variables (OVMX_LIB_TYPE, VMS_INCLUDE_DIR, ...) are
# set, so declare a project and supply link-libc defaults + the header-only
# include dirs the parent normally injects.
# ----------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vmslnm_standalone C)
    set(VMSLNM_STANDALONE ON)

    if(NOT DEFINED OVMX_LIB_TYPE)
        set(OVMX_LIB_TYPE STATIC)
    endif()
    # ssdef.h / lnmdef.h / ovmx_layout.h live in libvms's public include dir.
    if(NOT DEFINED VMS_INCLUDE_DIR)
        set(VMS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../libvms/include)
    endif()
    # vms_kif.h (the /dev/vms client ABI) lives in libvmssys. In-tree it arrives
    # via the vmssys target's exported includes; standalone we point at the dir.
    if(NOT DEFINED VMS_SYS_INCLUDE_DIR)
        set(VMS_SYS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../libvmssys)
    endif()
endif()

set(VMSLNM_SOURCES
    lnm_table.c
    lnm_translate.c
    lnm_client.c
    lnm_defaults.c
)

add_library(vmslnm ${OVMX_LIB_TYPE} ${VMSLNM_SOURCES})

target_include_directories(vmslnm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VMS_INCLUDE_DIR}
)

if(VMSLNM_STANDALONE)
    # Link-libc netbsd-vax cross: a clean libvmslnm.a to arch-check with
    # objdump/ar. vms_kif.h is a header-only compile dep (no vmssys CMake
    # target exists standalone); the elf32-vax libvmssys.a is linked at proof 2.
    target_include_directories(vmslnm PRIVATE ${VMS_SYS_INCLUDE_DIR})
    target_link_libraries(vmslnm PUBLIC pthread)
    target_compile_options(vmslnm PRIVATE -Wall -Wextra -O2)
else()
    # vmssys carries vms_kif_* -- the /dev/vms executive client. LNM$SYSTEM is
    # executive-resident (vms-d37/#193), so lnm_create/_delete/_translate route it
    # through vms_kif (vms-96e2). vmssys also exports the vms_kif.h include dir.
    target_link_libraries(vmslnm PUBLIC pthread vmssys)

    set_target_properties(vmslnm PROPERTIES
        PREFIX ""
        OUTPUT_NAME "LIBVMSLNM\$SHR"
        SUFFIX ".EXE"
    )
    install(TARGETS vmslnm LIBRARY DESTINATION ${VMS_SHARE_DIR})

    # Static twin for the static PID-1 bootstrap (STARTUP.EXE). IMGACT mode only.
    if(OVMX_IMGACT)
        add_library(vmslnm_static STATIC ${VMSLNM_SOURCES})
        target_include_directories(vmslnm_static PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${VMS_INCLUDE_DIR}
        )
        target_link_libraries(vmslnm_static PUBLIC pthread vmssys)
    endif()
endif()

# There is no logical name daemon. On OpenVMS the logical name tables live in
# the executive; there is no server process to start and no socket to connect
# to. VMSLNMD.EXE was an OVMX invention with zero clients product-wide, so it
# is gone rather than reduced (vms-a4b). The executive-resident tables that
# replace it are tracked by vms-ln0 / vms-d37.
