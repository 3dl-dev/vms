# tests/vmsrms - Unit tests for vmsrms (Record Management Services)

add_executable(test_vmsrms test_vmsrms.c)

target_include_directories(test_vmsrms PRIVATE
    ${VMS_RMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)

target_link_libraries(test_vmsrms PRIVATE vmsrms vmsfs vmslnm pthread)

add_test(NAME vmsrms_unit COMMAND test_vmsrms)
set_tests_properties(vmsrms_unit PROPERTIES
    LABELS "unit;rms"
    TIMEOUT 30
)

# vms-5f0: genuine Files-11 Prolog-3 indexed READ over the ACP block substrate.
# Authors a real Prolog-3 image (fixed prolog + key descriptor + single-level
# key-of-reference index bucket + two data buckets) and reads records BY PRIMARY
# KEY through the on-disk index engine (rms_prolog3.c) -- no .rms_idx sidecar, no
# flat scan. Host-only: the engine reads blocks via rms_io_read_exact wrapping a
# plain fd (no /dev/vms); the /dev/vms ACP end-to-end is the paired positive
# pending the write rung (vms-045). Links libvmsrms (which resolves the ACP
# block symbols); the test drives rms_prolog3.c through its public API on the
# POSIX rms_io backend.
add_executable(test_prolog3_read test_prolog3_read.c)
target_include_directories(test_prolog3_read PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsrms
    ${VMS_RMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)
target_link_libraries(test_prolog3_read PRIVATE vmsrms vmsfs vmslnm pthread)
add_test(NAME vmsrms_prolog3_read COMMAND test_prolog3_read)
set_tests_properties(vmsrms_prolog3_read PROPERTIES
    LABELS "unit;rms;ods2"
    TIMEOUT 30
)

# vms-045: genuine Files-11 Prolog-3 indexed WRITE over the ACP block substrate.
# The paired write half of test_prolog3_read: authors a real Prolog-3 file with
# the runtime write engine (rms_p3_create/put/update), $PUTs enough records to
# FORCE a genuine data-bucket SPLIT (new bucket + RRV + index-pointer update),
# then reads them ALL back BY KEY through the read engine (rms_p3_get_by_key) --
# proving writer<->reader agreement across a split. No .rms_idx sidecar, no /vms
# write, no flat file. Host-only: the engine writes blocks via rms_io_write_exact
# wrapping a plain fd (no /dev/vms); the /dev/vms ACP end-to-end is the paired
# positive tests/qemu/test_syssvc_rms_p3_acp.c.
add_executable(test_prolog3_write test_prolog3_write.c)
target_include_directories(test_prolog3_write PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsrms
    ${VMS_RMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)
target_link_libraries(test_prolog3_write PRIVATE vmsrms vmsfs vmslnm pthread)
add_test(NAME vmsrms_prolog3_write COMMAND test_prolog3_write)
set_tests_properties(vmsrms_prolog3_write PROPERTIES
    LABELS "unit;rms;ods2"
    TIMEOUT 30
)

# vms-2ae: genuine Files-11 Prolog-3 SECONDARY key / SIDR. Models SYSUAF
# (primary = 32-byte username, secondary key-of-reference 1 = 4-byte UIC with
# duplicates allowed). Defines the secondary index (rms_p3_add_secondary_key),
# $PUTs records past both a PRIMARY data-bucket split and a SIDR-bucket split,
# then reads records BY THE SECONDARY KEY -- descending the secondary index to
# the SIDR, taking the primary RFA, and resolving the primary record byte-exact
# (a real secondary index, NOT a flat scan). Covers a 3-member duplicate group,
# RFA stability across the primary split, and $DELETE purging the SIDR. Host-only
# (POSIX rms_io backend); the /dev/vms ACP positive is
# tests/qemu/test_syssvc_rms_p3_acp.c.
add_executable(test_prolog3_seckey test_prolog3_seckey.c)
target_include_directories(test_prolog3_seckey PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsrms
    ${VMS_RMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)
target_link_libraries(test_prolog3_seckey PRIVATE vmsrms vmsfs vmslnm pthread)
add_test(NAME vmsrms_prolog3_seckey COMMAND test_prolog3_seckey)
set_tests_properties(vmsrms_prolog3_seckey PROPERTIES
    LABELS "unit;rms;ods2"
    TIMEOUT 30
)

# vms-5c6d: indexed sys$close must flush the in-memory B-tree before freeing it.
# Regression for a Tier-1 silent-data-loss bug where records inserted since the
# last periodic index save (rms_idx_put persists only every 100 inserts) were
# dropped on close while sys$close still returned success. PUTs 250 records,
# closes, reopens, and keyed-$GETs records past the 200-insert save boundary.
# Host-only: the indexed B-tree is a userspace, file-backed structure.
add_executable(test_idx_close_flush test_idx_close_flush.c)
target_include_directories(test_idx_close_flush PRIVATE
    ${VMS_RMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)
target_link_libraries(test_idx_close_flush PRIVATE vmsrms vmsfs vmslnm pthread)
add_test(NAME vmsrms_idx_close_flush COMMAND test_idx_close_flush)
set_tests_properties(vmsrms_idx_close_flush PROPERTIES
    LABELS "unit;rms"
    TIMEOUT 30
)

# vms-d8e: concealed + rooted-directory device logicals through $PARSE
# (NAM$M_CNCL_DEV / NAM$M_ROOT_DIR) and structural rooted composition. Runs on
# the host against LNM$PROCESS (genuinely process-private, no /dev/vms needed).
add_executable(test_concealed_rooted test_concealed_rooted.c)
target_include_directories(test_concealed_rooted PRIVATE
    ${VMS_RMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)
target_link_libraries(test_concealed_rooted PRIVATE vmsrms vmsfs vmslnm pthread)
add_test(NAME vmsrms_concealed_rooted COMMAND test_concealed_rooted)
set_tests_properties(vmsrms_concealed_rooted PROPERTIES
    LABELS "unit;rms;lnm"
    TIMEOUT 30
)

# vms-f83: MOUNT disk transparency -- DISK$<volume-label> (Part 1) and a
# concealed rooted device addressed as a virtual disk root, FOO:[000000]
# (Part 2, composing on vms-d8e). Compiles dcl_disk_logical.c (the exact
# helpers cmd_mount()/cmd_dismount() call) directly, so the DISK$ naming /
# DEFINE / removal is exercised on the host against LNM$PROCESS, where ctest
# runs and no /dev/vms exists. The full MOUNT -> mount(2) -> DISK$ path is the
# paired positive in tests/qemu/test_mount_e2e.sh.
add_executable(test_disk_transparency
    test_disk_transparency.c
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/dcl_disk_logical.c
)
target_include_directories(test_disk_transparency PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsdcl/include
    ${VMS_RMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)
target_link_libraries(test_disk_transparency PRIVATE vmsrms vmsfs vmslnm pthread)
add_test(NAME vmsrms_disk_transparency COMMAND test_disk_transparency)
set_tests_properties(vmsrms_disk_transparency PROPERTIES
    LABELS "unit;rms;lnm;dcl"
    TIMEOUT 30
)

# vms-69e7: SYS$SYSROOT/SYS$SYSTEM composed live through the concealed rooted
# search-list machinery (no pre-flattened paths). Host LNM$PROCESS + a temp
# physical device, no /dev/vms needed.
add_executable(test_sysroot_composed test_sysroot_composed.c)
target_include_directories(test_sysroot_composed PRIVATE
    ${VMS_RMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)
target_link_libraries(test_sysroot_composed PRIVATE vmsrms vmsfs vmslnm pthread)
add_test(NAME vmsrms_sysroot_composed COMMAND test_sysroot_composed)
set_tests_properties(vmsrms_sysroot_composed PROPERTIES
    LABELS "unit;rms;lnm"
    TIMEOUT 30
)

# vms-0044: concealed-rooted directory-logical chain composes to concrete
# on-volume ODS-2 candidate specs (SYS$SYSTEM:SYSUAF.DAT ->
# DKA0:[SYS0.SYSCOMMON.SYSEXE]SYSUAF.DAT) -- the input the Files-11 ACP
# directory walk consumes (flip-blocker, epic vms-208). Host LNM$PROCESS, no
# /dev/vms needed; the ACP walk + byte-exact read is the paired positive in
# tests/qemu/test_syssvc_dirlogical_acp.c.
add_executable(test_dirlogical_compose test_dirlogical_compose.c)
target_include_directories(test_dirlogical_compose PRIVATE
    ${VMS_RMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)
target_link_libraries(test_dirlogical_compose PRIVATE vmsrms vmsfs vmslnm pthread)
add_test(NAME vmsrms_dirlogical_compose COMMAND test_dirlogical_compose)
set_tests_properties(vmsrms_dirlogical_compose PROPERTIES
    LABELS "unit;rms;lnm"
    TIMEOUT 30
)

# vms-240: iterative device-field LNM resolution through $PARSE and the unified
# TERMINAL-honoring driver (lnm_translate_filespec). Host LNM$PROCESS, no
# /dev/vms needed.
add_executable(test_iterative_translation test_iterative_translation.c)
target_include_directories(test_iterative_translation PRIVATE
    ${VMS_RMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)
target_link_libraries(test_iterative_translation PRIVATE vmsrms vmsfs vmslnm pthread)
add_test(NAME vmsrms_iterative_translation COMMAND test_iterative_translation)
set_tests_properties(vmsrms_iterative_translation PROPERTIES
    LABELS "unit;rms;lnm"
    TIMEOUT 30
)

# vms-3e0: genuine binary $RDBDEF RIGHTSLIST over the Files-11 Prolog-3 indexed
# engine (the binary sibling of the SYSUAF rung). Authors a real 2-key Prolog-3
# RIGHTSLIST (primary key = identifier VALUE @0x00, secondary key = identifier
# NAME @0x10) with the ORACLE's real identifiers (environmental IDs at
# %X8000000x, the SYSTEM/DEFAULT UIC identifiers, plus the RESOURCE identifier
# OVMXRES), re-binds the on-disk prologue, then reads every identifier BACK BY
# NAME (secondary key -> SIDR -> RFA) AND BY VALUE (primary key), asserting the
# record fields land at the exact oracle offsets (value@0x00, attr@0x04,
# holder@0x08==0, name@0x10) byte-exact, plus a value->name resolution. 48-byte
# records + 1-block buckets force a genuine data-bucket + SIDR split (a real
# index, NOT a flat scan). No ASCII colon format, no flat file, no /vms. The
# holder records + HOLDER key are a labelled deferred seam (rightslist_rms.c).
add_executable(test_rightslist_rms test_rightslist_rms.c)
target_include_directories(test_rightslist_rms PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsrms
    ${VMS_RMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)
target_link_libraries(test_rightslist_rms PRIVATE vmsrms vmsfs vmslnm pthread)
add_test(NAME vmsrms_rightslist_rms COMMAND test_rightslist_rms)
set_tests_properties(vmsrms_rightslist_rms PROPERTIES
    LABELS "unit;rms;ods2"
    TIMEOUT 30
)

# vms-f15a: the RIGHTSLIST ATOMIC FLIP proof. Authors a binary $RDBDEF
# RIGHTSLIST with the seed/runtime engine and resolves identifiers through the
# LIVE $ASCTOID/$IDTOASC seam (rightslist_live_*_rf, the testable core the
# runtime executive entry points delegate to): name->value and value->name for
# every seeded identifier read from the real binary record, case folding, and
# fail-honest SS$_NOSUCHID on an absent id -- no ASCII, no /vms. The /dev/vms
# ACP end-to-end (unprivileged resolve out of the WORLD:none rights database
# over the executive channel) is the qemu e2e gate (test_syssvc_ident).
add_executable(test_rightslist_live test_rightslist_live.c)
target_include_directories(test_rightslist_live PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsrms
    ${VMS_RMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)
target_link_libraries(test_rightslist_live PRIVATE vmsrms vmsfs vmslnm pthread)
add_test(NAME vmsrms_rightslist_live COMMAND test_rightslist_live)
set_tests_properties(vmsrms_rightslist_live PROPERTIES
    LABELS "unit;rms;ods2"
    TIMEOUT 30
)

# test_sysuaf_live (vms-d92): the SYSUAF ATOMIC FLIP proof. Authors a binary
# $UAFDEF SYSUAF with the seed/runtime code path (sysuaf_view_to_raw +
# sysuaf_set_password Purdy hash + sysuaf_put_record) and authenticates SYSTEM/
# MANAGER and GUEST/GUEST by reading the real binary record back through the
# indexed engine + Purdy-verifying -- no ASCII, no SHA-256. Links vms
# (sysuaf.c view API) transitively through vmsrms.
add_executable(test_sysuaf_live test_sysuaf_live.c)
target_include_directories(test_sysuaf_live PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsrms
    ${VMS_RMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)
target_link_libraries(test_sysuaf_live PRIVATE vmsrms vmsfs vmslnm pthread)
add_test(NAME vmsrms_sysuaf_live COMMAND test_sysuaf_live)
set_tests_properties(vmsrms_sysuaf_live PROPERTIES
    LABELS "unit;rms;ods2;sysuaf"
    TIMEOUT 30
)
