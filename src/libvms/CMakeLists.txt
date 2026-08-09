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
    rtl/str_util.c
)
target_include_directories(vms PUBLIC include ${VMS_RMS_INCLUDE_DIR} ${VMS_FS_INCLUDE_DIR} ${VMS_LNM_INCLUDE_DIR} ${VMS_PROCESS_INCLUDE_DIR})
# sys_imgact.c + imgact_prodreg.c consume the OVMX symbol-vector image format
# (ovmx_image.h / ovmx_symvec.h) -- the same single-source-of-truth headers
# LINK.EXE and IMGACT.EXE share (vms-db2). PRIVATE: only these TUs need them.
target_include_directories(vms PRIVATE ${CMAKE_SOURCE_DIR}/src/vmslink/include)
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
