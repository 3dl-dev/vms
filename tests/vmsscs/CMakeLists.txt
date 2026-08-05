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
