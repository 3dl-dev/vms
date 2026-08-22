# vmsrms - Record Management Services
#
# Provides the VMS-compatible RMS API for file and record operations.
# Supports sequential, relative, and indexed file organizations.
#
# Two configurations (mirrors src/vmsprocess/CMakeLists.txt):
#   1. In-tree (default): driven by the top-level project(); builds the VMS
#      shareable image LIBVMSRMS$SHR.EXE for the Linux/aarch64/x86_64 runtime.
#      UNCHANGED by the netbsd-vax port.
#   2. Standalone netbsd-vax cross (vms-271, epic vms-8e8, P4-C): configured
#      directly with `cmake -S src/vmsrms` under
#      tools/cross-vax/toolchain-vax-netbsd.cmake -- the top of the userspace
#      library graph. It builds a plain libvmsrms.a of elf32-vax objects,
#      proven to link ON TOP of the whole elf32-vax stack (libvms.a +
#      vmsfs.a + vmslnm.a + vmsprocess.a + libvmssys.a + NetBSD libc). RMS
#      file I/O is ordinary POSIX (open/lseek/off_t/fcntl/dirent/fnmatch); the
#      O_* flags come from the NetBSD sysroot's <fcntl.h> and off_t is the
#      sysroot's LFS type -- no Linux syscall or numeric-constant assumption
#      (audit-ilp32-vax-libvmssys.md §11).
cmake_minimum_required(VERSION 3.16)

# ----------------------------------------------------------------------------
# Standalone detection (mirrors src/vmsprocess/CMakeLists.txt). When configured
# on its own -- the netbsd-vax cross gate -- there is no top-level project() and
# none of the parent's cache variables (OVMX_LIB_TYPE, VMS_*_INCLUDE_DIR, ...)
# are set, so declare a project and supply the header-only include dirs the
# parent normally injects.
# ----------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vmsrms_standalone C)
    set(VMSRMS_STANDALONE ON)

    if(NOT DEFINED OVMX_LIB_TYPE)
        set(OVMX_LIB_TYPE STATIC)
    endif()
    # ssdef.h / rmsdef.h / ovmx_layout.h / lib$routines.h live in libvms's
    # public include dir.
    if(NOT DEFINED VMS_INCLUDE_DIR)
        set(VMS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../libvms/include)
    endif()
    # vmsfs/*.h (filespec/device/version) and vms/logical.h are header-only
    # compile deps; the elf32-vax vmsfs.a + vmslnm.a are supplied at proof link.
    if(NOT DEFINED VMS_FS_INCLUDE_DIR)
        set(VMS_FS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vmsfs/include)
    endif()
    if(NOT DEFINED VMS_LNM_INCLUDE_DIR)
        set(VMS_LNM_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vmslnm/include)
    endif()
endif()

add_library(vmsrms ${OVMX_LIB_TYPE}
    rms_core.c
    rms_io.c
    rms_seq.c
    rms_rel.c
    rms_idx.c
    rms_prolog3.c
    sysuaf_rms.c
    sysuaf_live.c
    rightslist_rms.c
    rightslist_live.c
    rms_record.c
    rms_parse.c
    rms_search.c
    rms_util.c
    sysgen_acp.c
)

target_include_directories(vmsrms PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
)

# vms-d5d/vms-329: RMS reaches SYS$DISK via the executive ACP (vms_kif_acp_*)
# on EVERY runtime substrate -- the standalone configuration above is not a
# "unit build", it is the netbsd-vax CROSS that produces the libvmsrms.a linked
# into the shipped VAX DCL.EXE / LOGINOUT.EXE / PROVISION.EXE / JOB_CONTROL.EXE
# (tools/cross-vax/build-*-vax.sh). Leaving OVMX_HAVE_ACP off there is what left
# every VAX image on RMS's POSIX /vms arm while the boot claimed to have flipped
# (rd vms-049's finding). The coupled cutover (vms-329) retires the vmsfs.ko VFS
# mount of SYS$DISK, so there IS no /vms tree on the VAX any more: the POSIX arm
# would not "degrade", it would simply fail -- and RMS's own ACP arm already
# defers to POSIX only when /dev/vms is genuinely absent (rms_acp_absent(),
# which is how host ctest and the plain-container gates still work).
#
# The include dir goes with it: vms_kif.h lives in libvmssys and pulls
# ../kernel/vms_ioctl.h -> vms_acp.h. It is a header-only compile dep; the
# vms_kif_* symbols resolve at link time through libvmssys (in-tree: the
# transitive vms -> vmsprocess -> libvmssys edge; netbsd-vax cross: libvmssys.a
# inside the --start-group the per-image recipes already link).
target_compile_definitions(vmsrms PRIVATE OVMX_HAVE_ACP)
target_include_directories(vmsrms PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../libvmssys
    ${CMAKE_CURRENT_SOURCE_DIR}/../kernel)

if(VMSRMS_STANDALONE)
    # Link-libc netbsd-vax cross: a clean libvmsrms.a to arch-check with
    # objdump/ar, proven to link ON TOP of the whole elf32-vax stack at proof 2.
    # No vms/vmsfs/vmslnm CMake targets exist standalone -- the elf32-vax
    # archives are linked in at proof 2; here we only record libc requirements.
    target_link_libraries(vmsrms PUBLIC pthread m)
    target_compile_options(vmsrms PRIVATE -Wall -Wextra -O2)
else()
    target_link_libraries(vmsrms PUBLIC vms vmsfs vmslnm)

    set_target_properties(vmsrms PROPERTIES
        PREFIX ""
        OUTPUT_NAME "LIBVMSRMS\$SHR"
        SUFFIX ".EXE"
    )
    install(TARGETS vmsrms LIBRARY DESTINATION ${VMS_SHARE_DIR})
    install(DIRECTORY include/rms/ DESTINATION include/vms/rms)
endif()
