# tests/vmsfs - Unit tests for vmsfs (VMS filesystem semantics)

add_executable(test_vmsfs test_vmsfs.c)

target_include_directories(test_vmsfs PRIVATE
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)

target_link_libraries(test_vmsfs PRIVATE vmsfs vmslnm pthread)

add_test(NAME vmsfs_unit COMMAND test_vmsfs)
set_tests_properties(vmsfs_unit PROPERTIES
    LABELS "unit;vmsfs"
    TIMEOUT 30
)

# vms-b12: search-list device logical fan-out on file open (try each member
# in order). Runs on the host against LNM$PROCESS (genuinely process-private,
# no /dev/vms needed); the executive-resident store is proven separately by
# tests/qemu/test_syssvc_lnm_searchlist.c.
add_executable(test_searchlist_open test_searchlist_open.c)
target_include_directories(test_searchlist_open PRIVATE
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_INCLUDE_DIR}
)
target_link_libraries(test_searchlist_open PRIVATE vmsfs vmslnm pthread)
add_test(NAME vmsfs_searchlist_open COMMAND test_searchlist_open)
set_tests_properties(vmsfs_searchlist_open PROPERTIES
    LABELS "unit;vmsfs;lnm"
    TIMEOUT 30
)
