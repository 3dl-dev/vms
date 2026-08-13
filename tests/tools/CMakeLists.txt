# tests/tools/CMakeLists.txt
#
# Unit tests for the tools/ utilities (LOGINOUT, MAIL, ...).

cmake_minimum_required(VERSION 3.16)

# LOGINOUT session-sequence fidelity (vms-417). Links only the pure display
# TU -- no VMS runtime needed -- so it runs fast and deterministically under
# ctest. Source paths are passed in so the source-guard assertions can read
# the emitters they protect.
# Source paths for the source-guard assertions travel through a generated
# header, NOT -D string definitions: a -DFOO="..." serialises into
# compile_commands.json with backslash escapes the kif_caller_census reader
# cannot parse (vms-417).
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/test_paths.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/test_paths.h
    @ONLY
)

add_executable(test_loginout_display
    ${CMAKE_CURRENT_SOURCE_DIR}/test_loginout_display.c
    ${CMAKE_SOURCE_DIR}/tools/loginout_display.c
)

target_include_directories(test_loginout_display PRIVATE
    ${CMAKE_SOURCE_DIR}/tools
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_compile_definitions(test_loginout_display PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_compile_options(test_loginout_display PRIVATE -Wall -Wextra)

add_test(NAME test_loginout_display COMMAND test_loginout_display)
