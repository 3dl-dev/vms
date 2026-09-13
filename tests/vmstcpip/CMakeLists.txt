# tests/vmstcpip - host unit/fuzz tests for the TCP/IP services headers.
#
# rd vms-deb3: never-crash / no-over-read fuzz of the TCPIP$SERVICE.DAT parsers
# (tcpip_inetd_parse_db in services/tcpip_inetd.h; tcpip_svcdb_parse_rec +
# format_rec in mgmt/tcpip_service_db.h). Both are static-inline header
# functions that parse an untrusted-ish, DCL-writable config into fixed buffers,
# so the fuzz TU compiles them straight in -- their heavy listen/spawn/file-I/O
# siblings are unused static inlines and are never emitted, so no library link.
# The include dirs satisfy the headers' RMS/bgsock declaration chain (nothing
# from it is called). Built WITH -fsanitize=address,undefined where available so
# an over-read is a HARD failure; the fuzz's exact-sized-buffer + non-vacuity
# assertions run everywhere.
add_executable(test_tcpip_svcdb_fuzz test_tcpip_svcdb_fuzz.c)
target_include_directories(test_tcpip_svcdb_fuzz PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmstcpip/services
    ${CMAKE_SOURCE_DIR}/src/vmstcpip/mgmt
    ${CMAKE_SOURCE_DIR}/src/vmstcpip/sockets
    ${CMAKE_SOURCE_DIR}/src/libvms/include
    ${CMAKE_SOURCE_DIR}/src/vmsrms/include
    ${CMAKE_SOURCE_DIR}/src/vmsrms/include/rms)
target_compile_options(test_tcpip_svcdb_fuzz PRIVATE -Werror)

include(CheckCSourceCompiles)
set(_tcpfz_save "${CMAKE_REQUIRED_FLAGS}")
set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
check_c_source_compiles("int main(void){return 0;}" OVMX_TCPIP_FUZZ_SANITIZE_OK)
set(CMAKE_REQUIRED_FLAGS "${_tcpfz_save}")
if(OVMX_TCPIP_FUZZ_SANITIZE_OK)
    target_compile_options(test_tcpip_svcdb_fuzz PRIVATE -fsanitize=address,undefined)
    target_link_options(test_tcpip_svcdb_fuzz PRIVATE -fsanitize=address,undefined)
endif()

add_test(NAME tcpip_svcdb_fuzz COMMAND test_tcpip_svcdb_fuzz)
set_tests_properties(tcpip_svcdb_fuzz PROPERTIES TIMEOUT 120)
