# vmsdcl - DCL Shell Interpreter
# Pure C implementation with hand-written recursive descent parser
#
# Two configurations (mirrors src/vmsrms/CMakeLists.txt):
#   1. In-tree (default): driven by the top-level project(); builds the DCL.EXE
#      shell executable for the Linux/aarch64/x86_64 runtime, linking the VMS
#      shareable images and (optionally) GNU readline. UNCHANGED by the
#      netbsd-vax port.
#   2. Standalone netbsd-vax cross (vms-1cb2, epic vms-8e8, P4-C; CLAUDE.md
#      "Library Build Order": ... vmsfs -> vmsrms -> vmsdcl -- the LAST userspace
#      library before the init+images integration). Configured directly with
#      `cmake -S src/vmsdcl` under tools/cross-vax/toolchain-vax-netbsd.cmake.
#      There is no enclosing project(), so declare one; it compiles every DCL
#      translation unit to elf32-vax objects and archives them into a plain
#      libvmsdcl.a. build-vmsdcl-vax.sh then links a real vax--netbsdelf DCL.EXE
#      from that archive (--whole-archive: every DCL object must resolve) against
#      the WHOLE elf32-vax userspace stack (vmsqueue + vmsrms + vmsfs + vmslnm +
#      libvms + vmsprocess + libvmssys) and NetBSD libc + libpthread + libm.
#      readline is NOT in the NetBSD/vax sysroot, so HAVE_READLINE stays
#      undefined and DCL's own non-readline line editor is used (the same path
#      the static musl QEMU build takes) -- readline is optional, not required.
cmake_minimum_required(VERSION 3.16)

# ----------------------------------------------------------------------------
# Standalone detection (mirrors src/vmsrms/CMakeLists.txt). When configured on
# its own -- the netbsd-vax cross gate -- there is no top-level project() and
# none of the parent's cache variables are set, so declare a project and supply
# the header-only include dirs the parent normally injects.
# ----------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vmsdcl_standalone C)
    set(VMSDCL_STANDALONE ON)

    if(NOT DEFINED OVMX_LIB_TYPE)
        set(OVMX_LIB_TYPE STATIC)
    endif()
    # ssdef.h / ovmx_layout.h / prvdef.h / lib$routines.h etc. live in libvms's
    # public include dir; the sibling library public headers are compile-only
    # deps (their elf32-vax archives are supplied at proof-time link).
    if(NOT DEFINED VMS_INCLUDE_DIR)
        set(VMS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../libvms/include)
    endif()
    if(NOT DEFINED VMS_FS_INCLUDE_DIR)
        set(VMS_FS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vmsfs/include)
    endif()
    if(NOT DEFINED VMS_LNM_INCLUDE_DIR)
        set(VMS_LNM_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vmslnm/include)
    endif()
    if(NOT DEFINED VMS_RMS_INCLUDE_DIR)
        set(VMS_RMS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vmsrms/include)
    endif()
    if(NOT DEFINED VMS_PROCESS_INCLUDE_DIR)
        set(VMS_PROCESS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vmsprocess/include)
    endif()
    # In-tree these three are absolute ${CMAKE_SOURCE_DIR}/src/... paths; here
    # CMAKE_SOURCE_DIR is src/vmsdcl, so resolve the siblings relatively.
    set(OVMX_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
else()
    set(OVMX_SRC_ROOT ${CMAKE_SOURCE_DIR}/src)
endif()

set(VMSDCL_SOURCES
    dcl_main.c
    dcl_lexer.c
    dcl_parser.c
    dcl_exec.c
    dcl_backup.c
    dcl_builtin.c
    dcl_cmd_show.c
    dcl_cmd_set.c
    dcl_cmd_file.c
    dcl_cmd_process.c
    dcl_cmd_io.c
    dcl_cmd_misc.c
    dcl_disk_logical.c
    dcl_editor.c
    dcl_terminal.c
    dcl_symbol.c
    dcl_lexical.c
    dcl_filespec.c
    dcl_io.c
    dcl_script.c
    dcl_messages.c
    dcl_library.c
    dcl_help.c
    dcl_mbx.c
)

if(VMSDCL_STANDALONE)
    # Cross gate: archive of elf32-vax objects (arch-checked with objdump);
    # build-vmsdcl-vax.sh performs the real DCL.EXE link over the full stack.
    # It links --whole-archive over the userspace stack, so rightslist_live.o /
    # sysuaf_live.o are already pulled -- no force-bind anchor needed here.
    add_library(vmsdcl STATIC ${VMSDCL_SOURCES})
else()
    # RMS force-bind anchor (vms-586, epic vms-208): F$IDENTIFIER reaches
    # RIGHTSLIST/SYSUAF through the libvms `#pragma weak` engine seam
    # (rightslist_name_to_value -> ovmx_rightslist_asctoid; the UIC-identifier
    # fall-through -> sysuaf_lookup -> ovmx_sysuaf_read_user). In this STATIC-musl
    # executable link, archive member-pull is per-object: a weak-undef reference
    # does NOT extract rightslist_live.o / sysuaf_live.o from libvmsrms, so the
    # weak cells stay NULL and every F$IDENTIFIER goes dead (test_syssvc_ident).
    # dcl_rms_bind.c is a guarded, never-executed strong reference to all four
    # engine entries that forces both leaf objects to be extracted -- the same
    # anchor mk_dcl.sh links into the LINK.EXE shareable DCL.EXE, reused here for
    # the cmake DCL.EXE the QEMU KE harness stages at /bin/DCL.EXE.
    add_executable(vmsdcl ${VMSDCL_SOURCES}
        ${OVMX_SRC_ROOT}/vmslink/dcl_rms_bind.c)
endif()

target_include_directories(vmsdcl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_RMS_INCLUDE_DIR}
    ${VMS_PROCESS_INCLUDE_DIR}
    # SHOW DEVICE reads the executive device table through the
    # kernel-interface client (vms-fb9): src/libvmssys/vms_kif.h, which
    # pulls src/kernel/vms_ioctl.h for the shared ioctl structures.
    ${OVMX_SRC_ROOT}/libvmssys
    # LIBRARY/OBJECT (.OLB) shares the ar-container reader/writer with
    # LIBRARIAN.EXE: src/vmslink/include/ovmx_olb.h -- a header-only format
    # module, so no vmslink link dependency is added. (vms-ca9)
    ${OVMX_SRC_ROOT}/vmslink/include
    # vmsqueue.h (SHOW/SUBMIT queue surface) lives in the vmsqueue source dir.
    # In-tree it arrives via the vmsqueue target's exported includes; standalone
    # (no vmsqueue CMake target) we point at the dir -- the elf32-vax
    # libvmsqueue.a is linked at proof time.
    ${OVMX_SRC_ROOT}/vmsqueue
)

