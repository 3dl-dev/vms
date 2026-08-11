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
