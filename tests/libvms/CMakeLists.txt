# tests/libvms/CMakeLists.txt
#
# Unit tests for libvms (system services and RTL).
# Each test is a standard POSIX executable linked against the vms
# shared library and its transitive dependencies.

cmake_minimum_required(VERSION 3.16)

# Helper: build a libvms test executable and register it with CTest
function(libvms_add_test name source)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE
        ${VMS_INCLUDE_DIR}
    )
    target_link_libraries(${name} PRIVATE
        vms
        pthread
        m
    )
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# Test 1: sys$fao / sys$faol — Formatted ASCII Output
libvms_add_test(test_libvms_fao
    ${CMAKE_CURRENT_SOURCE_DIR}/test_fao.c)

# Test 2: lib$get_vm / lib$free_vm / lib$create_vm_zone / lib$delete_vm_zone
libvms_add_test(test_libvms_lib_vm
    ${CMAKE_CURRENT_SOURCE_DIR}/test_lib_vm.c)

# Test 3: lib$extv / lib$extzv / lib$insv / lib$adawi / lib$bbcci / lib$bbssi
libvms_add_test(test_libvms_lib_bitops
    ${CMAKE_CURRENT_SOURCE_DIR}/test_lib_bitops.c)

# Test 4: str$copy_dx / str$concat / str$compare / str$trim / str$upcase / str$match_wild
libvms_add_test(test_libvms_str_routines
    ${CMAKE_CURRENT_SOURCE_DIR}/test_str_routines.c)

# Test 5: mth$sin / mth$cos / mth$sqrt / mth$exp / mth$alog / mth$power / etc.
libvms_add_test(test_libvms_mth_routines
    ${CMAKE_CURRENT_SOURCE_DIR}/test_mth_routines.c)

# Test 6: lib$/mth$/ots$ RTL — date/time, conversions, process/system info, output
libvms_add_test(test_libvms_lib_rtl
    ${CMAKE_CURRENT_SOURCE_DIR}/test_lib_rtl.c)

# Test 7: vms-cd4 conformance-gap batch — dynamic descriptors, simplified
# logical-name/CLI-symbol wrappers, sys$purgws, sys$getutc
libvms_add_test(test_libvms_lib_symtab
    ${CMAKE_CURRENT_SOURCE_DIR}/test_lib_symtab.c)

# Test 8: vms-fb3 conformance-gap batch (round 2) — lib$sys_asctim,
# sys$deltva_64, lib$getdvi, sys$dgblsc, sys$device_scan, sys$unwind,
# sys$lkwset/sys$ulwset
libvms_add_test(test_libvms_lib_fb3
    ${CMAKE_CURRENT_SOURCE_DIR}/test_lib_fb3.c)

# Test 9: vms-846.1 — binary SYSUAF record layout + RMS primary-key XABKEY
libvms_add_test(test_libvms_sysuaf_record
    ${CMAKE_CURRENT_SOURCE_DIR}/test_sysuaf_record.c)

# Test 10: vms-ci.8 — node identity + cluster SYSGEN params (SCSNODE/
# SCSSYSTEMID round-trip through the typed SYSGEN store, sys$getsyi)
libvms_add_test(test_libvms_sysgen_identity
    ${CMAKE_CURRENT_SOURCE_DIR}/test_sysgen_identity.c)

# Test 11: vms-e652 (INV-1) — system-identity SSOT + SYS$ANNOUNCE/SYS$WELCOME.
# Not via libvms_add_test(): the banner resolver translates logical names and
# VMS filespecs, so this one also needs vmslnm and vmsfs.
add_executable(test_libvms_identity
    ${CMAKE_CURRENT_SOURCE_DIR}/test_identity.c)
target_include_directories(test_libvms_identity PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
)
target_link_libraries(test_libvms_identity PRIVATE
    vms
    vmsfs
    vmslnm
    pthread
    m
)
add_test(NAME test_libvms_identity COMMAND test_libvms_identity)

# Test 12: vms-as1 (executive retrofit Phase 1) — sys$dclast/sys$setast route
# through vms_kif/vms.ko; on a host with no /dev/vms (this build environment
# is never the QEMU runtime -- CLAUDE.md Rule 9), they must fail honestly
# with SS$_NOSUCHDEV rather than silently simulating AST delivery locally.
# The positive "AST actually fires via the executive" proof lives in the
# QEMU kernel-executive CI job (tests/qemu/test_kmod_ast.c).
libvms_add_test(test_libvms_ast
    ${CMAKE_CURRENT_SOURCE_DIR}/test_ast.c)
