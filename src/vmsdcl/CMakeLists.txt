# vmsdcl - DCL Shell Interpreter
# Pure C implementation with hand-written recursive descent parser

add_executable(vmsdcl
    dcl_main.c
    dcl_lexer.c
    dcl_parser.c
    dcl_exec.c
    dcl_builtin.c
    dcl_symbol.c
    dcl_lexical.c
    dcl_filespec.c
    dcl_io.c
    dcl_script.c
)

target_include_directories(vmsdcl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_RMS_INCLUDE_DIR}
    ${VMS_PROCESS_INCLUDE_DIR}
)

# We need POSIX APIs (fork, sigaction, strncasecmp, realpath, etc.)
target_compile_definitions(vmsdcl PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

# Core link libraries
target_link_libraries(vmsdcl PRIVATE pthread vms vmsfs vmsprocess)

# Try to find readline for better interactive experience (skip for static builds)
if(NOT OVMX_STATIC)
    find_library(READLINE_LIB readline)
    if(READLINE_LIB)
        target_compile_definitions(vmsdcl PRIVATE HAVE_READLINE)
        target_link_libraries(vmsdcl PRIVATE ${READLINE_LIB})
    endif()
endif()

install(TARGETS vmsdcl RUNTIME DESTINATION bin)