# Feature-test macros. On Linux (glibc/musl) POSIX 2008 + the _DEFAULT_SOURCE
# BSD extensions expose fork/sigaction/flock/FNM_CASEFOLD/struct ifreq/etc. On
# NetBSD the same BSD surface (LOCK_*, FNM_CASEFOLD, struct ifreq, IFNAMSIZ,
# IFF_UP, SIOC*) is gated behind _NETBSD_SOURCE, and defining _POSIX_C_SOURCE
# would TURN IT OFF -- so the netbsd-vax cross selects the native widest
# namespace instead (vms-1cb2).
if(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    target_compile_definitions(vmsdcl PRIVATE _NETBSD_SOURCE)
else()
    target_compile_definitions(vmsdcl PRIVATE
        _POSIX_C_SOURCE=200809L
        _DEFAULT_SOURCE
    )
endif()

# vms-329: DCL reaches SYS$DISK via the executive Files-11 ACP on EVERY
# substrate. Two arms in this library were keyed on __linux__ back when the
# netbsd-vax runtime still VFS-mounted SYS$DISK and so still had a /vms POSIX
# tree to fall back to -- @-procedure execution (dcl_script.c) and activatable-
# image resolution (dcl_cmd_process.c). The coupled VAX cutover retired that
# mount, so on the VAX those fallbacks reach nothing: the traced cutover boot
# reached DCL and answered %DCL-E-OPENIN on SYS$MANAGER:STARTUP.COM. Both arms
# are substrate-neutral code (dcl_rms_read_open lives in dcl_filespec.c, which
# every configuration compiles), so this is a definition, not a port. The
# STANDALONE (netbsd-vax cross) archive is NOT a unit build -- it is the
# libvmsdcl.a that becomes the shipped DCL.EXE -- so it gets the definition too,
# exactly as src/vmsrms/CMakeLists.txt does. Runtime still fails honestly with
# no /dev/vms: the RMS ACP arm answers RMS$_ACC and DCL says so (INV-6).
target_compile_definitions(vmsdcl PRIVATE OVMX_HAVE_ACP)

if(VMSDCL_STANDALONE)
    target_compile_options(vmsdcl PRIVATE -Wall -Wextra -O2)
else()
    # Core link libraries. vms-481: DCL file commands + F$ file lexicals now
    # reach files through RMS ($OPEN/$GET/$CREATE/$PUT/$SEARCH + rms_file_attr,
    # which route to the Files-11 ODS-2 ACP), so vmsdcl links vmsrms. (The
    # native-link build already --uses LIBVMSRMS$SHR; mk_dcl.sh.)
    # vms-274: F$IDENTIFIER/SHOW read RIGHTSLIST/SYSUAF via RMS-over-ACP too.
    target_link_libraries(vmsdcl PRIVATE pthread vms vmsrms vmsfs vmsprocess vmslnm vmsqueue)

    # Try to find readline for better interactive experience (skip for static
    # builds; and for the netbsd-vax cross, which has no readline in its sysroot
    # and takes DCL's own non-readline line editor instead).
    if(NOT OVMX_STATIC)
        find_library(READLINE_LIB readline)
        if(READLINE_LIB)
            target_compile_definitions(vmsdcl PRIVATE HAVE_READLINE)
            target_link_libraries(vmsdcl PRIVATE ${READLINE_LIB})
        endif()
    endif()

    set_target_properties(vmsdcl PROPERTIES
        OUTPUT_NAME "DCL"
        SUFFIX ".EXE"
    )
    install(TARGETS vmsdcl RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
endif()
