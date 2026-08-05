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
#
# vms-abc adds the p. 2-31 message-guarantee enforcement: the receive-side
# sequence-gap detector, the delivery-failure predicate, and scs_vc_break(),
# which walks the broken circuit's Path Block connection queue, drives every
# connection through the SCA connection state machine and notifies its SYSAP.
# Hence the vmsscs_conn dependency (and, transitively, vmsscs_cdt).
add_library(vmsscs_vc STATIC scs_vc.c)
target_include_directories(vmsscs_vc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vmsscs_vc PUBLIC vmsscs_start vmsscs_config vmsscs_conn)

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
# vms-1d2 extends it with the p. 2-45 CREDIT WAIT queue on the CDT (a starved
# sender is suspended and resumed FIFO when credit arrives, pp. 2-45..2-46) and
# the p. 2-44 special credit message trigger.
#
# LINKED INTO scsd_exe SINCE vms-17f, and reached only on the TEARDOWN side.
# `nm build/bin/SCSD.EXE | grep -c ' T scs_credit'` = 22. It arrives three ways,
# all transitive -- vmsscs_depart, vmsscs_svc, and (vms-096) vmsscs_poll -- and
# the production call sites are scs_depart.c:88, scs_svc.c:269 and
# scs_poll.c:377, all scs_credit_wait_flush(), plus scs_credit_extend() at
# scs_svc.c:250 which scsd.c only ever reaches with 0. OVMX still emits no live
# credit, enters no Credit Wait and sends no special credit message, so every
# flush drops zero waiters today.
#
# NOTE (vms-096) the comment on vmsscs_reason below said the reason codec had no
# production caller, by the same "OVMX builds no DISCONNECT_REQ" argument that
# vms-591 falsified; both entries are corrected together so the two cannot drift
# apart again. See the reachability notes in scs_credit.h -- do not read a green
# test run as "OVMX does flow control".
add_library(vmsscs_credit STATIC scs_credit.c)
target_include_directories(vmsscs_credit PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vmsscs_credit PUBLIC vmsscs_cdt)

# vms-17f - ORDERED PATH-BLOCK TEARDOWN ON PEER DEPARTURE. Sits at the TOP of
# the config <- cdt <- credit dependency chain, because it is the only place
# from which the whole p. 2-28 sequence can be run: notify the SYSAPs on the
# circuit, flush their Credit Waits, release their CDTs (returning the MFREEQ
# share AND, since vms-b1d, the DFREEQ share to the port), and only then close
# the Path Block. Pure state: builds no
# frame, parses no frame, opens no socket. Carries the OVMX_NO_PEER_DEPART kill
# switch and the measured listen timeout.
add_library(vmsscs_depart STATIC scs_depart.c)
target_include_directories(vmsscs_depart PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vmsscs_depart PUBLIC vmsscs_credit)

# vms-dd5 - THE SCA CONNECTION STATE MACHINE (VAXcluster Principles ch. 2,
# Figures 2-14/2-15/2-16, pp. 2-22..2-27). Pure state: a static const transition
# table, one store into struct scs_cdt's conn_state, and a transition log. Builds
# no frame, parses no frame, opens no socket. Depends on vmsscs_cdt because the
# state it drives is the CDT field vms-e1a reserved for it.
add_library(vmsscs_conn STATIC scs_conn.c)
target_include_directories(vmsscs_conn PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vmsscs_conn PUBLIC vmsscs_cdt)

