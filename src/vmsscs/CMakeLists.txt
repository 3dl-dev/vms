# vmsscs - SCS Datalink daemon (SCSD)
#
# Greenfield AF_PACKET raw-socket datalink for the VMScluster wire protocol
# (docs/design-cluster-node.md sec 3.1). vms-294 gave this the receive +
# length-classify skeleton. vms-b62 adds the transmit path: scs_hello.c
# builds a spec-valid multicast HELLO (docs/cluster-protocol-spec.md sec
# 4a/4b) and scsd.c gains an --emit-hello beacon mode.
#
# No dependency on any other OVMX library for the classify/hello builders
# (plain POSIX + stdint); scsd_exe additionally pulls in src/libvms/include
# header-only readers (sysgen_params.h, cluster_authorize.h) for node/group
# identity -- no link dependency, they are static inline.

add_library(vmsscs_classify STATIC scs_classify.c)
target_include_directories(vmsscs_classify PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(vmsscs_hello STATIC scs_hello.c)
target_include_directories(vmsscs_hello PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# vms-5fe - VMS$VAXcluster SCS connect handshake (directed HELLO reply lives
# in vmsscs_hello; the connect-request/response builder + parser is here).
add_library(vmsscs_connect STATIC scs_connect.c)
target_include_directories(vmsscs_connect PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# vms-21e - phase-2 START/config (0x41) builder + SCS seq/ack state machine
# (the last step before the VAX binds the VMS$VAXcluster SCS connection).
add_library(vmsscs_start STATIC scs_start.c)
target_include_directories(vmsscs_start PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# vms-691 - SCS sequenced-message VC engine: 0x48 credit-return builder
# (spec sec 4h) + seq/ack tracking + retransmit trigger. Keeps the NISCA VC
# alive so the VAX stops logging "Excessive packet losses / Closed Virtual
# Circuit". Depends on vmsscs_start for the scs_seq_state helpers.
add_library(vmsscs_vc STATIC scs_vc.c)
target_include_directories(vmsscs_vc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vmsscs_vc PUBLIC vmsscs_start)

# vms-246 - SCS$DIRECTORY connect + SCS$DIR_LOOKUP responder: binds the
# SCS$DIRECTORY Con.ID pair and answers the established node's SYSAP-name
# lookups (spec sec 4h) so the VAX advances to the VMS$VAXcluster 0x4b connect.
# Depends on vmsscs_connect for the shared SCS envelope constants.
add_library(vmsscs_dir STATIC scs_dir.c)
target_include_directories(vmsscs_dir PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vmsscs_dir PUBLIC vmsscs_connect)

# vms-224 - VMS$VAXcluster connection-manager add-member transaction on the
# 190-byte VC (spec sec 4j): builds the op 0x14/0x01/0x02 config frames the
# joiner sends + the 0x81 responses to the member-driven 0x03/0x05 commit/
# lock-rebuild transactions. This is what promotes OVMX to a full cluster
# MEMBER (a CSB in SDA SHOW CLUSTER). Depends on nothing else in vmsscs
# (plain POSIX + stdint template builders).
add_library(vmsscs_member STATIC scs_member.c)
target_include_directories(vmsscs_member PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(scsd_exe scsd.c)
target_include_directories(scsd_exe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/libvms/include
)
target_link_libraries(scsd_exe PRIVATE vmsscs_classify vmsscs_hello vmsscs_connect vmsscs_dir vmsscs_start vmsscs_vc vmsscs_member)
set_target_properties(scsd_exe PROPERTIES
    OUTPUT_NAME "SCSD"
    SUFFIX ".EXE"
)
install(TARGETS scsd_exe RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
