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

# Test 1b: sys$filescan — file specification field scan (vms-4f3, self-host
# MMK spine). Parses full/partial/wildcard/node/rooted specs and asserts each
# FSCN$_ field's returned position, length, and the fldflags presence mask.
libvms_add_test(test_libvms_filescan
    ${CMAKE_CURRENT_SOURCE_DIR}/test_filescan.c)

# Test 2: lib$get_vm / lib$free_vm / lib$create_vm_zone / lib$delete_vm_zone
libvms_add_test(test_libvms_lib_vm
    ${CMAKE_CURRENT_SOURCE_DIR}/test_lib_vm.c)

# Test 3: lib$extv / lib$extzv / lib$insv / lib$adawi / lib$bbcci / lib$bbssi
libvms_add_test(test_libvms_lib_bitops
    ${CMAKE_CURRENT_SOURCE_DIR}/test_lib_bitops.c)

# Test 4: str$copy_dx / str$concat / str$compare / str$trim / str$upcase / str$match_wild
libvms_add_test(test_libvms_str_routines
    ${CMAKE_CURRENT_SOURCE_DIR}/test_str_routines.c)

# Test 4b: lib$table_parse / lib$tparse — TPARSE finite-state parser (vms-9f6)
libvms_add_test(test_libvms_lib_tparse
    ${CMAKE_CURRENT_SOURCE_DIR}/test_lib_tparse.c)

# Test 4c: MMK parse_tables.mar C port (vms-486, self-host spine #2) — the
# hand-port of the 704-line MACRO-32 TPARSE table (freeware MadGoat MMK) into
# the OVMX TPA_GRAMMAR format, driven through the real lib$table_parse engine.
add_executable(test_libvms_mmk_parse_tables
    ${CMAKE_CURRENT_SOURCE_DIR}/test_mmk_parse_tables.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mmk_parse_tables.c)
target_include_directories(test_libvms_mmk_parse_tables PRIVATE ${VMS_INCLUDE_DIR})
target_link_libraries(test_libvms_mmk_parse_tables PRIVATE vms pthread m)
add_test(NAME test_libvms_mmk_parse_tables COMMAND test_libvms_mmk_parse_tables)

# Test 3b: vms-801 R2.2 conformance batch — lib$addx/subx/emul/ediv (extended
# arithmetic), lib$ffs/ffc (bit scan), lib$crc_table/crc, lib$scanc/skpc/char,
# lib$analyze_sdesc(_64), lib$insqhi/insqti/remqhi/remqti (self-relative
# queues), lib$insert_tree/lookup_tree/traverse_tree, lib$get_common/put_common
libvms_add_test(test_libvms_lib_rtl_batch
    ${CMAKE_CURRENT_SOURCE_DIR}/test_lib_rtl_batch.c)

# Test 3c: vms-801 R2.2 conformance batch 2 — ots$ int<->text conversions,
# ots$move3/move5/powjj, lib$add_times/sub_times/cvt_vectim (date/time),
# lib$scopy_r_dx/cvt_dx_dx, lib$movtc/tra_asc_ebc/tra_ebc_asc, lib$currency/
# digit_sep/radix_point, plus sys$bintim delta-time parsing.
libvms_add_test(test_libvms_lib_rtl_batch2
    ${CMAKE_CURRENT_SOURCE_DIR}/test_lib_rtl_batch2.c)

# Test 3d: vms-801 R2.2 conformance batch 3 — lib$init/stat/show/free_timer,
# lib$sys_asctim delta formatting, lib$show/stat/find/show/verify VM-zone
# introspection, lib$convert_date_string/get_maximum_date_length/
# format_date_time/get_date_format, lib$get_users_language/sys_getmsg,
# lib$lookup_key, and lib$get_command/get_foreign RMS$_EOF behavior.
libvms_add_test(test_libvms_lib_rtl_batch3
    ${CMAKE_CURRENT_SOURCE_DIR}/test_lib_rtl_batch3.c)

