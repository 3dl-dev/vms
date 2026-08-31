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
# readers (sysgen_params.h, cluster_authorize.h) for node/group identity.
# sysgen_params.h's readers are still static inline, but (vms-d34) now call
# through to vmsfs (SYS$SYSTEM:OVMXVMSSYS.PAR version resolution), so scsd_exe
# DOES carry a real link dependency on vmsfs/vmslnm now -- see scsd_exe below.

# vms-838 - the raw-L2 datalink abstraction (scs_datalink.h/.c): ONE
# transport backend, Linux AF_PACKET or NetBSD bpf(4) selected at compile
# time via __linux__/__NetBSD__ inside scs_datalink.c itself (no per-image
# CMake branching needed -- the toolchain file already picks the platform).
# No dependency on any other vmsscs library; scsd_exe links it for its
# socket/bpf open + MAC-get + send + recv primitives (see scsd.c's
# send_frame_raw()/get_iface_hwaddr()). Written generic (no SCS type baked
# in) so rd vms-30e (DECnet Phase IV), a second raw-L2 consumer, can link it
# too instead of re-deriving a bpf backend.
add_library(vmsscs_datalink STATIC scs_datalink.c)
target_include_directories(vmsscs_datalink PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# vms-7eb piece 2: the booted OVMX runtime routes the raw L2 SCS wire through
# the EXECUTIVE (/dev/vms VMS_IOCTL_L2_*), so a non-root SCSD needs no
# CAP_NET_RAW -- the kernel owns the socket. OVMX_SCS_DATALINK_EXECUTIVE=ON
# compiles ONLY that backend into scs_datalink.c; the direct-AF_PACKET probe
# code is #ifdef'd out entirely, so a booted SCSD.EXE is PHYSICALLY INCAPABLE of
# opening a raw socket (the anti-fabrication separation the milestone gate
# rests on). Default OFF keeps the AF_PACKET PROBE backend -- the clean-room
# RE/oracle instrument the lab kubectl-exec's -- for dev/test/lab builds. The
# bootable image (distro/Dockerfile.bootable) sets it ON.
option(OVMX_SCS_DATALINK_EXECUTIVE
    "Route SCSD's raw L2 datalink through the executive (/dev/vms) instead of a userspace AF_PACKET socket"
    OFF)
if(OVMX_SCS_DATALINK_EXECUTIVE)
    target_compile_definitions(vmsscs_datalink PRIVATE SCS_DATALINK_VIA_EXECUTIVE)
    target_include_directories(vmsscs_datalink PRIVATE ${CMAKE_SOURCE_DIR}/src/kernel)
endif()

# vms-ec7 (MSCP epic Phase A) - THE SCS MESSAGE ENVELOPE, as a layer. One build
# path, one parse path, one set of offsets, one MTYPE namespace, shared by every
# SCS message OVMX emits or receives. Pure: no socket, no allocation, no
# environment, no logging, and no dependency on any other vmsscs library. Every
# builder below links it, and scs_rx.c is now a receive-side view of it rather
# than a second copy of the layout.
add_library(vmsscs_env STATIC scs_env.c)
target_include_directories(vmsscs_env PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

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
# vms-ec7: shares the SCS message envelope (scs_env.h) instead of hand-writing
# the inner length / format word / MTYPE / credit / Con.ID-pair stores.
target_link_libraries(vmsscs_connect PUBLIC vmsscs_env)

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
# vms-ec7: shares the SCS message envelope (scs_env.h) instead of hand-writing
# the inner length / format word / MTYPE / credit / Con.ID-pair stores.
target_link_libraries(vmsscs_dir PUBLIC vmsscs_connect vmsscs_env)

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
# vms-ec7: shares the SCS message envelope (scs_env.h) instead of hand-writing
# the inner length / format word / MTYPE / credit / Con.ID-pair stores.
target_link_libraries(vmsscs_member PUBLIC vmsscs_env)

# vms-7a9 - connection-manager CEVOTES/QUORUM computation + quorum gate. Pure
# state + arithmetic (no frame, no socket): the documented state-transition
# quorum recomputation (VMScluster Systems sec 2.3.5; CM transcript ch7-part01
# pp. 7-5..7-7) plus the quorum-loss block-and-wait gate (vms-2d6). Consumes the
# VOTES field scs_member parses off the 190-byte VC body[22:24] (spec sec 4j).
add_library(vmsscs_quorum STATIC scs_quorum.c)
target_include_directories(vmsscs_quorum PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# vms-c7d - the Connection Manager CSB connectivity state machine and the
# RECNXINTERVAL virtual-circuit-breakage reconnect loop (transcript ch7-part02
# pp. 7-23/7-24 and p. 7-30). Pure state + clock: builds no frame, opens no
# socket; every time value is a caller-supplied monotonic-ms stamp so the loop
# is unit-testable by injecting a breakage and driving the clock.
add_library(vmsscs_recnx STATIC scs_recnx.c)
target_include_directories(vmsscs_recnx PUBLIC
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
# vms-aa1 GAVE IT A LIVE DATA-PATH ENTRY POINT, and this entry is rewritten
# rather than appended to because the previous one -- "OVMX still emits no live
# credit" -- is now FALSE, and a stale reachability claim in a build file is
# exactly the class of comment this epic keeps rejecting.
#
# What is live now, in scsd.c and nowhere else:
#   - scsd_credit_stamp_outbound(), called from send_frame_vc(): every outbound
#     MTYPE-10 (p. 4-13 application message) frame debits a Send Credit and
#     carries the connection's Pending Receive Credit in the GROUNDED credit
#     field at SCA [48:50] (scs_credit.h WIRE VERDICT). OVMX DOES emit live
#     credit on the wire as of this item; OVMX_NO_CREDIT_ACCOUNTING=1 restores
#     the builders' bytes exactly.
#   - scsd_handle_frame(): an inbound MTYPE-10 credit field is added to that
#     connection's Send Credit (scs_credit_on_recv), an inbound ACCEPT_REQ's
#     credit field is the p. 2-43 initial extension (scs_credit_grant_from_peer),
#     and a delivered message's buffer is released (scs_credit_release_buffer),
#     which is what makes the NEXT piggyback non-zero.
#
# STILL NOT LIVE, stated so the gap is not read as coverage: no send enters a
# Credit Wait (a starved send is transmitted unstamped and COUNTED as
# `starved=` in the exit summary, because the frame's sequence number is already
# consumed by the time the choke point sees it), and OVMX sends no special
# credit message -- nothing installs the p. 2-44 emitter hook. Every
# scs_credit_wait_flush() on the teardown side therefore still drops zero
# waiters. Do not read a green test run as "OVMX implements p. 2-45".
#
# It arrives in scsd_exe three transitive ways (vmsscs_depart, vmsscs_svc,
# vmsscs_poll) and is now ALSO named directly on the link line, because scsd.c
# calls it directly and a direct caller depending on a transitive edge is one
# refactor away from a link error.
#
# THE PATTERN, KEPT FROM THE ENTRY THIS REPLACED (vms-096): the vmsscs_reason
# note below made the same "no production caller" claim by the same argument,
# and vms-591 falsified it. Two modules have now had that claim go stale in this
# file. When a third does, correct this entry and that one together.
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
# vms-ec7: shares the SCS message envelope (scs_env.h) instead of hand-writing
# the inner length / format word / MTYPE / credit / Con.ID-pair stores.
target_link_libraries(vmsscs_disc PUBLIC vmsscs_reason vmsscs_env)

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
# plain POSIX + stdint template builders.
add_library(vmsscs_mscp STATIC scs_mscp.c)
target_include_directories(vmsscs_mscp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# vms-ec7: shares the SCS message envelope (scs_env.h) instead of hand-writing
# the inner length / format word / MTYPE / credit / Con.ID-pair stores.
# vms-020: also links vmsscs_member for scs_member_vms_time_now() -- P.TIME
# (sec 6.16) is live host time, not a frozen capture replay.
# vms-d76: also links vmsscs_credit for scs_credit_peek_pending() -- the
# non-mutating read of the connection's Pending Receive Credit (the piggyback
# account) that the CREDIT build site uses, correcting vms-8de's read of the
# wrong CDT field (send_credit).
target_link_libraries(vmsscs_mscp PUBLIC vmsscs_env vmsscs_member vmsscs_credit)

# vms-291 (MSCP epic Phase D) - THE OTHER HALF: the MSCP disk SERVER responder.
# vmsscs_mscp builds the commands OVMX sends as a disk client; this answers the
# commands a real VAX's disk class driver sends to OVMX, so a real VAX can MOUNT
# a disk OVMX serves. End-message layouts are AA-L619A-TK Table A-7 (public doc,
# Rule 8) and the SCC/GUS builders are proven byte-exact against real captured
# VAX server end messages (86-content 0x84 and 110-content 0x83 classes). Plain
# POSIX + stdint; pread() on a raw block image is the whole backing store.
add_library(vmsscs_mscp_srv STATIC scs_mscp_srv.c)
target_include_directories(vmsscs_mscp_srv PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vmsscs_mscp_srv PUBLIC vmsscs_mscp vmsscs_env)

# vms-94c (DLM epic vms-7fa rung 1) - THE DLM SYSAP MESSAGE CLASS: encode/decode
# for the four core distributed-lock-manager messages (ENQ / GRANT / DEQ /
# BLKAST). Rides the shared SCS envelope (vmsscs_env) as a p. 4-13 application
# message (MTYPE 10), the same nesting scs_mscp.c uses. The SEMANTIC field values
# are authentic $LCKDEF (lckdef.h in src/libvms/include, oracle-pinned); the DLM
# body BYTE LAYOUT is a LABELLED OVMX design choice (VSI/HPE do not publish it --
# Rule 8), recorded in docs/compat/facilities/cluster-dlm.yaml. Pure: no socket,
# no allocation. scsd_exe links it for the DLM SYSAP receive/transmit path.
add_library(vmsscs_dlm STATIC scs_dlm.c)
target_include_directories(vmsscs_dlm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/libvms/include
)
target_link_libraries(vmsscs_dlm PUBLIC vmsscs_env)

# vms-7c0 - THE RECEIVE-SIDE SCS HEADER. One pure function: decode the sec
# 4(h)(1b) envelope, classify the p. 4-13 MTYPE, hand out the destination and
# source Con.IDs so scsd.c can do the p. 2-29 CDL lookup instead of comparing a
# Con.ID against a compile-time macro. Builds no frame, opens no socket,
# allocates nothing, and deliberately indexes NO table -- the CDL bound check
# and slot-ownership check stay in scs_cdl_lookup(), which is the only place
# allowed to turn a peer-supplied Con.ID into a pointer. No dependency on any
# other vmsscs library (plain stdint).
add_library(vmsscs_rx STATIC scs_rx.c)
target_include_directories(vmsscs_rx PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vmsscs_rx PUBLIC vmsscs_env)

add_executable(scsd_exe scsd.c)
target_include_directories(scsd_exe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/libvms/include
    ${CMAKE_SOURCE_DIR}/src/kernel   # vms-94c: vms_ioctl.h (VMS_IOCTL_DLM_XNODE) for the /dev/vms DLM dispatch
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
)
# vms-dd5 links vmsscs_conn (and, transitively, vmsscs_cdt -- which until this
# item was built but NOT linked into the daemon, exactly as scs_cdt.h said).
# vms-578 adds vmsscs_mscp back: the worktree-760 CM layer merged into scsd.c
# drives the MSCP$DISK client command builders after NEW.
# vms-34b adds vmsscs_mscp_srv: scsd_mscp_srv_msg_input() calls the disk-server
# responder (scs_mscp_srv_handle() etc.) built by vms-291/vms-4e31, which until
# this item was compiled and unit-tested but never linked into the daemon that
# is supposed to run it.
# vms-d34: sysgen_params.h's readers now resolve SYS$SYSTEM:OVMXVMSSYS.PAR
# through vmsfs (highest-version lookup) instead of a bare Linux path, so
# scsd_exe needs the vmsfs/vmslnm link it previously didn't (see the
# header-only claim at the top of this file -- no longer true).
# vms-838: the raw-L2 transport backend (see vmsscs_datalink above).
target_link_libraries(scsd_exe PRIVATE vmsscs_classify vmsscs_hello vmsscs_connect vmsscs_dir vmsscs_start vmsscs_vc vmsscs_member vmsscs_quorum vmsscs_recnx vmsscs_mscp vmsscs_mscp_srv vmsscs_dlm vmsscs_config vmsscs_conn vmsscs_depart vmsscs_dgram vmsscs_sdir vmsscs_svc vmsscs_reason vmsscs_disc vmsscs_poll vmsscs_rx vmsscs_credit vmsscs_datalink vmsrms vmsfs vmslnm)
# vms-5f0/vms-c3b: SCSD reads SYS$SYSTEM:OVMXVMSSYS.PAR (SCSNODE/SCSSYSTEMID/
# ALLOCLASS/RECNXINTERVAL) through sysgen_read_* -> the executive Files-11 ACP
# (RMS-over-ACP), not the retired /vms passthrough. Link vmsrms; the strong-ref
# macro keeps LIBVMSRMS$SHR from --as-needed drop and pulls sysgen_acp.o from
# the static archive (build-static).
target_compile_definitions(scsd_exe PRIVATE OVMX_SYSGEN_ACP_STRONG)
set_target_properties(scsd_exe PROPERTIES
    OUTPUT_NAME "SCSD"
    SUFFIX ".EXE"
)
install(TARGETS scsd_exe RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
