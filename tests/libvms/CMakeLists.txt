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