# Test 3e: vms-801 R2.2 conformance batch 4 — lib$scopy_dxdx (S/D/VS string
# copy), lib$reserve_ef (specific event-flag reservation), lib$wait (real-time
# delay; IEEE + VAX float decode), ots$divct_r3/mulct_r3/powctct_r3/powctj_r3
# (complex arithmetic), ots$cnvout_t (float->sci text), ots$cvt_t_t (text->float).
libvms_add_test(test_libvms_lib_rtl_batch4
    ${CMAKE_CURRENT_SOURCE_DIR}/test_lib_rtl_batch4.c)

# Test 4c: sys$getjpiw with an ILE3 item list (vms-da9) — regression for the
# starlet.h signature fix (prcnam/itmlst are void*, so ILE3 item lists compile).
libvms_add_test(test_libvms_getjpi_ile3
    ${CMAKE_CURRENT_SOURCE_DIR}/test_getjpi_ile3.c)

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

# Test 11b: vms-98c (self-host spine #4, exec-drive prereq A) — lib$spawn
# creates a REAL DCL subprocess that runs the command, replacing the /bin/sh
# facade. Redirects SYS$SYSTEM to a private root and stages the build's own
# vmsdcl there as DCL.EXE (passed in as VMSDCL_PATH), so the real lib$spawn
# resolve+fork+exec path runs hermetically. Needs vmsfs (filespec translator)
# and vmslnm (SYS$SYSTEM redirect), same as test_libvms_identity above.
add_executable(test_libvms_lib_spawn_dcl
    ${CMAKE_CURRENT_SOURCE_DIR}/test_lib_spawn_dcl.c)
target_include_directories(test_libvms_lib_spawn_dcl PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
)
add_dependencies(test_libvms_lib_spawn_dcl vmsdcl)
target_link_libraries(test_libvms_lib_spawn_dcl PRIVATE
    vms
    vmsfs
    vmslnm
    pthread
    m
)
add_test(NAME test_libvms_lib_spawn_dcl COMMAND test_libvms_lib_spawn_dcl)
# The DCL image path is handed over at RUNTIME via the environment, NOT as a
# quoted -D compile definition: a string compile definition puts backslash-
# escaped quotes into compile_commands.json, which the kif_caller_census gate's
# reader does not unescape. $<TARGET_FILE:vmsdcl> keeps it hermetic (the image
# the build produced). SKIP_RETURN_CODE lets the test skip honestly if the
# variable is ever absent (e.g. run standalone) rather than fake a pass.
set_tests_properties(test_libvms_lib_spawn_dcl PROPERTIES
    ENVIRONMENT "OVMX_TEST_DCL_IMAGE=$<TARGET_FILE:vmsdcl>"
    SKIP_RETURN_CODE 77)

# Test 12: vms-2b8 — the SYSTEM protection category is "UIC group <=
# MAXSYSGROUP", not "UID 0". Guards the rule RMS enforces file protection
# with once a session really runs as the authenticated user's UIC.
#
# vms-f81 extended this test to round-trip real files through
# vmsfs_mode_to_protection() (vmsfs) and sys$chkpro() (vms) together, so
# it needs vmsfs linked in -- not via libvms_add_test(), same reason
# test_libvms_identity above isn't.
add_executable(test_libvms_protection
    ${CMAKE_CURRENT_SOURCE_DIR}/test_protection.c)
target_include_directories(test_libvms_protection PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
)
target_link_libraries(test_libvms_protection PRIVATE
    vms
    vmsfs
    pthread
    m
)
add_test(NAME test_libvms_protection COMMAND test_libvms_protection)

# ...and the fixture that lets it run honestly on a host whose distro policy
# blocks unprivileged user namespaces (rd vms-bd7). test_protection.c
# synthesizes a caller UIC either with setgid()+setuid() (needs CAP_SETGID +
# CAP_SETUID) or with CLONE_NEWUSER; Ubuntu 24.04 -- both the GitHub Actions
# runner and the workshop dev seat -- restricts the second by AppArmor default.
# CI carried an inline `sudo setcap` for exactly this and the dev seat did not,
# so the same commit was green in one place and red in the other. The grant now
# lives in ONE script that both call.
#
# It is a FIXTURE, not a post-build step, because setcap's file capabilities are
# dropped on every relink -- the fixture re-applies them on every ctest run. The
# script always exits 0 by design: where the grant is impossible the test must
# still RUN and fail honestly, never be marked "Not Run", which is what a
# nonzero fixture would do and is a skip by another name.
add_test(NAME test_libvms_protection_caps
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/grant_protection_caps.sh
            $<TARGET_FILE:test_libvms_protection>)
