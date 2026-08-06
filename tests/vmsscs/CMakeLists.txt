# tests/vmsscs - Unit tests for vmsscs (SCS datalink length classifier)

add_executable(test_scs_classify test_scs_classify.c)

target_include_directories(test_scs_classify PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_classify PRIVATE vmsscs_classify)

add_test(NAME vmsscs_classify_unit COMMAND test_scs_classify)
set_tests_properties(vmsscs_classify_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-b62 - HELLO builder byte-layout test.
add_executable(test_scs_hello test_scs_hello.c)

target_include_directories(test_scs_hello PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_hello PRIVATE vmsscs_hello)

add_test(NAME vmsscs_hello_unit COMMAND test_scs_hello)
set_tests_properties(vmsscs_hello_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-9f3 - padded directed-HELLO builder (NISCA channel packet-size
# verification, spec sec 4k): proves the padded frame is a genuine directed
# HELLO with only the SCA length field changed + a pure-zero pad tail, and that
# smaller probes are byte-identical prefixes of the 1500-byte frame.
add_executable(test_scs_hello_padded test_scs_hello_padded.c)

target_include_directories(test_scs_hello_padded PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_hello_padded PRIVATE vmsscs_hello)

add_test(NAME vmsscs_hello_padded_unit COMMAND test_scs_hello_padded)
set_tests_properties(vmsscs_hello_padded_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-d94 - directed-HELLO abs-30 channel-verify per-frame word (spec sec 4a
# offset-30): proves the builder can reach b4 (not just the fixed b3 vms-5fe
# held), the response rule b2->b3/b3->b4/b4->b3, and that changing the word
# changes ONLY abs30 (keep-everything-else invariant).
add_executable(test_scs_hello_pfw test_scs_hello_pfw.c)

target_include_directories(test_scs_hello_pfw PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_hello_pfw PRIVATE vmsscs_hello)

add_test(NAME vmsscs_hello_pfw_unit COMMAND test_scs_hello_pfw)
set_tests_properties(vmsscs_hello_pfw_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-5fe - directed HELLO + VMS$VAXcluster SCS connect handshake test.
add_executable(test_scs_connect test_scs_connect.c)

target_include_directories(test_scs_connect PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_connect PRIVATE vmsscs_hello vmsscs_connect vmsscs_conn)

add_test(NAME vmsscs_connect_unit COMMAND test_scs_connect)
set_tests_properties(vmsscs_connect_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-21e - phase-2 START/config (0x41) builder + SCS seq/ack state machine.
add_executable(test_scs_start test_scs_start.c)

target_include_directories(test_scs_start PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_start PRIVATE vmsscs_start)

add_test(NAME vmsscs_start_unit COMMAND test_scs_start)
set_tests_properties(vmsscs_start_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-691 - SCS VC engine: 0x48 credit-return builder + seq/ack + retransmit.
add_executable(test_scs_vc test_scs_vc.c)

target_include_directories(test_scs_vc PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_vc PRIVATE vmsscs_vc)

add_test(NAME vmsscs_vc_unit COMMAND test_scs_vc)
set_tests_properties(vmsscs_vc_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-246 - SCS$DIRECTORY connect + SCS$DIR_LOOKUP responder: CONNECT-RESPONSE
# Con.ID binding + lookup-response builder ("NOT PRESENT HERE" / affirmative).
add_executable(test_scs_dir test_scs_dir.c)

target_include_directories(test_scs_dir PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_dir PRIVATE vmsscs_dir vmsscs_conn)

add_test(NAME vmsscs_dir_unit COMMAND test_scs_dir)
set_tests_properties(vmsscs_dir_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-224 - VMS$VAXcluster connection-manager add-member transaction: op
# 0x14/0x01/0x02 config builders (byte-exact vs golden joiner frames), VOTES=0
# non-voting, the 0x81 response echoing the member's REAL captured (txn,checksum)
# token (spec sec 4j). THE MILESTONE builders.
add_executable(test_scs_member test_scs_member.c)

target_include_directories(test_scs_member PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_member PRIVATE vmsscs_member)

add_test(NAME vmsscs_member_unit COMMAND test_scs_member)
set_tests_properties(vmsscs_member_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-7be - SB/PB/PDT configuration queue: the architected replacement for the
# MAC-keyed peer struct. Asserts the documented queue transitions of VAXcluster
# Principles ch. 2 (pp. 2-11..2-21), including the p. 2-21 Note (an old SB with
# no other PB queued to it is REFRESHED from the formative SB on a rejoin).
add_executable(test_scs_config test_scs_config.c)

target_include_directories(test_scs_config PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

# vms-17f also puts the ORDERED TEARDOWN cases here (scs_pb_close's refusal, the
# p. 2-28 notify-before-destroy sequence, the p. 2-43 MFREEQ reconciliation and
# the OVMX_NO_PEER_DEPART kill switch), so this target links vmsscs_depart --
# which pulls in vmsscs_credit and vmsscs_cdt, the other two layers the sequence
# spans.
target_link_libraries(test_scs_config PRIVATE vmsscs_config vmsscs_depart)

add_test(NAME vmsscs_config_unit COMMAND test_scs_config)
set_tests_properties(vmsscs_config_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-e1a - CDT / CDL / Path-Block connection queue. Asserts the documented
# connection model of VAXcluster Principles ch. 2: CONID -> CDL -> CDT lookup
# (p. 2-29), two CDTs on one Path Block both reachable from the PB and both
# notified on VC loss (p. 2-28), SCSCONNCNT+200 two-tier CDL sizing with
# released-not-deallocated reuse (p. 2-30), and a packet routed to the right
# SYSAP input routine by DESTINATION CONID with the SOURCE CONID checked
# (pp. 2-29, 2-35).
add_executable(test_scs_cdt test_scs_cdt.c)

target_include_directories(test_scs_cdt PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_cdt PRIVATE vmsscs_cdt)

add_test(NAME vmsscs_cdt_unit COMMAND test_scs_cdt)
set_tests_properties(vmsscs_cdt_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-dd5 - THE SCA CONNECTION STATE MACHINE. Transcribes Figures 2-14/2-15/2-16
# a SECOND time, independently of src/vmsscs/scs_conn.c's table, and asserts the
# production table agrees row for row; then walks the formation, rejection and
# disconnect paths live from BOTH ends asserting the exact state after every
# message in each direction. Also RUNS the OVMX_NO_CONN_FSM kill switch
# (guardrail 23) and pins the stuck-connection diagnostic.
add_executable(test_scs_conn test_scs_conn.c)

target_include_directories(test_scs_conn PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_conn PRIVATE vmsscs_conn)

add_test(NAME vmsscs_conn_unit COMMAND test_scs_conn)
set_tests_properties(vmsscs_conn_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-7be - THE SCSD-LEVEL TEST. The other targets in this file link the builder
# libraries; this one compiles src/vmsscs/scsd.c itself (SCSD_UNIT_TEST swaps the
# AF_PACKET sendto for a capture buffer and renames the daemon's main) so the
# peer-state accessors the SB/PB refactor rewrote are asserted through the REAL
# frame-assembly path: the learned SCS System Address IS the SCA peer-logical
# field and nothing else, the port address is only the destination, a shared
# System Block aliases two ports' peer-logical, and SB-pool exhaustion degrades
# to a zero peer-logical WITH an SCSD-E-NOSB log rather than silently.
add_executable(test_scsd_wire test_scsd_wire.c)

target_include_directories(test_scsd_wire PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
    ${CMAKE_SOURCE_DIR}/src/libvms/include
)

target_link_libraries(test_scsd_wire PRIVATE
    vmsscs_classify vmsscs_hello vmsscs_connect vmsscs_dir
    vmsscs_start vmsscs_vc vmsscs_member vmsscs_config vmsscs_conn
    vmsscs_depart vmsscs_dgram vmsscs_svc vmsscs_reason vmsscs_disc
    vmsscs_poll vmsscs_rx
    # vms-aa1: scsd.c now calls scs_credit_* directly on both the send and the
    # receive path, so the credit account is a DIRECT dependency of this test,
    # not one that happens to arrive through vmsscs_depart.
    vmsscs_credit
    # vms-578: the merged scsd.c drives the MSCP$DISK client builders, so the
    # unit-test seam needs them too (it #includes scsd.c whole).
    vmsscs_mscp
)

add_test(NAME vmsscs_scsd_wire_unit COMMAND test_scsd_wire)
set_tests_properties(vmsscs_scsd_wire_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-76e - SCA message flow control. Reproduces the VAXcluster Principles
# pp. 2-43..2-44 worked example and its simultaneous-send variation EXACTLY,
# the p. 2-45 Credit Wait refusal, the p. 2-44 special credit message, the
# credit field read out of two REAL captured frames (110-byte CONNECT-REQUEST
# carrying 10 = CLUSTER_CREDITS; 190-byte VC message carrying a piggybacked 3),
# and the OVMX_NO_CREDIT_ACCOUNTING kill switch with a bracketing control.
add_executable(test_scs_credit test_scs_credit.c)

target_include_directories(test_scs_credit PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_credit PRIVATE vmsscs_credit)

add_test(NAME vmsscs_credit_unit COMMAND test_scs_credit)
set_tests_properties(vmsscs_credit_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-76e - the WIRE VERDICT prose must match the measurement.
#
# tools/scs_credit_measure.py re-derives every figure in the WIRE VERDICT of
# src/vmsscs/include/scs_credit.h from the lab-1 captures and PASS/FAILs each
# against a checked-in EXPECTED table. Those captures are host-only (13 GB, 47
# files, not in git), so ctest cannot run that half -- run it by hand on a lab
# host. What ctest DOES run needs no captures: it reads EXPECTED out of the same
# script and asserts every figure still appears in the header and in
# docs/cluster-protocol-spec.md, plus the two structural facts the vms-76e
# review turned on (conservation is scoped to the UNFILTERED population; the
# inverted ">>> FILTER, REQUIRED <<<" heading stays deleted).
#
# WHY IT EXISTS: nothing in ctest re-derived any WIRE VERDICT figure, so a wrong
# recorded measurement was invisible to a green run. That is how a mis-scoped
# filter claim survived two review rounds.
#
# The gate is registered UNCONDITIONALLY, under the same name and labels either
# way. A missing interpreter must NOT make it vanish (a skipped test is a
# failing test), and must NOT break configure either: tests/qemu/Dockerfile
# configures this whole tree with -DOVMX_STATIC=ON -DBUILD_TESTS=ON on a musl
# builder image with no python3, builds only `qemu_syssvc_tests vmsdcl`, and
# never invokes ctest -- an earlier configure-time FATAL_ERROR here turned five
# CI jobs red that run no vmsscs test at all. So the absence is made loud at
# TEST time instead: scs_credit_figures_no_python3.cmake exits non-zero and
# names the missing interpreter. Configure succeeds, a build that never runs
# ctest is unaffected, and any build that does run ctest sees a RED test.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME scs_credit_figures
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_scs_credit_figures.py)
else()
    add_test(NAME scs_credit_figures
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/scs_credit_figures_no_python3.cmake)
endif()
set_tests_properties(scs_credit_figures PROPERTIES
    LABELS "unit;scs;cluster;docs"
    TIMEOUT 60
)

# vms-aa1 - THE MUTATION BATTERY FOR THE LIVE FLOW-CONTROL ACCOUNT.
#
# WHY IT EXISTS, in one fact: vms-76e and vms-1d2 shipped a fully unit-tested
# pp. 2-43..2-45 credit account that NOTHING CALLED, and every one of those
# tests stayed green throughout -- because they called the account themselves.
# vms-aa1 moved the assertions onto the production path (the four test_credit_*
# cases in test_scsd_wire.c); this battery is what makes "those assertions would
# notice" a measurement. It breaks one live behaviour in scsd.c at a time,
# rebuilds, and requires the C test to go NON-ZERO. It found two of its own
# survivors on first run (an assertion comparing a banked 0 against a wire 0,
# and a p. 2-35 source check no fed frame violated) -- both are now covered by
# captured frames rather than by argument.
#
# The mutant count and the kill count are PRINTED by the script and deliberately
# not restated here (the vms-6b3 precedent: a second copy of a figure is how a
# coverage claim drifts). It needs a C compiler, which is why CMAKE_C_COMPILER
# is passed through: the gate must run under whatever toolchain configured the
# tree, musl-gcc included. Registered unconditionally.
if(Python3_Interpreter_FOUND)
    add_test(NAME scs_credit_live_mutants
             COMMAND ${CMAKE_COMMAND} -E env OVMX_CC=${CMAKE_C_COMPILER}
                     ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_scs_credit_live_mutants.py)
else()
    add_test(NAME scs_credit_live_mutants
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/scs_credit_live_mutants_no_python3.cmake)
endif()
set_tests_properties(scs_credit_live_mutants PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 600
)

# vms-abc - THE SEND SITE CENSUS. test_scsd_send_sites.py reads src/vmsscs/scsd.c
# and asserts that every direct caller of the transport (send_frame_raw) is one
# of the functions the SEND SITE TABLE names as EXEMPT, i.e. that every other
# sender goes through the send_frame_vc() choke point where p. 2-31 is enforced.
#
# WHY A SOURCE GATE AND NOT ONLY A RUNTIME ONE: the runtime test
# (test_a_broken_circuit_carries_no_traffic) can only prove the send paths its
# captured frames happen to drive. Three review rounds on this item each found
# one more sender that no runtime test reached. This one cannot be outflanked by
# adding a path -- adding one reds it.
#
# Registered unconditionally, exactly like scs_credit_figures above: a missing
# interpreter must make the gate RED, never make it vanish.
if(Python3_Interpreter_FOUND)
    add_test(NAME scs_send_sites
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_scsd_send_sites.py)
else()
    add_test(NAME scs_send_sites
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/scs_send_sites_no_python3.cmake)
endif()
set_tests_properties(scs_send_sites PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 60
)

# vms-b1d - SCA datagram buffer management: the per-port DFREEQ and the
# per-connection buffer count in the CDT (VAXcluster Principles pp. 2-42..2-43).
# Covers both documented discard classes, the fact that a no-quota discard
# RETURNS the buffer to the DFREEQ, the per-connection and per-port counters,
# the run-log report that makes a wire-silent discard locally visible (INV-6),
# and the OVMX_NO_DGRAM_ACCOUNTING kill switch.
add_executable(test_scs_dgram test_scs_dgram.c)

target_include_directories(test_scs_dgram PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

# vmsscs_depart as well as vmsscs_dgram: the connection-lifecycle fixtures drive
# the DFREEQ deposit return through scs_pb_depart(), the PRODUCTION caller of
# scs_cdl_release() since vms-17f, not only through scs_cdl_release() directly.
target_link_libraries(test_scs_dgram PRIVATE vmsscs_dgram vmsscs_depart)

add_test(NAME vmsscs_dgram_unit COMMAND test_scs_dgram)
set_tests_properties(vmsscs_dgram_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-561 - THE FIVE SCS SYSAP SERVICES: LISTEN, CONNECT, ACCEPT, REJECT,
# DISCONNECT (VAXcluster Principles pp. 2-22..2-27, p. 2-56). Asserts the
# p. 2-56 allocation rule as a number on every service (CONNECT and ACCEPT
# allocate, DISCONNECT releases, REJECT never touches the CDL), the p. 2-47
# circuit-selection refusal, the descriptorless OVMX_NO_CONN_FSM mode with a
# matched control, and -- the integration case -- two independent SCS instances
# forming, USING and tearing down a connection through the services, ending with
# both CDLs empty and both ports' p. 2-42/2-43 buffer deposits returned.
add_executable(test_scs_svc test_scs_svc.c)

target_include_directories(test_scs_svc PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_svc PRIVATE vmsscs_svc vmsscs_dir)

add_test(NAME vmsscs_svc_unit COMMAND test_scs_svc)
set_tests_properties(vmsscs_svc_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-7fe - the p. 2-48 SDIR queue and the listening CDTs (Figure 2-24): LISTEN
# allocates both, an inbound CONNECT_REQ is resolved by scanning the queue, and
# the p. 2-49 rule that ACCEPT's local CONID is the NEW CDT's. The p. 2-50 BUSY
# reply is exercised here and nowhere else -- see the SCOPE HONESTY note at the
# top of the file for why the daemon cannot reach it.
# vms-66f - the SCS Process Poller (SCS$DIR_LOOKUP, p. 2-50). Drives the real
# scs_connect()/scs_disconnect() services and the real connection state machine
# through a stub port driver, and builds the real request frames on the way, so
# the cadence/notify/disable rules are tested against production code paths
# rather than asserted beside them. Carries the OVMX_NO_PROCESS_POLLER
# kill-switch measurement and its matched control.
add_executable(test_scs_poll test_scs_poll.c)
target_include_directories(test_scs_poll PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)
target_link_libraries(test_scs_poll PRIVATE vmsscs_poll)
add_test(NAME vmsscs_poll_unit COMMAND test_scs_poll)
set_tests_properties(vmsscs_poll_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

add_executable(test_scs_sdir test_scs_sdir.c)
target_include_directories(test_scs_sdir PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)
target_link_libraries(test_scs_sdir PRIVATE vmsscs_sdir)
add_test(NAME vmsscs_sdir_unit COMMAND test_scs_sdir)
set_tests_properties(vmsscs_sdir_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-fdd - the SCA CONNECT DATA verdict prose must match the measurement.
#
# tools/scs_connect_data_measure.py re-derives every figure in the CONNECT DATA
# verdict of src/vmsscs/include/scs_connect.h and spec section 4(n) from the
# lab-1 captures and PASS/FAILs each against a checked-in EXPECTED table. Those
# captures are host-only (not in git), so ctest cannot run that half -- run it
# by hand on a lab host. What ctest DOES run needs no captures: it reads
# EXPECTED out of the same script and asserts every figure still appears in the
# header and the spec, and that the C array scs_connect_data_vaxcluster[], the
# header verdict and the spec agree on the 16 bytes byte for byte.
#
# WHY IT EXISTS: those 16 bytes are a VMS VERSION CLAIM that the peer connection
# manager is documented to reject on (VAXcluster Principles p. 2-25). A wrong
# recorded measurement behind them would be invisible to a green run.
#
# Registered UNCONDITIONALLY under the same name and labels either way -- see
# the vms-76e note above for why the missing-interpreter case reds at TEST time
# instead of failing configure.
if(Python3_Interpreter_FOUND)
    add_test(NAME scs_connect_data_figures
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_scs_connect_data_figures.py)
else()
    add_test(NAME scs_connect_data_figures
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/scs_connect_data_figures_no_python3.cmake)
endif()
set_tests_properties(scs_connect_data_figures PROPERTIES
    LABELS "unit;scs;cluster;docs"
    TIMEOUT 60
)

# vms-beb - the capture corpus is a MANIFEST, not a glob.
#
# tools/cluster/capture_manifest.py is the declared allowlist of every lab
# capture a measurement tool may read, and which lab produced it -- the
# replacement for the `-lab2-` filename-marker blocklist that let six lab-2
# vaxlab-4 captures silently mix into the lab-1 grounding library (vms-096).
# test_capture_manifest.py needs NO lab captures: every scenario uses empty
# placeholder files in a scratch tmp dir, since the manifest functions only
# ever look at filenames. It proves the item's own acceptance test -- a
# lab-1 census pointed at a real lab-2-declared capture reds -- through both
# the list-based and directory-globbing entry points, and asserts all five
# tools named in vms-beb actually call into the module (a manifest nobody
# calls is not a fence).
#
# Registered UNCONDITIONALLY under the same name either way -- see the
# vms-76e note above for why the missing-interpreter case reds at TEST time
# instead of failing configure.
if(Python3_Interpreter_FOUND)
    add_test(NAME capture_manifest
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_capture_manifest.py)
else()
    add_test(NAME capture_manifest
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/capture_manifest_no_python3.cmake)
endif()
set_tests_properties(capture_manifest PROPERTIES
    LABELS "unit;scs;cluster;docs"
    TIMEOUT 30
)

# vms-69c - a census cannot silently restrict its own population, in either
# direction.
#
# tools/cluster/census_guard.py is the shared refusal every SCA measurement
# tool now calls: check_census() REFUSES to run a census that silently
# narrows to a subset of the envelope-conformant SCA length classes present
# in its own population (the vms-c11 under-sampling failure -- every census
# in the epic filtered on {62,66,110} and was blind to the 58-byte RESPONSE
# class, so the spec wrongly said message types 5/7 do not exist), and
# REFUSES a census that spans classes not sharing the SCA envelope (the
# mirror -- reading [46:48] across the still-undecoded 70-content class).
# Either direction requires an explicit, checked-in justification string and
# reports the excluded/over-generalised population size regardless.
# test_census_guard.py needs NO lab captures for the pure half (check_census()
# never touches a byte) and builds its own synthetic classic-pcap files for
# the end-to-end half, so the envelope-conformance byte offsets are exercised
# against the SAME dissect_sca.read_pcap() every measurement tool uses. It
# also asserts scs_reason_measure.py, scs_disc_measure.py and
# scs_connect_data_measure.py all actually call check_census() -- a guard
# nobody calls is not a fence.
#
# Registered UNCONDITIONALLY under the same name either way -- see the
# vms-76e note above for why the missing-interpreter case reds at TEST time
# instead of failing configure.
if(Python3_Interpreter_FOUND)
    add_test(NAME census_guard
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_census_guard.py)
else()
    add_test(NAME census_guard
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/census_guard_no_python3.cmake)
endif()
set_tests_properties(census_guard PROPERTIES
    LABELS "unit;scs;cluster;docs"
    TIMEOUT 30
)

# vms-6b3 - the 16-bit REJECT/DISCONNECT reason code (VAXcluster Principles
# p. 2-26). Asserts byte position, LE order and round-trip in BOTH frames; that
# two REAL captured VMS frames (one REJECT_REQ, one DISCONNECT_REQ) read zero at
# the OVMX slot, which is the observation the placement rests on; and the
# OVMX_NO_REASON_CODE kill switch with a bracketing control on both sides.
add_executable(test_scs_reason test_scs_reason.c)

target_include_directories(test_scs_reason PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_reason PRIVATE vmsscs_reason)

add_test(NAME vmsscs_reason_unit COMMAND test_scs_reason)
set_tests_properties(vmsscs_reason_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-6b3, review round 2 - THE REASON-CODE CENSUS GATE. Both defects that round
# found were the same defect: a figure that only a comment carried. The header
# and the spec justified payload [58:60] as "the only slot zero in 100% of
# observed frames" -- refuted by re-running the census with the file's own
# population rule -- and the spec said "nothing after the Con.ID pair varies"
# while tools/cluster/scs_reason_measure.py printed the DISCONNECT_REQ
# payload[60:62] split in the same commit. Nothing in ctest read the
# measurement, so both survived a green run.
#
# test_scs_reason_figures.py PARSES the CENSUS-A/B/C lines out of scs_reason.h
# and the CENSUS-marked tables out of docs/cluster-protocol-spec.md and compares
# them field by field against the EXPECTED table in scs_reason_measure.py, pins
# the two REFUTATION-FACT lines that carry the measurements the dead claims deny,
# and reds if a refuted claim is asserted anywhere outside an explicit
# REFUTED-QUOTE-BEGIN/END block. It needs no captures.
#
# NO MUTATION-COVERAGE NUMBER LIVES IN THIS COMMENT. The round-2 version of it
# claimed "26 of 26 mutants (drifted digits in either document ... and both
# refuted sentences reinstated)" and the clause that mattered was FALSE:
# re-asserting "Nothing after the Con.ID pair varies." in three natural sites
# left the gate GREEN, because its refuted-claim check was a proximity window
# over two documents that are entirely ABOUT the refutation. A comment that
# overstates a gate is worse than no gate. The count is now MEASURED on every
# ctest run by scs_reason_mutants below -- read its output, not a comment.
#
# Registered unconditionally, exactly like scs_credit_figures above: a missing
# interpreter must make the gate RED, never make it vanish.
if(Python3_Interpreter_FOUND)
    add_test(NAME scs_reason_figures
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_scs_reason_figures.py)
else()
    add_test(NAME scs_reason_figures
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/scs_reason_figures_no_python3.cmake)
endif()
set_tests_properties(scs_reason_figures PROPERTIES
    LABELS "unit;scs;cluster;docs"
    TIMEOUT 60
)

# vms-6b3, review round 3 - THE MUTATION BATTERY FOR THE GATE ABOVE.
#
# WHY IT EXISTS: the defect round 3 found was not in the spec or the header, it
# was in the artifact built to protect them. scs_reason_figures shipped with a
# refuted-claim check that could not fail, and a CMake comment that said it
# could. The fix is not a better comment -- it is a test that re-derives the
# kill count from scratch every run.
#
# test_scs_reason_mutants.py copies scs_reason.h, cluster-protocol-spec.md and
# tools/cluster/scs_reason_measure.py into a scratch tree, proves the UNMUTATED
# copy is green (the control), then applies each mutant on its own and requires
# scs_reason_figures to exit non-zero, restoring and re-verifying the control
# after every one. Mutants that do not actually change the file are reported as
# failures, never scored as kills. Nothing under src/, docs/ or tools/ is
# written; the gate is pointed at the scratch copies with the
# OVMX_SCS_REASON_{HEADER,SPEC,MEASURE} environment overrides.
#
# The classes it covers: drifted digits and deleted rows in either document and
# in EXPECTED; the four sites where a verbatim re-assertion of a refuted claim
# survived round 2 (three of them the adversary's); the four sites where one
# survived round 3, each the first prose paragraph after a CENSUS table or a
# REFUTATION-FACT block (the DATABLOCK-* group); five REWORDED forms of the same
# claims; four attacks on the quarantine mechanism itself; six attacks on the
# pinned REFUTATION-FACT lines; and the structural claims the placement
# rationale rests on. The NUMBER of mutants and how many died is PRINTED by the
# test -- deliberately not restated here, because a second copy of a figure is
# how this item got rejected twice.
if(Python3_Interpreter_FOUND)
    add_test(NAME scs_reason_mutants
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_scs_reason_mutants.py)
else()
    add_test(NAME scs_reason_mutants
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/scs_reason_mutants_no_python3.cmake)
endif()
set_tests_properties(scs_reason_mutants PROPERTIES
    LABELS "unit;scs;cluster;docs"
    TIMEOUT 300
)

# vms-591 - THE DISCONNECT DIALOGUE ON THE WIRE (Figure 2-16, pp. 2-26/2-27).
#
# Asserts byte-exact reproduction of TWO REAL CAPTURED FRAMES -- one
# DISCONNECT_REQ and one DISCONNECT_RSP from a single complete teardown in
# formation-ci1.pcap -- from both ends of the dialogue, plus every substituted
# field, the derived length fields, the newly-grounded [60:62] matching flag,
# and the OVMX_NO_CLEAN_SHUTDOWN kill switch RUN with controls on both sides.
#
# The DISCONNECT_RSP half is the one that matters most: the spec said message
# type 7 does not appear on our wire and told the next agent not to build one.
# It does appear; every census had been restricted to SCA length classes
# {62, 66, 110} and the response class is 58. The test carries that correction
# as an assertion over the captured bytes, not as a sentence.
# vms-ec7 (MSCP epic Phase A) - THE SCS MESSAGE ENVELOPE unit test.
#
# scs_env.c is the single build/parse path for the envelope every SCS message
# shares, so a one-byte defect in it is a one-byte defect on every frame OVMX
# transmits. The per-class byte-exact tests already cover the happy path --
# MEASURED: a +1 mutation of the MTYPE store reds seven test binaries. This
# binary covers what they cannot reach: the REFUSALS (the 106-content START, the
# 120-content HELLO and the undecoded 70-content class must all fail the
# conformance test), the boundaries (the 58-content class is envelope and
# nothing else, so its payload must be NULL rather than a pointer past the
# frame), the frame-vs-content addressing, and the p. 4-15 dispatch decision
# over the WHOLE u16 MTYPE space.
add_executable(test_scs_env test_scs_env.c)
target_include_directories(test_scs_env PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)
target_link_libraries(test_scs_env PRIVATE vmsscs_env)
add_test(NAME vmsscs_env_unit COMMAND test_scs_env)
set_tests_properties(vmsscs_env_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-ec7 - THE FIGURES GATE for the envelope. Pins src/vmsscs/include/scs_env.h
# to tools/cluster/scs_env_measure.py's EXPECTED table, and (vms-371, on a host
# with the lab captures) pins EXPECTED to the packets -- including the BUILD
# ROUND TRIP, which rebuilds content[42:58] for every envelope-conformant frame
# in the corpus with scs_env_build()'s own rule and requires byte-identity.
# It also enforces the structural half of the item: scs_rx.h must ALIAS the
# offsets rather than restate them, no builder may write an envelope field by
# open-coded offset any more, and scs_start.c / scs_hello.c must NOT route their
# non-envelope classes through the shared builder.
if(Python3_Interpreter_FOUND)
    add_test(NAME scs_env_figures
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_scs_env_figures.py)
else()
    add_test(NAME scs_env_figures
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/scs_env_figures_no_python3.cmake)
endif()
set_tests_properties(scs_env_figures PROPERTIES
    LABELS "unit;scs;cluster;docs"
    TIMEOUT 300
)

add_executable(test_scs_disc test_scs_disc.c)
target_include_directories(test_scs_disc PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)
target_link_libraries(test_scs_disc PRIVATE vmsscs_disc vmsscs_reason)
add_test(NAME vmsscs_disc_unit COMMAND test_scs_disc)
set_tests_properties(vmsscs_disc_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-591 - the FIGURES gate for the disconnect measurement. Needs no captures:
# it asserts every number tools/cluster/scs_disc_measure.py recorded still
# appears verbatim in scs_disc.h, in scsd.c's shutdown-timeout justification and
# in docs/cluster-protocol-spec.md, so the prose cannot drift away from the
# measurement. Only the measure script, on a host with the captures, re-derives
# the numbers themselves.
#
# It ALSO gates the correction: the spec's old "5 and 7 DO NOT EXIST ON OUR
# WIRE" / "do not build a 5 or 7 frame" claims are REFUTED, and this test reds
# if either sentence comes back anywhere in the spec outside the block that
# quarantines it as refuted history.
if(Python3_Interpreter_FOUND)
    add_test(NAME scs_disc_figures
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_scs_disc_figures.py)
else()
    add_test(NAME scs_disc_figures
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/scs_disc_figures_no_python3.cmake)
endif()
set_tests_properties(scs_disc_figures PROPERTIES
    LABELS "unit;scs;cluster;docs"
    TIMEOUT 60
)

# vms-a58 - THE TYPES-8/9 GATE. Message types 8 and 9 had been read three
# different ways in this repo's own documents ("a ninth and tenth
# connection-control message", "the SCA credit-flow control pair", "constant
# credit, so not a credit message") and no gate read a census of them at all,
# so nothing could tell the readings apart.
#
# vms-a58 measured the length-unrestricted, OUI-split census and, decisively,
# replayed the p. 2-43 credit LEDGER frame by frame: it predicts [48:50] on 938
# of 938 frames with zero residuals, which makes the constant 1 a COUNT (not a
# version, not a flag) and puts types 8 and 9 INSIDE the credit account that
# every connection-control type sits outside. The names remain unknown and the
# gate keeps them unnamed.
#
# It pins the four spec tables and scsd.c's T89-CENSUS-INV ruling lines to
# tools/cluster/scs_t89_measure.py's EXPECTED, reds if any of the four
# eliminated readings is written back down as an identification, and reds if
# the honest limit (the owning-SYSAP census CANNOT discriminate, because the
# entire teardown population is one SYSAP) is deleted. On a lab host it also
# re-derives every figure from the packets via scs_wire.gate().
if(Python3_Interpreter_FOUND)
    add_test(NAME scs_t89_figures
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_scs_t89_figures.py)
else()
    add_test(NAME scs_t89_figures
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/scs_t89_figures_no_python3.cmake)
endif()
set_tests_properties(scs_t89_figures PROPERTIES
    LABELS "unit;scs;cluster;docs"
    TIMEOUT 120
)

# vms-a58 - THE MUTATION BATTERY for scs_t89_figures. Nine mutants against
# COPIES of the two documents that gate reads; it must red for every one. It
# has already found a real defect in the gate -- a sentence splitter that cut
# "p. 2-44" in half, so no claim stated with a page cite could be seen, and the
# "types 8 and 9 ARE the special credit message" mutant walked through it.
# The mutant and kill counts are PRINTED by the test and deliberately not
# restated here: a second copy of a figure is how a drift in one copy hides.
if(Python3_Interpreter_FOUND)
    add_test(NAME scs_t89_mutants
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_scs_t89_mutants.py)
else()
    add_test(NAME scs_t89_mutants
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/scs_t89_mutants_no_python3.cmake)
endif()
set_tests_properties(scs_t89_mutants PROPERTIES
    LABELS "unit;scs;cluster;docs"
    TIMEOUT 900
)

# vms-0fe - THE PORT-LAYER (PPD) FIGURES GATE.
#
# WHY IT EXISTS: the "%PEA0, Inappropriate SCA Control Message -
# FLAGS/OPC/STATUS/PORT 00/22/00/DD" console line was carried through two items
# as evidence that a real VAX RECEIVES and REFUSES OVMX's DISCONNECT_REQ. It
# never meant that -- vms-096 measured the SCA answer (10/10, sub-millisecond)
# and vms-0fe read the line off VMS's own shipped decode tables (HELP/MESSAGE,
# ANALYZE/ERROR_LOG, $SCSDEF in LIB.MLB): it is a PORT-layer message whose
# documented effect is a port-to-port VC close, and its four fields are the PPD
# header, not the SCA connection-control message type.
#
# The gate pins those figures ONE COPY EACH in docs/cluster-protocol-spec.md,
# pins the NEGATIVE reproduction result (four fresh lab-2 runs including a
# matched OVMX_NO_CLEAN_SHUTDOWN=1 control that put zero disconnect frames on
# the wire -- the line appeared in none of them), and quarantines both dead
# readings so they cannot be re-asserted in the spec, in scs_disc.h or in
# scsd.c. Needs no captures and no lab. Registered unconditionally: a missing
# interpreter must make the gate RED, never make it vanish.
if(Python3_Interpreter_FOUND)
    add_test(NAME scs_ppd_figures
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_scs_ppd_figures.py)
else()
    add_test(NAME scs_ppd_figures
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/scs_ppd_figures_no_python3.cmake)
endif()
set_tests_properties(scs_ppd_figures PROPERTIES
    LABELS "unit;scs;cluster;docs"
    TIMEOUT 60
)

# vms-66f, review round 2 - THE SCS$DIRECTORY ROLE / LOOKUP-CREDIT GATE.
#
# WHY IT EXISTS: two claims shipped in round 1 that the capture they cited
# REFUTES. docs/cluster-protocol-spec.md sec 4(h)(2a) and src/vmsscs/scsd.c said
# the reference joiner "only answers" and connects "without having polled
# anybody" (the roles are inverted: the ESTABLISHED MEMBER sends the only
# VMS$VAXcluster CONNECT_REQ, and the joiner does poll, later); and
# src/vmsscs/scs_dir.c commented SCA [48:50] as a fixed request/response flag
# (it is the sec 4(d) credit field, and 1 appears on four of six requests).
# Nothing in ctest read the capture, so both survived a green run.
#
# test_scs_dir_figures.py reads the EXPECTED table out of
# tools/cluster/scs_dir_role_measure.py and requires every figure the prose
# states to match it -- each frame WITH ITS DIRECTION, not a bare digit -- and
# reds if either dead sentence appears outside an explicit
# REFUTED-QUOTE-BEGIN/END block. It needs no captures. The capture half is
# scs_dir_role_measure.py itself; run it on a lab host.
#
# NO COVERAGE NUMBER LIVES IN THIS COMMENT -- scs_dir_mutants below measures it
# on every run. Registered unconditionally: a missing interpreter must make the
# gate RED, never make it vanish.
if(Python3_Interpreter_FOUND)
    add_test(NAME scs_dir_figures
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_scs_dir_figures.py)
else()
    add_test(NAME scs_dir_figures
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/scs_dir_figures_no_python3.cmake)
endif()
set_tests_properties(scs_dir_figures PROPERTIES
    LABELS "unit;scs;cluster;docs"
    TIMEOUT 60
)

# vms-591 - THE MUTATION BATTERY for scs_disc_figures. See the file header: it
# found two real defects in that gate (a quarantine parser that accepted nested
# markers, and a claim matcher that missed "neither 5 nor 7"), which is the
# whole argument for having it. The mutant count and the kill count are PRINTED
# by the test and deliberately not restated here -- a second copy of a figure is
# how the vms-6b3 gate got rejected twice.
if(Python3_Interpreter_FOUND)
    add_test(NAME scs_disc_mutants
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_scs_disc_mutants.py)
else()
    add_test(NAME scs_disc_mutants
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/scs_disc_mutants_no_python3.cmake)
endif()
set_tests_properties(scs_disc_mutants PROPERTIES
    LABELS "unit;scs;cluster;docs"
    TIMEOUT 300
)

# vms-66f, review round 2 - THE MUTATION BATTERY FOR THE GATE ABOVE.
#
# The first draft of scs_dir_figures checked the dead sentences with a PROXIMITY
# WINDOW, which is the artifact vms-6b3 was rejected for: inside documents that
# are entirely ABOUT the refutation, the excuse words are everywhere, so a
# re-assertion three lines under the correction passes. test_scs_dir_mutants.py
# copies every input into a scratch tree, proves the UNMUTATED copy green (the
# control), applies each mutant alone via the OVMX_SCS_DIR_ROOT override,
# requires a non-zero exit, and restores and re-verifies the control after every
# one. A mutant that does not change the file is reported as a failure, never
# scored as a kill. Nothing under src/, docs/, tools/ or tests/ is written.
#
# Classes covered: drifted figures in the prose and in EXPECTED (including the
# 0-based/1-based frame-number slip that caused the original error); the dead
# sentences re-asserted at eight natural sites, one of them directly under the
# correction; four attacks on the quarantine mechanism; the deleted tool
# citation and the deleted sec 4(d) attribution; and a template relabelled back
# to "flag". The COUNT is printed by the test, not restated here.
if(Python3_Interpreter_FOUND)
    add_test(NAME scs_dir_mutants
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_scs_dir_mutants.py)
else()
    add_test(NAME scs_dir_mutants
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/scs_dir_mutants_no_python3.cmake)
endif()
set_tests_properties(scs_dir_mutants PROPERTIES
    LABELS "unit;scs;cluster;docs"
    TIMEOUT 300
)

# vms-70e2 - THE JOIN-CAPABILITY GATE.
#
# WHY IT EXISTS: vms-70e2 set out to prove a same-identity REJOIN end to end and
# never reached the question, because its POSITIVE CONTROL failed -- an SCSD
# built from work/vms-187-closure cannot complete a FIRST join either. A matched
# bracket on one lab-2 pod (three runs, twelve minutes, only the binary varying,
# each identity proven from the capture bytes) measured what that binary does
# and does not put on the wire, and docs/cluster-protocol-spec.md sec 4(O)
# records it.
#
# Needs no captures. It drives tools/cluster/scs_join_capability_measure.py's
# decoder with synthesized frames whose answer is known by construction, pins
# every figure in EXPECTED to sec 4(O), and quarantines the claim this item
# refuted: scs_sdir.h justified a deliberate deviation from p. 2-50 partly on
# "OVMX HAS NEVER OBSERVED AN ACCEPT_RSP ADDRESSED TO ITSELF", and run A1
# contains one -- addressed to the OVMX MAC, carrying OVMX's own Con.ID pair,
# 0.5 ms after OVMX's ACCEPT_REQ. On a lab host that also holds the captures the
# test re-derives EXPECTED from them; on any other host it says so and the other
# three checks still gate.
if(Python3_Interpreter_FOUND)
    add_test(NAME scs_join_capability_figures
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_scs_join_capability_figures.py)
else()
    add_test(NAME scs_join_capability_figures
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/scs_join_capability_figures_no_python3.cmake)
endif()
set_tests_properties(scs_join_capability_figures PROPERTIES
    LABELS "unit;scs;cluster;docs"
    TIMEOUT 120
)

# vms-70e2 - THE MUTATION BATTERY FOR THE GATE ABOVE.
#
# Copies the measurement, the spec and scs_sdir.h into a scratch tree, proves
# the unmutated copy green, then applies each mutant alone through the
# OVMX_SCS_JOINCAP_{MEASURE,SPEC,HEADER} overrides and requires a non-zero exit,
# restoring and re-verifying between each. A mutant that does not change the
# file is scored as a FAILURE, never a kill. The captures are hidden from the
# gate during the battery, so what is scored is the part that runs on every
# host including CI. Classes covered: the decoder's direction, length classes,
# message-type offset and identity harvest; the checked-in figures and the
# self-consistency of the bracket in EXPECTED; the sec 4(O) prose including its
# explicit non-claim; and four attacks on the quarantine. The COUNT is printed
# by the test, not restated here.
if(Python3_Interpreter_FOUND)
    add_test(NAME scs_join_capability_mutants
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_scs_join_capability_mutants.py)
else()
    add_test(NAME scs_join_capability_mutants
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/scs_join_capability_mutants_no_python3.cmake)
endif()
set_tests_properties(scs_join_capability_mutants PROPERTIES
    LABELS "unit;scs;cluster;docs"
    TIMEOUT 300
)

# vms-449 - THE MUTATION BATTERY FOR THE REJOIN BRACKET'S SHAPE.
#
# vms-449 asked vms-2f3's question on the tree that joins and recorded the
# answer: a returning OVMX identity is REFUSED, four times, on a virgin pod,
# with four fresh identities admitted around it (spec sec 4(O.2),
# EXPECTED_449). A negative result of that cost is only worth what the
# experiment behind it was worth, and every property that makes it an
# experiment rather than nine runs -- controls BETWEEN the tests and closing
# the bracket, three-or-more rejoins, one subject identity no control shares,
# every control joining -- lives in check_449_bracket_shape().
#
# It scores a SECOND battery against check_449r_bracket_shape(), the vms-449
# replication on a different pod (spec sec 4(O.3), EXPECTED_449R). That bracket
# is deliberately weaker -- one rejoin, not three -- so it gets its own check
# and its own mutants; the extra rules it DOES carry are the cross-checks that
# make it a replication at all (different pod, different tap MAC, different
# subject identity from EXPECTED_449). The two are scored separately on
# purpose: folded together, a mutant of one could be "killed" by the other's
# rules and score a pass it did not earn.
#
# So this gate does not assert the check passes. It applies each mutant alone
# and requires the check to RED, restoring and re-verifying between each; a
# mutant that changes nothing is scored a FAILURE, never a kill. Needs no
# captures, no lab and no network: it runs everywhere including CI, which is
# the half of the measurement CI can actually hold. The COUNT is printed by the
# test, not restated here.
if(Python3_Interpreter_FOUND)
    add_test(NAME scs_449_bracket_shape
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_scs_449_bracket_shape.py)
else()
    add_test(NAME scs_449_bracket_shape
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/scs_449_bracket_shape_no_python3.cmake)
endif()
set_tests_properties(scs_449_bracket_shape PROPERTIES
    LABELS "unit;scs;cluster;docs"
    TIMEOUT 120
)
# vms-760 - MSCP-over-SCS disk-client command builders: SET CONTROLLER
# CHARACTERISTICS (0x04) + GET UNIT STATUS (0x03) byte-exact vs the golden af2
# joiner commands, plus the END-response parser (0x84/0x83, status + returned
# unit-word that drives the NEXT-UNIT enumeration). The disk-discovery the
# joiner must complete after NEW before an established VAX1 commits it to MEMBER.
add_executable(test_scs_mscp test_scs_mscp.c)

target_include_directories(test_scs_mscp PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vmsscs/include
)

target_link_libraries(test_scs_mscp PRIVATE vmsscs_mscp)

add_test(NAME vmsscs_mscp_unit COMMAND test_scs_mscp)
set_tests_properties(vmsscs_mscp_unit PROPERTIES
    LABELS "unit;scs;cluster"
    TIMEOUT 30
)

# vms-371 - THE BATTERY THAT PROVES THE SIX FIGURES GATES CAN FAIL.
#
# MEASURED FAILURE THIS EXISTS FOR: `ctest -L scs` was 32/32 GREEN with
# scs_disc_figures, scs_credit_figures, scs_reason_figures and
# scs_connect_data_figures all passing, while the four tools those gates cite
# were FAILING -- 23, 11, 13 and 18 checks red respectively. Every gate
# string-matched prose against a checked-in EXPECTED table and never opened a
# pcap, so a table that drifted WITH the prose was invisible.
#
# The fix is in tests/vmsscs/scs_wire.py: on a host with the lab captures each
# gate now calls the measurement tool's rederive() and reds on any figure the
# packets no longer support; on a host without them it prints a banner instead
# of passing quietly, and OVMX_SCS_REQUIRE_WIRE=1 makes the absence itself red.
#
# THIS test is what keeps that honest. Per gate it scores: a wire mutation (a
# capture truncated to zero records), a hollowed-out rederive(), an EXPECTED
# figure nothing measures, and the same-size same-second edit behind a PRIMED
# __pycache__ -- the mutation that survives any importlib file loader, run with
# the wire arm OFF so only the loader can kill it. The battery proves the
# staleness in-process before scoring that one, and requires each control to be
# green AND to show the wire arm actually ran.
#
# NO COVERAGE NUMBER LIVES IN THIS COMMENT -- the battery prints how many
# mutants it scored and how many it killed on every run. Read its output.
#
# On a host without captures the packet-dependent arms report SKIPPED by name
# and the other two still score, so this is a real test in CI too.
#
# Registered unconditionally, exactly like scs_credit_figures above.
if(Python3_Interpreter_FOUND)
    add_test(NAME scs_figures_wire_mutants
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_scs_figures_wire_mutants.py)
else()
    add_test(NAME scs_figures_wire_mutants
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/scs_figures_wire_mutants_no_python3.cmake)
endif()
set_tests_properties(scs_figures_wire_mutants PROPERTIES
    LABELS "unit;scs;cluster;docs"
    TIMEOUT 900
)

# vms-ebb - THE DISK-DISCOVERY TRIGGER RULING, AND THE BATTERY THAT PROVES THE
# GATE READS IT.
#
# vms-096 deleted a dead immediate trigger and left the choice -- one trigger or
# two -- to a comment. A ruling in a comment decays: sec 4(O.1)'s bracket ran
# the DEFAULT environment and never entered the block at all, so nothing in
# ctest read the comment or could contradict it. vms-ebb ran the missing
# pure-server bracket (spec sec 4(O.4)) and these two tests hold the documents
# to it WITHOUT needing the captures, which are host-only under CLAUDE.md rule 8.
#
# The one thing they exist above all to stop: the FIRST draft of this ruling
# claimed the peer never sends the DISCONNECT_REQ an immediate trigger would
# fire on. The bracket measured the opposite -- 2 per run, 3 of 3 arms. That
# claim family is now a red.
if(Python3_Interpreter_FOUND)
    add_test(NAME scs_diskrun_figures
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_scs_diskrun_figures.py)
else()
    add_test(NAME scs_diskrun_figures
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/scs_diskrun_figures_no_python3.cmake)
endif()
set_tests_properties(scs_diskrun_figures PROPERTIES
    LABELS "unit;scs;cluster;docs"
    TIMEOUT 60
)

if(Python3_Interpreter_FOUND)
    add_test(NAME scs_diskrun_mutants
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_scs_diskrun_mutants.py)
else()
    add_test(NAME scs_diskrun_mutants
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/scs_diskrun_mutants_no_python3.cmake)
endif()
set_tests_properties(scs_diskrun_mutants PROPERTIES
    LABELS "unit;scs;cluster;docs"
    TIMEOUT 300
)

# vms-4eb - THE 110-CONTENT TYPE-10 DECODE.
#
# Type 10 was the MAJORITY of what real VAXes put on the 110-byte 0x?B13 class
# (2 889 of 4 314 frames) and the spec carried it as a TAXON -- "the SCS
# application-message MTYPE" -- which says nothing about what the message
# contains. spec sec 4(h)(1e) now decodes it as the MSCP GET UNIT STATUS end
# message against the PUBLIC AA-L619A-TK manual, and this gate holds the
# document to tools/cluster/scs_type10_measure.py's EXPECTED table. On a host
# with the (host-only, CLAUDE.md rule 8) lab captures it also re-derives
# EXPECTED from the packets via scs_wire.gate().
#
# The two things it exists above all to stop: (1) the decode's admitted gaps --
# body[48:52] and the unit-flags bit Table A-5 does not define -- being quietly
# NAMED, which is the exact failure this epic already shipped once ("grounded"
# with zero observations, and wrong); (2) OVMX emitting a 110-content type-10
# frame, which is an MSCP END message and is gated behind
# docs/design-mscp-direction.md Phase D.
if(Python3_Interpreter_FOUND)
    add_test(NAME scs_type10_figures
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_scs_type10_figures.py)
else()
    add_test(NAME scs_type10_figures
             COMMAND ${CMAKE_COMMAND}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/scs_type10_figures_no_python3.cmake)
endif()
set_tests_properties(scs_type10_figures PROPERTIES
    LABELS "unit;scs;cluster;docs"
    TIMEOUT 120
)
