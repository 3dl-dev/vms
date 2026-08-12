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