set_tests_properties(test_libvms_protection_caps PROPERTIES
    FIXTURES_SETUP ovmx_protection_caps)
set_tests_properties(test_libvms_protection PROPERTIES
    FIXTURES_REQUIRED ovmx_protection_caps)

# Test 13: vms-2b8 round 13 — CodeQL cpp/unclear-buffer-write on the
# snprintf-return accumulation at dcl_cmd_show.c:1597 (Authorized
# privileges grid) and dcl_lexical.c:1472 (CURPRIV/AUTHPRIV renderer).
# Host-buildable: reproduces the pre-fix and post-fix patterns against
# the REAL privilege name table (generated from the same
# VMS_PRIV_NAME_LIST X-macro both fixed files read) with a canary-guarded
# undersized buffer neither renderer's real callers can reach today
# (see the file's own header comment for the reachability measurement).
add_executable(test_libvms_priv_render_bounds
    ${CMAKE_CURRENT_SOURCE_DIR}/test_priv_render_bounds.c)
target_include_directories(test_libvms_priv_render_bounds PRIVATE
    ${VMS_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/include
)
add_test(NAME test_libvms_priv_render_bounds
         COMMAND test_libvms_priv_render_bounds)

# Test 14: vms-08f — every SYSUAF row with an unset password hash refuses
# EVERY password, derived from the actual shipped SYSUAF.DAT rather than a
# hand-enumerated account list (see the file's own header comment). Takes
# the real shipped file as argv[1] so the assertion always runs against
# what actually boots, not a fixture that can drift from it.
add_executable(test_libvms_sysuaf_auth
    ${CMAKE_CURRENT_SOURCE_DIR}/test_sysuaf_auth.c)
target_include_directories(test_libvms_sysuaf_auth PRIVATE
    ${VMS_INCLUDE_DIR}
)
target_link_libraries(test_libvms_sysuaf_auth PRIVATE
    vms
    pthread
    m
)
add_test(NAME test_libvms_sysuaf_auth
         COMMAND test_libvms_sysuaf_auth
         "${CMAKE_SOURCE_DIR}/distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/SYSUAF.DAT")

# Tests vms-e60 (SYSUAF.DAT UIC fields are OCTAL) and vms-2f8 (F$IDENTIFIER
# READS the rights database) MIGRATED to the QEMU kernel-executive harness by
# vms-d37: tests/qemu/test_syssvc_sysuaf_uic_base.c and
# tests/qemu/test_syssvc_rightslist.c. They locate SYS$SYSTEM:SYSUAF.DAT /
# SYS$SYSTEM:RIGHTSLIST.DAT by resolving the SYS$SYSTEM logical, which is
# executive-resident. On the host that only ever "worked" because
# lnm_seed_system_locating() reseeded LNM$SYSTEM into LNM$PROCESS_TABLE with no
# executive -- the host-LNM fake CLAUDE.md Rule 9 / INV-6 forbids and vms-fk1
# deletes. So the assertions now run against a real /dev/vms (reading the actual
# shipped files at SYS$SYSTEM:) and honest-skip 77 without it.

# The SYSUAF record limit, at the boundary (vms-9b7).
#
# Guards the defect that let a boot halt with "%OVMX-F-EXECINIT, no SYSTEM
# record in SYS$SYSTEM:SYSUAF.DAT": five parsers of one format with three
# different line limits between them, so a row one wrote was a row another
# silently truncated. Drives real files through the real writer and the real
# reader at 511/512/513/1022/1023/1024/1025 bytes -- the sizes the deleted
# implementations disagreed on.
libvms_add_test(test_libvms_sysuaf_line_limit
    ${CMAKE_CURRENT_SOURCE_DIR}/test_sysuaf_line_limit.c)

# vms-982 — the LCK$M_/LCK$K_ constants in <lckdef.h> must match the authentic
# OpenVMS $LCKDEF layout, pinned by two independent documented-tool methods on
# the VAX V7.3 oracle (LIBRARIAN extract + MACRO-32 symbol table). Guards the
# nine wrong (several SWAPPED) bits this header carried, which made three corpus
# programs pass wrong $ENQ flag patterns. Compile-time _Static_asserts — drift
# fails the build.
libvms_add_test(test_libvms_lock_flag_bits
    ${CMAKE_CURRENT_SOURCE_DIR}/test_lock_flag_bits.c)

# vms-e9e — INV-DCL veracity gate for SET PASSWORD (docs/design-dcl-fidelity.md
# sec 3): sysuaf_write_record() persists a real hash change to SYSUAF, so
# authenticate() flips (new password succeeds, old fails) and a bystander
# row survives untouched. Needs vmsfs (vmsfs_to_linux_path/device table) and
# vmslnm (lnm_setup_defaults) to resolve SYS$SYSTEM: the same way AUTHORIZE
# and LOGIN do -- not via libvms_add_test(), same reason
# test_libvms_identity above isn't. Bootstraps its own throwaway temp root
# as DKA0: (never the real /vms mount), so it cannot race any other test.
add_executable(test_libvms_sysuaf_write_veracity
    ${CMAKE_CURRENT_SOURCE_DIR}/test_sysuaf_write_veracity.c)
target_include_directories(test_libvms_sysuaf_write_veracity PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
)
target_link_libraries(test_libvms_sysuaf_write_veracity PRIVATE
    vms
    vmsfs
    vmslnm
    pthread
    m
)
add_test(NAME test_libvms_sysuaf_write_veracity
         COMMAND test_libvms_sysuaf_write_veracity)

# vms-17d -- INV-DCL veracity gate for SET ACCOUNTING (docs/design-dcl-fidelity.md
# sec 5): ovmx_accounting_set_enabled()/_is_enabled() persist a real,
# system-wide flag, and ovmx_accounting_record_login() (login/SSH's writer)
# now honours it -- disabled means no lastlogin record, enabled means one
# appears. Needs vmsfs (vmsfs_to_linux_path/device table) and vmslnm
# (lnm_setup_defaults) to resolve SYS$MANAGER: the same way AUTHORIZE and
# LOGIN do -- same reason test_libvms_sysuaf_write_veracity above isn't via
# libvms_add_test(). Bootstraps its own throwaway temp root as DKA0: (never
# the real /vms mount), so it cannot race any other test.
add_executable(test_libvms_accounting_veracity
    ${CMAKE_CURRENT_SOURCE_DIR}/test_accounting_veracity.c)
target_include_directories(test_libvms_accounting_veracity PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
)
target_link_libraries(test_libvms_accounting_veracity PRIVATE
    vms
    vmsfs
    vmslnm
    pthread
    m
)
add_test(NAME test_libvms_accounting_veracity
         COMMAND test_libvms_accounting_veracity)

# vms-f89 -- date/time formatting reads SYS$TIMEZONE_DIFFERENTIAL at point of
# use (config from logicals dogfood, parent vms-704). lib$format_date_time now
# honours the TDF logical live, so redefining it shifts displayed time without a
# rebuild; on origin/main it hardcoded localtime and the redefinition was inert.
# Drives the libvms logical store directly (sys$crelnm/sys$trnlnm) -- host-
# buildable, no executive needed.
libvms_add_test(test_libvms_config_logical_timezone
    ${CMAKE_CURRENT_SOURCE_DIR}/test_config_logical_timezone.c)

# vms-e48 -- per-user identity logicals (SYS$LOGIN / SYS$LOGIN_DEVICE /
# SYS$SCRATCH) are REAL LNM$PROCESS logicals sourced from the SYSUAF record,
# not Linux env vars, so F$TRNLNM returns the user's home instead of the
# generic [USERS] default; and the SYSUAF LGICMD field is honoured over the
# old hardcoded /LOGIN.COM. Needs vmslnm (lnm_define_login_logicals /
# lnm_translate) alongside vms (sysuaf parse + login command file), so it is
# not built via libvms_add_test(), same reason test_libvms_identity above isn't.
add_executable(test_libvms_login_logicals
    ${CMAKE_CURRENT_SOURCE_DIR}/test_login_logicals.c)
target_include_directories(test_libvms_login_logicals PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
)
target_link_libraries(test_libvms_login_logicals PRIVATE
    vms
    vmslnm
    pthread
    m
)
add_test(NAME test_libvms_login_logicals
         COMMAND test_libvms_login_logicals)

# vms-8c1 -- CLI$ callable interface (cli$dcl_parse / cli$present /
# cli$get_value) + compiled-CLD support (cli$compile_cld). Compiles the REAL
# vendored MMK command table (tests/corpus/tier3-mmk/mmk_cld.cld) and drives it
# with MMK's exact call idioms. Unblocks native MMK.EXE (vms-ec70).
libvms_add_test(test_libvms_lib_cli
    ${CMAKE_CURRENT_SOURCE_DIR}/test_lib_cli.c)
# NB: pass the path UNQUOTED and stringize it in the test (see MMK_CLD_PATH
# there). A quoted -D value would emit \" backslash escapes into
# compile_commands.json, which the kif_caller_census reader rejects.
target_compile_definitions(test_libvms_lib_cli PRIVATE
    MMK_CLD_PATH=${CMAKE_SOURCE_DIR}/tests/corpus/tier3-mmk/mmk_cld.cld)

# vms-947 -- sys$setddir: set/read the process default directory string, the
# [dir] used to resolve relative file specs. Proves the change lands in the PCB
# store sys$assign reads (SYS$DISK / relative resolution) and round-trips
# through the service's own old-directory out-arguments. Unblocks native MMK.
libvms_add_test(test_libvms_setddir
    ${CMAKE_CURRENT_SOURCE_DIR}/test_setddir.c)

# vms-f16 -- conformance-gap constants (docs/conformance-gap-report.md §3.3):
# TRUE/FALSE + the SS$_/RMS$_/XAB$/CHP$M/ISS$C and OVMX-private SYI$_/FAB$M_/
# OSS$M_ constants that were defined nowhere in OVMX's headers. Compile-time
# _Static_asserts pin every VMS-authentic value to the reference oracle (see the
# file header) and guard the OVMX-private values against drift. Pure header
# test -- needs both the libvms and vmsrms (rms/fab.h, rms/xab.h) include dirs,
# so not via libvms_add_test().
add_executable(test_libvms_conformance_constants
    ${CMAKE_CURRENT_SOURCE_DIR}/test_conformance_constants.c)
target_include_directories(test_libvms_conformance_constants PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_RMS_INCLUDE_DIR}
)
add_test(NAME test_libvms_conformance_constants
         COMMAND test_libvms_conformance_constants)

# vms-0d75 -- reflexive Ctrl/T status line. Compiles the PRODUCTION renderer
# (src/vmsdcl/dcl_terminal.c) directly and drives dcl_format_ctrl_t_status()
# against a struct vms_procinfo -- the executive's real $GETJPI contract --
# so it proves the format and real-field consumption at the true data
# boundary, no monkeypatch. dcl_terminal.c needs the DCL, libvms and
# libvmssys (vms_kif.h -> vms_ioctl.h) include dirs; the vms lib supplies its
# incidental vms_kif_procscan() reference. _DEFAULT_SOURCE for localtime_r.
add_executable(test_libvms_ctrl_t_status
    ${CMAKE_CURRENT_SOURCE_DIR}/test_ctrl_t_status.c
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/dcl_terminal.c)
target_include_directories(test_libvms_ctrl_t_status PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/include
    ${VMS_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/src/libvmssys
)
target_compile_definitions(test_libvms_ctrl_t_status PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)
target_link_libraries(test_libvms_ctrl_t_status PRIVATE vms pthread m)
add_test(NAME test_libvms_ctrl_t_status
         COMMAND test_libvms_ctrl_t_status)
