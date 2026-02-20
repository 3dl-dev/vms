add_library(vms ${OVMX_LIB_TYPE}
    descrip.c
    status.c
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
    rtl/lib_vm.c
    rtl/lib_output.c
    rtl/lib_signal.c
    rtl/lib_datetime.c
    rtl/lib_misc.c
    rtl/lib_string_ops.c
    rtl/lib_bitops.c
    rtl/lib_eventflags.c
    rtl/str_routines.c
    rtl/mth_routines.c
    rtl/ots_routines.c
    syssvc/sys_float.c
)
target_include_directories(vms PUBLIC include ${VMS_LNM_INCLUDE_DIR} ${VMS_PROCESS_INCLUDE_DIR})
target_link_libraries(vms PUBLIC vmsprocess pthread m)
if(NOT OVMX_STATIC)
    set_target_properties(vms PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0)
endif()
install(TARGETS vms LIBRARY DESTINATION lib)
install(DIRECTORY include/ DESTINATION include/vms)
