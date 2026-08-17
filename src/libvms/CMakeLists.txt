# libvms - VMS runtime (system services sys$*, RTL lib$/str$/mth$/ots$).
#
# Two configurations (mirrors src/vmsprocess/CMakeLists.txt):
#   1. In-tree (default): driven by the top-level project(); builds the VMS
#      shareable image LIBVMS$SHR.EXE for the Linux/aarch64/x86_64 runtime.
#      UNCHANGED by the netbsd-vax port.
#   2. Standalone netbsd-vax cross (vms-1b2, epic vms-8e8, P4-C): configured
#      directly with `cmake -S src/libvms` under
#      tools/cross-vax/toolchain-vax-netbsd.cmake -- the same way the
#      libvmssys / vmsprocess gates configure their sources. There is no
#      enclosing project(), so declare one; it builds a plain libvms.a of
#      elf32-vax objects (link-libc substrate: NetBSD libc + libpthread + libm
#      supply pthread/signal/stdio/math; the elf32-vax libvmssys.a + vmsprocess.a
#      sit below it). io_uring is a Linux-only QIO accelerator -- on the netbsd
#      substrate sys_uring.c compiles to "unavailable" stubs and sys$qio uses its
#      real synchronous read/write path (not a LARP fallback: the classic
#      "no io_uring in this kernel" case already takes that path on Linux).
cmake_minimum_required(VERSION 3.16)

# ----------------------------------------------------------------------------
# Standalone detection (mirrors src/vmsprocess/CMakeLists.txt). When configured
# on its own -- the netbsd-vax cross gate -- there is no top-level project() and
# none of the parent's cache variables (OVMX_LIB_TYPE, VMS_*_INCLUDE_DIR, ...)
# are set, so declare a project and supply the header-only include dirs the
# parent normally injects. Each is overridable with -D.
# ----------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vms_standalone C)
    set(LIBVMS_STANDALONE ON)

    if(NOT DEFINED OVMX_LIB_TYPE)
        set(OVMX_LIB_TYPE STATIC)
    endif()
    if(NOT DEFINED VMS_RMS_INCLUDE_DIR)
        set(VMS_RMS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vmsrms/include)
    endif()
    if(NOT DEFINED VMS_FS_INCLUDE_DIR)
        set(VMS_FS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vmsfs/include)
    endif()
    if(NOT DEFINED VMS_LNM_INCLUDE_DIR)
        set(VMS_LNM_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vmslnm/include)
    endif()
    if(NOT DEFINED VMS_PROCESS_INCLUDE_DIR)
        set(VMS_PROCESS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vmsprocess/include)
    endif()
    # vms_kif.h (libvmssys), the vmslink symbol-vector headers, and the vmsscs
    # membership header are peer dirs of this one; the in-tree build reaches them
    # via ${CMAKE_SOURCE_DIR}/src/... (repo root), unavailable standalone.
    if(NOT DEFINED VMS_SYS_INCLUDE_DIR)
        set(VMS_SYS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../libvmssys)
    endif()
    if(NOT DEFINED VMS_LINK_INCLUDE_DIR)
        set(VMS_LINK_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vmslink/include)
    endif()
    if(NOT DEFINED VMS_SCS_INCLUDE_DIR)
        set(VMS_SCS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vmsscs/include)
    endif()
endif()

