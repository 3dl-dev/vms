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
#
# vms-4071 also puts the VIRTUAL-CIRCUIT FORMATION state machine here (the
# CLOSED / START SENT / START RECEIVED / OPEN dialogue of VAXcluster Principles
# pp. 2-12..2-16, with the p. 2-14 acceptable-response table, the reissue timer,
# the OS-dependent retry limit and the p. 2-16 implied ACK). That machine's
# state lives in the Path Block, hence the vmsscs_config dependency.
add_library(vmsscs_vc STATIC scs_vc.c)
target_include_directories(vmsscs_vc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vmsscs_vc PUBLIC vmsscs_start vmsscs_config)

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

# vms-7be - SCA System Blocks / Path Blocks / Port Descriptor Tables and the
# VMS SCA Configuration Queue (VAXcluster Principles ch. 2, pp. 2-11..2-21).
# Pure state: builds no frame, parses no frame, opens no socket -- it replaces
# the ad-hoc MAC-keyed peer struct with the architected structures.
add_library(vmsscs_config STATIC scs_config.c)
target_include_directories(vmsscs_config PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# vms-e1a - SCA Connection Descriptor Tables, the Connection Descriptor List
# that addresses them by CONID, and the per-Path-Block connection queue
# (VAXcluster Principles ch. 2, pp. 2-28..2-30, 2-35). Pure state + delivery:
# builds no frame, parses no frame, opens no socket. Depends on vmsscs_config
# for struct scs_pb / struct scs_sb (the CDT points at both).
add_library(vmsscs_cdt STATIC scs_cdt.c)
target_include_directories(vmsscs_cdt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vmsscs_cdt PUBLIC vmsscs_config)

# vms-76e - SCA message flow control: the Send / Receive / Pending Receive
# Credit account per connection, held in the CDT (VAXcluster Principles ch. 2
# sec 2.8, pp. 2-42..2-45), plus the credit field in the message header --
# grounded at SCA [48:50] from our own lab captures (see the WIRE VERDICT in
# scs_credit.h). Depends on vmsscs_cdt for struct scs_cdt / struct scs_pdt.
#
# NOT LINKED INTO scsd_exe: nothing in the daemon calls it yet, and OVMX
# therefore still emits no live credit. See the reachability note in
# scs_credit.h -- do not read a green test run as "OVMX does flow control".
add_library(vmsscs_credit STATIC scs_credit.c)
target_include_directories(vmsscs_credit PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vmsscs_credit PUBLIC vmsscs_cdt)

add_executable(scsd_exe scsd.c)
target_include_directories(scsd_exe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/libvms/include
)
target_link_libraries(scsd_exe PRIVATE vmsscs_classify vmsscs_hello vmsscs_connect vmsscs_dir vmsscs_start vmsscs_vc vmsscs_member vmsscs_config)
set_target_properties(scsd_exe PROPERTIES
    OUTPUT_NAME "SCSD"
    SUFFIX ".EXE"
)
install(TARGETS scsd_exe RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
