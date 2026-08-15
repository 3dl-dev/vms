# vmsfs - userspace VMS filespec / device / version / protection library.
#
# Two configurations (mirrors src/vmsprocess/CMakeLists.txt):
#   1. In-tree (default): driven by the top-level project(); builds the VMS
#      shareable image LIBVMSFS$SHR.EXE for the Linux/aarch64/x86_64 runtime,
#      plus the genuine ODS-2 reader/writer sublibrary (ods2/). UNCHANGED by the
#      netbsd-vax port.
#   2. Standalone netbsd-vax cross (vms-271, epic vms-8e8, P4-C): configured
#      directly with `cmake -S src/vmsfs` under
#      tools/cross-vax/toolchain-vax-netbsd.cmake. There is no enclosing
#      project(), so declare one; it builds a plain libvmsfs.a of elf32-vax
#      objects, linking ON TOP of the elf32-vax vmslnm.a + libvmssys.a. The
#      genuine ODS-2 sublibrary (ods2/, byte-exact volume codec, vms-bb8's peer)
#      is a distinct module cross-built by its own gate -- not pulled in here.
cmake_minimum_required(VERSION 3.16)

# ----------------------------------------------------------------------------
# Standalone detection (mirrors src/vmsprocess/CMakeLists.txt). When configured
# on its own -- the netbsd-vax cross gate -- there is no top-level project() and
# none of the parent's cache variables (OVMX_LIB_TYPE, VMS_*_INCLUDE_DIR, ...)
# are set, so declare a project and supply the header-only include dirs the
# parent normally injects.
# ----------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vmsfs_standalone C)
    set(VMSFS_STANDALONE ON)

    if(NOT DEFINED OVMX_LIB_TYPE)
        set(OVMX_LIB_TYPE STATIC)
    endif()
    # ssdef.h / rmsdef.h / lnmdef.h / ovmx_layout.h / ovmx_fileprot.h live in
    # libvms's public include dir.
    if(NOT DEFINED VMS_INCLUDE_DIR)
        set(VMS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../libvms/include)
    endif()
    # vms/logical.h (vmslnm) is a header-only compile dep here; the elf32-vax
    # libvmslnm.a is supplied at proof-time link.
    if(NOT DEFINED VMS_LNM_INCLUDE_DIR)
        set(VMS_LNM_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vmslnm/include)
    endif()
endif()

set(VMSFS_SOURCES
    vmsfs_translate.c
    vmsfs_version.c
    vmsfs_case.c
    vmsfs_protect.c
    vmsfs_device.c
)
add_library(vmsfs ${OVMX_LIB_TYPE} ${VMSFS_SOURCES})
target_include_directories(vmsfs PUBLIC include ${VMS_INCLUDE_DIR} ${VMS_LNM_INCLUDE_DIR})

if(VMSFS_STANDALONE)
    # Link-libc netbsd-vax cross: a clean libvmsfs.a to arch-check with
    # objdump/ar, proven to link ON TOP of the elf32-vax vmslnm.a + libvmssys.a
    # at proof 2. The genuine ODS-2 sublibrary is not built here (its own gate).
    # No vmslnm CMake target exists standalone -- the elf32-vax libvmslnm.a is
    # linked in at proof 2; here we only record the libc-provided requirements.
    target_link_libraries(vmsfs PUBLIC pthread)
    target_compile_options(vmsfs PRIVATE -Wall -Wextra -O2)
else()
    target_link_libraries(vmsfs PUBLIC vmslnm)
    set_target_properties(vmsfs PROPERTIES
        PREFIX ""
        OUTPUT_NAME "LIBVMSFS\$SHR"
        SUFFIX ".EXE"
    )
    install(TARGETS vmsfs LIBRARY DESTINATION ${VMS_SHARE_DIR})

    # Genuine ODS-2 (Files-11 level 2) structures + reader — a new, standalone
    # module distinct from the ODS-2-inspired on-disk format. See ods2/.
    add_subdirectory(ods2)

    # Userspace MOUNTED-VOLUME table (vms-351, epic vms-5eb "B2"): device name
    # -> (open backing fd + cached genuine-ODS-2 block-backed reader handle).
    # A DISTINCT static module linking the ods2 sublibrary directly -- NOT a
    # translation unit of LIBVMSFS$SHR.EXE. Folding the ods2_bdev_* / pread
    # primitives into the shareable's STRICT symbol-vector link
    # (src/vmslink/mk_vmsfs_shr.sh, no --allow-undefined) would drag the ODS-2
    # runtime-flip dependencies into the live shareable before the atomic group
    # (rungs R2/R3/R5/R6) is ready. Its consumers link it directly: PID 1
    # (STARTUP.EXE) today for the additive boot registration; RMS/DCL when the
    # flip lands. See src/vmsfs/vmsfs_volume.c and docs/design-ods2-runtime-flip.md.
    # ods2_sysdisk.c: the SYS$DISK read adapter -- the "/vms/..." -> ods2_bdev
    # path bridge every flip consumer (RMS/DCL/MOUNT) reroutes onto. Additive;
    # flips no live path. See src/vmsfs/sysdisk.h + docs/design-ods2-runtime-flip.md.
    add_library(vmsfs_volume STATIC vmsfs_volume.c ods2_sysdisk.c)
    target_include_directories(vmsfs_volume PUBLIC include ${VMS_INCLUDE_DIR})
    target_link_libraries(vmsfs_volume PUBLIC ods2 pthread)
    set_target_properties(vmsfs_volume PROPERTIES POSITION_INDEPENDENT_CODE ON)

    # Static twin for the static PID-1 bootstrap (STARTUP.EXE). IMGACT mode only.
    if(OVMX_IMGACT)
        add_library(vmsfs_static STATIC ${VMSFS_SOURCES})
        target_include_directories(vmsfs_static PUBLIC include ${VMS_INCLUDE_DIR} ${VMS_LNM_INCLUDE_DIR})
        target_link_libraries(vmsfs_static PUBLIC vmslnm_static)
    endif()
endif()
