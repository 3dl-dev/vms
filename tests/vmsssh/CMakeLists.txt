# tests/vmsssh/CMakeLists.txt
# SSH integration tests for vmssshd

# ------------------------------------------------------------------ #
# test_term_mapping — unit test for TERM → VMS device type mapping    #
# ------------------------------------------------------------------ #
add_executable(test_term_mapping test_term_mapping.c)
add_test(NAME term_mapping
    COMMAND test_term_mapping
)
set_tests_properties(term_mapping PROPERTIES
    LABELS "unit;ssh"
    TIMEOUT 10
)

# ------------------------------------------------------------------ #
# test_ssh_client — libssh-based SSH test client                      #
# ------------------------------------------------------------------ #
find_library(LIBSSH_LIBRARY NAMES ssh)
find_path(LIBSSH_INCLUDE_DIR NAMES libssh/libssh.h)

if(LIBSSH_LIBRARY AND LIBSSH_INCLUDE_DIR)
    add_executable(test_ssh_client test_ssh_client.c)

    target_include_directories(test_ssh_client PRIVATE
        ${LIBSSH_INCLUDE_DIR}
    )

    target_link_libraries(test_ssh_client PRIVATE
        ${LIBSSH_LIBRARY}
    )

    set(SSH_CLIENT_PATH $<TARGET_FILE:test_ssh_client>)
    set(SSH_CLIENT_AVAILABLE TRUE)
else()
    message(STATUS "libssh not found — test_ssh_client will not be built")
    set(SSH_CLIENT_AVAILABLE FALSE)
endif()

# ------------------------------------------------------------------ #
# vmsssh_integration — full Docker-based integration test             #
# ------------------------------------------------------------------ #
# Only register the integration test when test_ssh_client is available,
# so that running ctest without Docker still produces a clear skip rather
# than an unexplained failure due to a missing client binary.
if(SSH_CLIENT_AVAILABLE)
    add_test(NAME vmsssh_integration
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_vmsssh.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(vmsssh_integration PROPERTIES
        LABELS "integration;ssh;docker"
        TIMEOUT 180
        SKIP_RETURN_CODE 77
        ENVIRONMENT "TEST_CLIENT=${SSH_CLIENT_PATH}"
    )
endif()
