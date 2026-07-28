# vmsscs - SCS Datalink daemon (SCSD)
#
# Greenfield AF_PACKET raw-socket datalink for the VMScluster wire protocol
# (docs/design-cluster-node.md sec 3.1). This is the SCSD skeleton: it opens
# a raw socket on ethertype 0x6007 (DEC SCA/LAVC), receives frames, and
# classifies them by the GROUNDED length rule (docs/cluster-protocol-
# spec.md sec 2). RECEIVE-ONLY -- no NISCA virtual circuits, no SCS
# connection multiplexing, no frame emission (see vms-b62 for emission).
#
# No dependency on any other OVMX library -- plain POSIX sockets.

add_library(vmsscs_classify STATIC scs_classify.c)
target_include_directories(vmsscs_classify PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(scsd_exe scsd.c)
target_include_directories(scsd_exe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(scsd_exe PRIVATE vmsscs_classify)
set_target_properties(scsd_exe PROPERTIES
    OUTPUT_NAME "SCSD"
    SUFFIX ".EXE"
)
install(TARGETS scsd_exe RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
