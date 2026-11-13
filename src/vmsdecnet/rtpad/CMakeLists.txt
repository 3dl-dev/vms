# RTPAD.EXE -- the remote terminal pad behind $ SET HOST (rd vms-0a7 S2,
# vms-18c9). DCL's command tables send SET HOST to SYS$SYSTEM:RTPAD.EXE, which
# reads NODE and /USERNAME with CLI$GET_VALUE, opens the CTERM link (object 42)
# through _NET: ($QIO, brokered by NETACP) and carries the terminal over it with
# $QIO on the terminal channel. It replaces the retired DECnet daemon's former --set-host
# client role; no argv, no stdio, no poll, no thread.
#
# rtpad_session: the link- and terminal-independent CTERM client protocol (the
# host tests drive it over a replayed real-VAX session).
add_library(rtpad_session STATIC rtpad_session.c)
target_include_directories(rtpad_session PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/libvms/include)           # ssdef.h, rmsdef.h
target_link_libraries(rtpad_session PUBLIC vmsdecnet_cterm)

add_executable(rtpad_exe rtpad.c)
target_include_directories(rtpad_exe PRIVATE
    ${CMAKE_SOURCE_DIR}/src/libvms/include
    ${CMAKE_SOURCE_DIR}/src/libvmssys)
target_link_libraries(rtpad_exe PRIVATE rtpad_session vmsdecnet_broker vms vmssys)
set_target_properties(rtpad_exe PROPERTIES
    OUTPUT_NAME "RTPAD"
    SUFFIX ".EXE"
)
install(TARGETS rtpad_exe RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
