# vmsdcl - DCL Shell Interpreter
# Pure C implementation with hand-written recursive descent parser

add_executable(vmsdcl
    dcl_main.c
    dcl_lexer.c
    dcl_parser.c
    dcl_exec.c
    dcl_backup.c
    dcl_builtin.c
    dcl_cmd_show.c
    dcl_cmd_set.c
    dcl_cmd_file.c
    dcl_cmd_process.c
    dcl_cmd_io.c
    dcl_cmd_misc.c
    dcl_disk_logical.c
    dcl_editor.c
    dcl_terminal.c
    dcl_symbol.c
    dcl_lexical.c
    dcl_filespec.c
    dcl_io.c
    dcl_script.c
    dcl_messages.c
    dcl_library.c
)

target_include_directories(vmsdcl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_RMS_INCLUDE_DIR}
    ${VMS_PROCESS_INCLUDE_DIR}
    # SHOW DEVICE reads the executive device table through the
    # kernel-interface client (vms-fb9): src/libvmssys/vms_kif.h, which
    # pulls src/kernel/vms_ioctl.h for the shared ioctl structures.
    ${CMAKE_SOURCE_DIR}/src/libvmssys
    # SHOW CLUSTER reads the SCS daemon's live membership publication
    # (vms-8d4): src/vmsscs/include/scs_membership.h -- a header-only IPC,
    # so no vmsscs link dependency is added.
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
    # LIBRARY/OBJECT (.OLB) shares the ar-container reader/writer with
    # LIBRARIAN.EXE: src/vmslink/include/ovmx_olb.h -- a header-only format
    # module, so no vmslink link dependency is added. (vms-ca9)
    ${CMAKE_SOURCE_DIR}/src/vmslink/include
)

# We need POSIX APIs (fork, sigaction, strncasecmp, realpath, etc.)
target_compile_definitions(vmsdcl PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

# Core link libraries
target_link_libraries(vmsdcl PRIVATE pthread vms vmsfs vmsprocess vmslnm vmsqueue)

# Try to find readline for better interactive experience (skip for static builds)
if(NOT OVMX_STATIC)
    find_library(READLINE_LIB readline)
    if(READLINE_LIB)
        target_compile_definitions(vmsdcl PRIVATE HAVE_READLINE)
        target_link_libraries(vmsdcl PRIVATE ${READLINE_LIB})
    endif()
endif()

set_target_properties(vmsdcl PROPERTIES
    OUTPUT_NAME "DCL"
    SUFFIX ".EXE"
)
install(TARGETS vmsdcl RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