# vms-6b3 - THE 16-BIT REJECT/DISCONNECT REASON CODE (VAXcluster Principles
# p. 2-26). Two bytes and their kill switch: no allocation, no dependency on any
# other vmsscs library.
#
# BOTH HALVES ARE LIVE (corrected vms-096; this said "the send half is a tested
# codec with no production caller because OVMX builds neither REJECT_REQ nor
# DISCONNECT_REQ", which stopped being true at vms-591). RECEIVE: scsd.c decodes
# and logs the field. SEND: scs_disc.c:138 calls scs_reason_put() inside
# scs_disc_build_request(), driven from scsd.c with SCS_REASON_SYSAP_SHUTDOWN
# and SCS_REASON_PEER_DISCONNECT -- and those values are on a real VAX's wire
# (6x reason 5, 4x reason 7 over the lab-2 vaxlab-4 captures, against 0 in every
# VMS-origin frame we hold).
#
# vmsscs_disc therefore links vmsscs_reason, which is why THIS entry must stay
# above it. Both the byte OFFSET and the code VALUES are LABELED OVMX design
# choices -- see scs_reason.h and docs/cluster-protocol-spec.md sec 5.
add_library(vmsscs_reason STATIC scs_reason.c)
target_include_directories(vmsscs_reason PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# vms-591 - THE DISCONNECT DIALOGUE ON THE WIRE (VAXcluster Principles Figure
# 2-16, pp. 2-26/2-27). Two frame builders and their kill switch: the 62-byte
# DISCONNECT_REQ and the 58-byte DISCONNECT_RSP, both from byte-exact captured
# templates. Grounding the RESPONSE is the part that was thought impossible --
# docs/cluster-protocol-spec.md sec 4(h)(1a) said message types 5 and 7 do not
# appear on our wire and "do not build a 5 or 7 frame on this section". They do
# appear: every census had been restricted to SCA length classes {62, 66, 110}
# and both RESPONSE messages are 58 bytes. vms-591 corrects the spec.
#
# Depends on vmsscs_reason for the ONE definition of the p. 2-26 reason code's
# payload offset -- respelling the constant here is how the two would drift.
add_library(vmsscs_disc STATIC scs_disc.c)
target_include_directories(vmsscs_disc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vmsscs_disc PUBLIC vmsscs_reason)

# vms-b1d - SCA DATAGRAM BUFFER MANAGEMENT: the per-port Datagram Free Queue
# (DFREEQ) and the per-connection datagram buffer count in the CDT (VAXcluster
# Principles ch. 2 sec 2.8, pp. 2-42..2-43, 2-45). Pure local accounting: builds
# no frame, parses no frame, changes no transmitted byte. Depends on vmsscs_cdt
# for struct scs_cdt / struct scs_pdt.
#
# NOT flow control -- p. 2-42: "SCA does not provide a flow control mechanism
# for the datagram service." Its whole observable behaviour is a SILENT DISCARD,
# which is why every discard is counted and reported.
#
# LINKED into scsd_exe for the exit-summary report ONLY. No production caller
# routes a datagram through scs_dgram_cdl_deliver(), so the reported figures are
# zeros today -- see the reachability note in scs_dgram.h.
add_library(vmsscs_dgram STATIC scs_dgram.c)
target_include_directories(vmsscs_dgram PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vmsscs_dgram PUBLIC vmsscs_cdt)

# vms-7fe - THE QUEUE OF SCS DIRECTORY ENTRIES (SDIRs) AND THE LISTENING CDTs
# (VAXcluster Principles pp. 2-48..2-50, Figure 2-24). This is what the SCA
# "list of listening SYSAPs" actually is: LISTEN allocates an SDIR carrying the
# SYSAP name plus the CONID of a listening CDT, and an inbound CONNECT_REQ is
# resolved by scanning it -- no match is p. 2-48's "no such SYSAP", a second
# concurrent request is p. 2-50's "busy ... try again later". THAT SECOND
# OUTCOME IS MODULE-ONLY: scsd.c answers synchronously and so cannot present a
# second concurrent request (scs_sdir.h DESIGN CHOICE 3). tests/vmsscs/
# test_scsd_wire.c measures that -- it sums the daemon's busy count across every
# case and asserts 0 -- and tests/vmsscs/test_scs_sdir.c is the only thing that
# reaches the BUSY path at all.
#
# PURE STATE: it builds no frame and opens no socket. It parses exactly one
# grounded field (the 16-byte target SYSAP name at payload [62:78], spec sec
# 4(h)(2)) and returns an enum; the port driver decides what frame that becomes,
# which is the same p. 2-56 split scs_svc.c keeps. Depends on vmsscs_cdt for the
# CDL the listening CDTs are claimed from.
add_library(vmsscs_sdir STATIC scs_sdir.c)
target_include_directories(vmsscs_sdir PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vmsscs_sdir PUBLIC vmsscs_cdt)

# vms-561 - THE FIVE SCS SYSAP SERVICES: LISTEN, CONNECT, ACCEPT, REJECT,
# DISCONNECT (VAXcluster Principles pp. 2-22..2-27, p. 2-56). An API SURFACE
# over the layers this epic already built -- it allocates no memory, builds no
# frame, parses no frame and opens no socket. The port driver half (frame
# assembly and transmission) stays in scsd.c behind the emitter callback, which
# is what keeps the p. 2-31 send choke point and its census intact.
#
# Links credit + dgram (the optional p. 2-42/2-43 CONNECT/ACCEPT arguments, and
# the p. 2-45 Credit Wait flush on release) and conn (the state machine); cdt
# and config arrive transitively.
add_library(vmsscs_svc STATIC scs_svc.c)
target_include_directories(vmsscs_svc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vmsscs_svc PUBLIC vmsscs_conn vmsscs_credit vmsscs_dgram vmsscs_sdir)

# vms-66f - the SCS Process Poller (SCS$DIR_LOOKUP, p. 2-50). The SEND side of
# the directory exchange: it CONNECTs to a remote SCS$DIRECTORY, asks whether
# named SYSAPs are listening, notifies on a Yes only, and DISCONNECTs. Links
# vmsscs_svc for the five services it drives and vmsscs_dir for the request
# frame shapes and the answer classification.
#
# vms-096 adds vmsscs_depart, for scs_depart_enabled() ALONE:
# scs_poll_pb_departing() must decline exactly when scs_pb_depart() would
# decline, and respelling that kill switch here is how the two would drift. The
# poller sits ABOVE the departure teardown in this graph -- which is why
# scs_depart.c cannot make the call itself and scsd.c makes it instead.
add_library(vmsscs_poll STATIC scs_poll.c)
target_include_directories(vmsscs_poll PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vmsscs_poll PUBLIC vmsscs_svc vmsscs_dir vmsscs_depart)

# vms-760 - MSCP-over-SCS disk-client command builders (SET CONTROLLER
# CHARACTERISTICS 0x04 + GET UNIT STATUS 0x03, byte-exact vs the golden af2
# joiner commands). What the joiner sends as a disk CLIENT after NEW, on its own
# MSCP$DISK connection, so an established VAX1 promotes it to MEMBER. MSCP is a
# publicly documented DEC protocol (Rule 8: public opcodes + lab-wire capture);
# plain POSIX + stdint template builders, no other vmsscs dependency.
add_library(vmsscs_mscp STATIC scs_mscp.c)
target_include_directories(vmsscs_mscp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(scsd_exe scsd.c)
target_include_directories(scsd_exe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/libvms/include
)
# vms-dd5 links vmsscs_conn (and, transitively, vmsscs_cdt -- which until this
# item was built but NOT linked into the daemon, exactly as scs_cdt.h said).
# vms-578 adds vmsscs_mscp back: the worktree-760 CM layer merged into scsd.c
# drives the MSCP$DISK client command builders after NEW.
target_link_libraries(scsd_exe PRIVATE vmsscs_classify vmsscs_hello vmsscs_connect vmsscs_dir vmsscs_start vmsscs_vc vmsscs_member vmsscs_mscp vmsscs_config vmsscs_conn vmsscs_depart vmsscs_dgram vmsscs_sdir vmsscs_svc vmsscs_reason vmsscs_disc vmsscs_poll)
set_target_properties(scsd_exe PROPERTIES
    OUTPUT_NAME "SCSD"
    SUFFIX ".EXE"
)
install(TARGETS scsd_exe RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