add_library(vms ${OVMX_LIB_TYPE}
    descrip.c
    status.c
    # Compile-time agreement lock between prvdef.h and the executive's
    # privilege table (vms-2b8). Must stay in the default target -- the
    # guard is the only thing stopping the two copies drifting apart.
    prv_agreement.c
    syssvc/sys_assign.c
    syssvc/sys_mailbox.c
    syssvc/sys_qio.c
    syssvc/sys_uring.c
    syssvc/sys_event.c
    syssvc/sys_time.c
    syssvc/sys_process.c
    syssvc/sys_memory.c
    syssvc/sys_logical.c
    syssvc/sys_ast.c
    syssvc/sys_lock.c
    syssvc/sys_misc.c
    syssvc/sys_security.c
    syssvc/sys_fao.c
    syssvc/sys_msg.c
    syssvc/sys_filescan.c
    rtl/ovmx_accounting.c
    rtl/lib_vm.c
    rtl/lib_output.c
    rtl/lib_signal.c
    rtl/lib_datetime.c
    rtl/lib_timer.c
    rtl/lib_misc.c
    rtl/lib_dyndesc.c
    rtl/lib_logical.c
    rtl/lib_symbol.c
    rtl/lib_string_ops.c
    rtl/lib_bitops.c
    rtl/lib_arith.c
    rtl/lib_charscan.c
    rtl/lib_queue.c
    rtl/lib_tree.c
    rtl/lib_tparse.c
    rtl/lib_common.c
    rtl/lib_eventflags.c
    rtl/str_routines.c
    rtl/mth_routines.c
    rtl/ots_routines.c
    rtl/lib_strconv.c
    rtl/lib_xlate_tables.c
    syssvc/sys_float.c
    syssvc/sys_uai.c
    syssvc/sys_device.c
    syssvc/sys_operator.c
    syssvc/sys_condition.c
    syssvc/sys_imgact.c
    syssvc/imgact_prodreg.c
    rtl/sha256.c
    rtl/sysuaf.c
    rtl/rightslist.c
    rtl/rms_textfile.c
    rtl/str_util.c
    rtl/lib_cli.c
)
target_include_directories(vms PUBLIC include ${VMS_RMS_INCLUDE_DIR} ${VMS_FS_INCLUDE_DIR} ${VMS_LNM_INCLUDE_DIR} ${VMS_PROCESS_INCLUDE_DIR})

if(LIBVMS_STANDALONE)
    # Link-libc netbsd-vax cross: a clean libvms.a to arch-check with objdump/ar.
    # sys_imgact.c/imgact_prodreg.c need the vmslink symbol-vector headers;
    # F$GETSYI needs the vmsscs membership header; sys_lock.c/sys_qio.c need
    # vms_kif.h (libvmssys). All header-only compile deps -- no link edge.
    target_include_directories(vms PRIVATE
        ${VMS_SYS_INCLUDE_DIR}
        ${VMS_LINK_INCLUDE_DIR}
        ${VMS_SCS_INCLUDE_DIR})
    # Ordinary hosted compile against the NetBSD sysroot (toolchain provides
    # CMAKE_SYSROOT). No VMS image naming; the gate wants a plain archive.
    target_link_libraries(vms PUBLIC pthread m)
    target_compile_options(vms PRIVATE -Wall -Wextra -O2)
else()
    # sys_imgact.c + imgact_prodreg.c consume the OVMX symbol-vector image format
    # (ovmx_image.h / ovmx_symvec.h) -- the same single-source-of-truth headers
    # LINK.EXE and IMGACT.EXE share (vms-db2). PRIVATE: only these TUs need them.
    target_include_directories(vms PRIVATE ${CMAKE_SOURCE_DIR}/src/vmslink/include)
    # F$GETSYI CLUSTER_MEMBER/CLUSTER_NODES read SCSD's live membership publication
    # (vms-8d4): src/vmsscs/include/scs_membership.h -- header-only, no link dep.
    target_include_directories(vms PRIVATE ${CMAKE_SOURCE_DIR}/src/vmsscs/include)
    # vmssys provides vms_kif_* (the /dev/vms ioctl wrappers used by sys_lock.c
    # to reach the kernel lock manager).
    target_link_libraries(vms PUBLIC vmsprocess vmsfs vmssys pthread m)
    set_target_properties(vms PROPERTIES
        PREFIX ""
        OUTPUT_NAME "LIBVMS\$SHR"
        SUFFIX ".EXE"
    )
    install(TARGETS vms LIBRARY DESTINATION ${VMS_SHARE_DIR})
    install(DIRECTORY include/ DESTINATION include/vms)
endif()
