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
    ${CMAKE_SOURCE_DIR}/src/libvmssys
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

# rd vms-a9e7: the host-name resolver engine (services/tcpip_resolve.h). The unit
# test covers the TCPIP$HOST.DAT / TCPIP$NAMESERVICE.DAT record parsers, the RFC
# 1035 query and hand-built replies (A, CNAME->A, NXDOMAIN, TC, wrong ID, pointer
# loop / past-end, huge RDLENGTH); the fuzz mutates valid replies and holds the
# PEER-RECEIVE parser to "never reads past the datagram" under ASan/UBSan. Both
# compile the header straight in; the BGn: exchange half is unused here (the QEMU
# suite test_syssvc_bg_udp proves it against a real /dev/vms).
foreach(_t test_tcpip_resolve test_tcpip_resolve_fuzz)
    add_executable(${_t} ${_t}.c)
    target_include_directories(${_t} PRIVATE
        ${CMAKE_SOURCE_DIR}/src/vmstcpip/services
        ${CMAKE_SOURCE_DIR}/src/libvms/include
        ${CMAKE_SOURCE_DIR}/src/libvmssys)
    target_compile_options(${_t} PRIVATE -Werror)
    if(OVMX_TCPIP_FUZZ_SANITIZE_OK)
        target_compile_options(${_t} PRIVATE -fsanitize=address,undefined)
        target_link_options(${_t} PRIVATE -fsanitize=address,undefined)
    endif()
endforeach()
add_test(NAME tcpip_resolve COMMAND test_tcpip_resolve)
add_test(NAME tcpip_resolve_fuzz COMMAND test_tcpip_resolve_fuzz)
set_tests_properties(tcpip_resolve tcpip_resolve_fuzz PROPERTIES TIMEOUT 120)

# rd vms-d0c4: the TELNET/FTP client's peer-receive parsers (tcpip_client.h:
# tcpip_telnet_filter, tcpip_ftp_parse_pasv) -- bytes a server outside the VM
# controls. Same bar and same sanitizer wiring as the SERVICE.DAT fuzz above.
add_executable(test_tcpip_client_fuzz test_tcpip_client_fuzz.c)
target_include_directories(test_tcpip_client_fuzz PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmstcpip/services
    ${CMAKE_SOURCE_DIR}/src/libvms/include
    ${CMAKE_SOURCE_DIR}/src/libvmssys)
# -Wno-comment: starlet.h pulls opcdef.h, whose comment nests a "/*".
target_compile_options(test_tcpip_client_fuzz PRIVATE -Werror -Wno-comment)
if(OVMX_TCPIP_FUZZ_SANITIZE_OK)
    target_compile_options(test_tcpip_client_fuzz PRIVATE -fsanitize=address,undefined)
    target_link_options(test_tcpip_client_fuzz PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME tcpip_client_fuzz COMMAND test_tcpip_client_fuzz)
set_tests_properties(tcpip_client_fuzz PROPERTIES TIMEOUT 120)
