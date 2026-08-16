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

# vms-af7a (epic vms-5eb, R2 -- the ODS-2 runtime flip): the RMS SYS$DISK
# WORKING-COPY model (src/vmsrms/rms_core.c rms_sysdisk_checkout/_checkin).
# Drives the real RMS API (sys$open/$create/$connect/$get/$put/$close) against
# files on a GENUINE-ODS-2 SYS$DISK registered as DKA0:, proving: $OPEN checks
# the file out into a memfd whose bytes are byte-identical (seq $GET matches a
# POSIX read of the same bytes), a write-open+$PUT+$CLOSE checks the copy back
# in as a NEW ODS-2 version (prior version survives), a read-only close mints no
# version, ods2_sysdisk_owns_path routing, and fail-honest (no volume -> $OPEN
# fails, no POSIX fallback). Links vmsfs_volume for the ODS-2 adapters (the same
# distinct static module rms_core.c consumes for the flip -- vms-5eb).
add_executable(test_rms_sysdisk_workcopy test_rms_sysdisk_workcopy.c)
target_include_directories(test_rms_sysdisk_workcopy PRIVATE
    ${VMS_RMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)
target_link_libraries(test_rms_sysdisk_workcopy PRIVATE
    vmsrms vmsfs vmslnm vmsfs_volume pthread)
add_test(NAME vmsrms_sysdisk_workcopy COMMAND test_rms_sysdisk_workcopy)
set_tests_properties(vmsrms_sysdisk_workcopy PROPERTIES
    LABELS "unit;rms;ods2;vmsfs"
    TIMEOUT 30
)
