#!/bin/sh
#
# host_defects.sh - the R1 host-side mutation manifest for
# tests/cluster/host (vms-8e7).
#
# WHY THIS EXISTS
#
# tests/qemu/facility_defects.sh proves the QEMU/real-/dev/vms executive
# facilities can go red under an injected defect. The 46 R1 host suites in
# THIS directory (tests/cluster/host/) had NO analogous gate: nothing proved
# a single one of them could ever go red. A suite that has never been shown
# capable of failing is not a control, it is a tautology with printf calls.
#
# This file is the FIRST INCREMENT of that gate: one minimal defect, so the
# pattern exists and every later Bucket D/E TU can be added the same way --
# one DEFECTS entry, one `defect_field` case, one `apply_edit` case, one
# `/* negctl: <name> */` anchor above the assertion(s) it names.
#
# SHAPE, DELIBERATELY BORROWED FROM tests/qemu/facility_defects.sh
#
# Same idiom (DEFECTS list, a per-defect `case` exposing facility/targets/
# suites_red/isolation/why/require_fail, an `apply` subcommand that sed(1)s
# the source and PROVES the edit landed by comparing against a pristine copy,
# a `list` subcommand, and a `selftest` that applies to a throwaway copy and
# asserts the anchor still matches) -- but HOST-NATIVE: no QEMU, no /dev/vms,
# no vms.ko. tests/cluster/host builds with a plain host C compiler against
# src/kernel-core alone (see tests/cluster/host/CMakeLists.txt's own header),
# so the driver that actually EXECUTES a mutation
# (tests/cluster/host/run_host_negctl.sh) is a cmake build + run, not a QEMU
# boot. THIS FILE, like facility_defects.sh, is otherwise STATIC: `list`,
# `field` and the injection half of `apply` touch no suite and prove nothing
# about what goes red. Only run_host_negctl.sh executes anything.
#
# NOT PORTED (deliberately, this increment): the coverage/floor/scope-out
# machinery (facility_defects.sh's cmd_coverage, the count floor, the
# SCOPE_OUT_* declarations). With one defect there is nothing to floor or
# scope; that machinery belongs to the item that grows DEFECTS to cover the
# other 17 Bucket D/E TUs.
#
# THE ONE DEFECT (vms-8e7)
#
#   coord-genesis-refusal-uncounted   src/kernel-core/vms_cnxman_coord_fsm.c
#   drops the `c->genesis_refused_noquorum++;` counter bump inside the
#   NO_QUORUM refusal branch of cnxman_coord_found()'s genesis path. The
#   refusal itself (coord_found_refuse(..., CNXMAN_COORD_REF_NO_QUORUM, ...))
#   is UNTOUCHED -- a node that may not found still refuses to found -- only
#   the anti-LARP tripwire that counts how many times it refused goes silent.
#   That is exactly the shape of defect this suite's own header warns about:
#   "a thousand refusals must still be a thousand refusals -- nothing
#   accumulates toward a mint", proven only by a counter nobody would notice
#   is wrong from the refusal's rc/state/CLUB-unchanged side alone.
#
# GROWN (vms-55e) to six more Bucket-D TUs, each the same shape: one minimal
# single-property mutation, isolated to ONE existing R1 suite, with its
# require_fail set MEASURED (not guessed) against a real host build --
#
#   codec-vc-zero-incarnation-not-refused    vms_cluster_codec_vc.c
#   codec-cm-short-body-not-refused          vms_cluster_codec_cm.c
#   codec-blk-no-trailer-not-honest          vms_cluster_codec_blk.c
#   barrier-bit0-uncounted                   vms_cnxman_barrier_fsm.c
#   phase2-count-mismatch-uncounted          vms_cnxman_phase2.c
#   recnx-last-gasp-uncounted                vms_cnxman_recnx_fsm.c
#   ldwv-refusal-uncounted                   vms_dlm_ldwv.c
#
# (vms_dlm_ldwv.c above -- FC-P4.3's Lock Directory Weight Vector -- was the
# seventh TU named by the item that grew this manifest; see its own defect
# entry below for the shape.)
#
# GROWN (vms-b2a) to Bucket E: the ten MSCP/DLM TUs (vms_cluster_codec_dlm.c,
# vms_dlm_scs_fsm.c, vms_cluster_codec_mscp.c, vms_mscp_cl.c,
# vms_mscp_cl_conn_fsm.c, vms_mscp_cl_fsm.c, vms_mscp_cl_io_fsm.c,
# vms_mscp_srv.c, vms_mscp_srv_fsm.c, vms_mscp_srv_io.c). Three of these
# (vms_mscp_cl.c, vms_mscp_srv.c, vms_mscp_srv_io.c) are GLUE that names
# exec_kbackend.h and so is never compiled by any host target -- their
# existing suites (test_mscp_cl.c, test_mscp_srv.c) instead source-scan the
# SHIPPING file at runtime (the same two-proof shape test_cnxman_glue.c
# established). For those three, apply_edit inserts one forbidden substring
# rather than disarming a guard: the suite's own NEGATIVE half already reads
# "this substring must NEVER appear in the glue" (the pure layer must own
# the property, not the fork-context glue), so making it appear is the
# minimal single-property injection for a file nothing ever compiles here.
#
#   dlm-lkid-guard-disabled              vms_cluster_codec_dlm.c
#   dlm-requester-hash-refusal-uncounted vms_dlm_scs_fsm.c
#   codec-mscp-gus-tail2-invented        vms_cluster_codec_mscp.c
#   mscp-cl-glue-device-name-leaked      vms_mscp_cl.c
#   mscp-cl-conn-refusal-uncounted       vms_mscp_cl_conn_fsm.c
#   mscp-cl-fsm-unit-uncounted           vms_mscp_cl_fsm.c
#   mscp-cl-io-empty-cell-uncounted      vms_mscp_cl_io_fsm.c
#   mscp-srv-glue-end-message-leaked     vms_mscp_srv.c
#   mscp-srv-fsm-writeprotect-uncounted  vms_mscp_srv_fsm.c
#   mscp-srv-io-worker-registers-handler vms_mscp_srv_io.c
#
# GROWN (vms-4f0) with the defect that BLOCKED CN=3 against a real OpenVMS VAX
# V7.3 -- a member that does not answer the coordinator's op-0x12 relay:
#
#   join-relay-unanswered                vms_cnxman_join_fsm.c
#
SELF="$0"

DEFECTS="coord-genesis-refusal-uncounted
codec-vc-zero-incarnation-not-refused
codec-cm-short-body-not-refused
codec-blk-no-trailer-not-honest
barrier-bit0-uncounted
phase2-count-mismatch-uncounted
recnx-last-gasp-uncounted
ldwv-refusal-uncounted
dlm-lkid-guard-disabled
dlm-requester-hash-refusal-uncounted
codec-mscp-gus-tail2-invented
mscp-cl-glue-device-name-leaked
mscp-cl-conn-refusal-uncounted
mscp-cl-fsm-unit-uncounted
mscp-cl-io-empty-cell-uncounted
mscp-srv-glue-end-message-leaked
mscp-srv-fsm-writeprotect-uncounted
mscp-srv-io-worker-registers-handler
join-own-connect-not-suppressed
join-relay-unanswered"

# ---------------------------------------------------------------------------
# HOST_OWNED_UNITS (vms-181, 2026-09-13)
#
# The cluster-family kernel-core translation-unit domain THIS ladder OWNS --
# every cluster-family TU under src/kernel-core/ MINUS the 6 Bucket-A TUs
# tests/qemu/facility_defects.sh already reaches single-node (vms_cluster_
# api.c, vms_cluster_fork.c, vms_cluster_fork_bind.c, vms_cnxman.c,
# vms_pe.c, vms_scs.c). Those 6 execute on a lone QEMU guest with no peer at
# all, so that gate covers them; every TU below executes only against a REAL
# peer that speaks SCA back (a join, a barrier, a directory lookup, an MSCP
# server accepting a connect) -- something a single-node VAXCLUSTER=0 QEMU
# guest structurally cannot produce -- so it is THIS ladder's to floor.
#
# STATIC, not derived from a directory listing: peer-vs-single-node is a
# human judgment call, not a mechanical property of a file's name or
# location (docs/design/negctl-coverage-paydown.md SS0.1). `cmd_owned` below
# hands this list, live, to facility_defects.sh's cmd_coverage, which trusts
# it as ITS derived scope-out for section 1 -- a TU named here is this
# ladder's coverage problem (cmd_coverage below), not that script's.
# ---------------------------------------------------------------------------
HOST_OWNED_UNITS="kernel-core/vms_cluster_codec.c
kernel-core/vms_cluster_codec_blk.c
kernel-core/vms_cluster_codec_cm.c
kernel-core/vms_cluster_codec_dlm.c
kernel-core/vms_cluster_codec_hello.c
kernel-core/vms_cluster_codec_mscp.c
kernel-core/vms_cluster_codec_scs.c
kernel-core/vms_cluster_codec_vc.c
kernel-core/vms_cluster_emit_guard.c
kernel-core/vms_cluster_sysgen.c
kernel-core/vms_cnxman_barrier_fsm.c
kernel-core/vms_cnxman_coord_fsm.c
kernel-core/vms_cnxman_csb.c
kernel-core/vms_cnxman_diag.c
kernel-core/vms_cnxman_join_fsm.c
kernel-core/vms_cnxman_phase2.c
kernel-core/vms_cnxman_quorum.c
kernel-core/vms_cnxman_recnx_fsm.c
kernel-core/vms_dlm_ldwv.c
kernel-core/vms_dlm_scs.c
kernel-core/vms_dlm_scs_fsm.c
kernel-core/vms_mscp_cl.c
kernel-core/vms_mscp_cl_conn_fsm.c
kernel-core/vms_mscp_cl_fsm.c
kernel-core/vms_mscp_cl_io_fsm.c
kernel-core/vms_mscp_srv.c
kernel-core/vms_mscp_srv_fsm.c
kernel-core/vms_mscp_srv_io.c
kernel-core/vms_pe_fsm.c
kernel-core/vms_scs_dir.c
kernel-core/vms_scs_fsm.c"

cmd_owned() { echo "$HOST_OWNED_UNITS"; }

# ---------------------------------------------------------------------------
# Metadata (same field meanings as tests/qemu/facility_defects.sh):
#   facility     human-readable name of the property under test.
#   targets      source files, relative to a src/ root, the mutation edits.
#   suites_red   the suite(s) this defect is allowed to redden.
#   isolation    isolated - every suite must produce a verdict, no suite
#                           outside suites_red may fail.
#   why          the single sentence describing the mutation and its effect.
#   require_fail assertion texts (the ct_check/ct_check_eq_u32 `what` label,
#                with any ": got ..." tail stripped) that MUST appear in the
#                observed red set, and the ONLY texts that may -- run_host_
#                negctl.sh asserts the two sets are EQUAL.
# ---------------------------------------------------------------------------
defect_field() {
    _d="$1"; _f="$2"
    case "$_d" in

    coord-genesis-refusal-uncounted)
        case "$_f" in
        facility)     echo "cluster GENESIS founding-refusal accounting (cnxman_coord_found(), the anti-LARP tripwire docs/design-cluster-genesis.md warns must never drift)";;
        targets)      echo "kernel-core/vms_cnxman_coord_fsm.c";;
        suites_red)   echo "test_cnxman_genesis_negctl";;
        isolation)    echo "isolated";;
        why)          echo "cnxman_coord_found()'s NO_QUORUM branch stops incrementing genesis_refused_noquorum. The refusal itself (rc, last_refusal, state, and the byte-for-byte CLUB) is untouched -- only the count of how many times the node refused to found goes silent, so a node that should show N counted refusals shows one fewer than it really performed.";;
        require_fail) cat <<'EOF'
the refusal is COUNTED (the anti-LARP tripwire)
counted
every attempt was refused, and counted
EOF
                      ;;
        esac;;

    codec-vc-zero-incarnation-not-refused)
        case "$_f" in
        facility)     echo "the §4(i).B incarnation-echo INV-6 gate in vms_scs_seq_stamp() (test_codec_vc.c's E66 case: a zero incarnation is never an honest value to stamp)";;
        targets)      echo "kernel-core/vms_cluster_codec_vc.c";;
        suites_red)   echo "test_codec_vc";;
        isolation)    echo "isolated";;
        why)          echo "vms_scs_seq_stamp()'s 'incarnation == 0 -> VMS_CODEC_E_INVAL' refusal is disabled, so a zero incarnation -- which appears in 0 of 239,981 reference frames -- is stamped onto the wire like any other value instead of being refused.";;
        require_fail) cat <<'EOF'
a zero incarnation is REFUSED, never written
and the refused frame's span was not touched at all
EOF
                      ;;
        esac;;

    codec-cm-short-body-not-refused)
        case "$_f" in
        facility)     echo "vms_cm_body_build()'s body_len length gate (the DLM-reply wrapper must never tail-pad a short caller buffer)";;
        targets)      echo "kernel-core/vms_cluster_codec_cm.c";;
        suites_red)   echo "test_codec_cm";;
        isolation)    echo "isolated";;
        why)          echo "vms_cm_body_build()'s 'body_len != VMS_CM_BODY_LEN -> VMS_CODEC_E_INVAL' refusal is disabled, so a body one byte short of the 132-byte SYSAP body is wrapped and sent anyway instead of being refused.";;
        require_fail) cat <<'EOF'
  a short body is rejected rather than tail-padded with whatever the caller's buffer held
EOF
                      ;;
        esac;;

    codec-blk-no-trailer-not-honest)
        case "$_f" in
        facility)     echo "vms_blk_trailer_parse()'s 'no trailer' honesty rule (design's TRAP 1: frame_len == inner_frame_len means nothing was piggybacked, not an error)";;
        targets)      echo "kernel-core/vms_cluster_codec_blk.c";;
        suites_red)   echo "test_pe_block";;
        isolation)    echo "isolated";;
        why)          echo "vms_blk_trailer_parse()'s early 'frame_len <= inner_frame_len -> VMS_CODEC_OK, no trailer' return is disabled, so the exact-length case that legitimately carries no piggyback falls through to the length arithmetic below and is refused VMS_CODEC_E_SHORT instead of being answered as the common, honest, no-trailer case. (A real READ end message always carries at least the 28-byte header, so this path is only exercised by TRAP 1's own synthetic case -- a receiver bounding the frame at the message's OWN declared length, which never sees the trailer at all.)";;
        require_fail) cat <<'EOF'
parsing with the DECLARED bound is not an error
EOF
                      ;;
        esac;;

    barrier-bit0-uncounted)
        case "$_f" in
        facility)     echo "the barrier's nodemap bit-0 instrumentation (book p. 7-25: CSV slot 0 is never used, so a set bit 0 is a width tell that must be COUNTED, never silently folded into the popcount)";;
        targets)      echo "kernel-core/vms_cnxman_barrier_fsm.c";;
        suites_red)   echo "test_cnxman_barrier";;
        isolation)    echo "isolated";;
        why)          echo "the barrier's 'b->bitmap_bit0++;' instrumentation, taken when an OPEN/ADD's nodemap byte has the impossible bit 0 set, is dropped. The bitmap is still read and processed identically -- only the counter that tells an operator the impossible bit fired goes silent.";;
        require_fail) cat <<'EOF'
the impossible bit is counted
EOF
                      ;;
        esac;;

    phase2-count-mismatch-uncounted)
        case "$_f" in
        facility)     echo "the p. 7-42 Phase 2 commit's count-mismatch accounting (phase2_commit_count(): a disagreement between the committed CSB count and the wire's own popcount is the tell for a nodemap byte too narrow to read)";;
        targets)      echo "kernel-core/vms_cnxman_phase2.c";;
        suites_red)   echo "test_cnxman_barrier";;
        isolation)    echo "isolated";;
        why)          echo "phase2_commit_count()'s 'st->count_mismatch++;' is dropped from the branch that fires when the committed member count differs from the transition's own nodemap popcount. The count itself (and the %CNXMAN log line) is still computed and logged correctly -- only the counter an operator or a later test reads back goes silent.";;
        require_fail) cat <<'EOF'
and the disagreement with the nodemap is COUNTED -- which is exactly the width tell
counted
EOF
                      ;;
        esac;;

    recnx-last-gasp-uncounted)
        case "$_f" in
        facility)     echo "the p. 7-29/7-49 last-gasp accounting (cnxman_recnx_shutdown(): the SHUTDOWN datagram this node emits to the peers it is leaving must be counted, not just sent)";;
        targets)      echo "kernel-core/vms_cnxman_recnx_fsm.c";;
        suites_red)   echo "test_cnxman_recnx";;
        isolation)    echo "isolated";;
        why)          echo "cnxman_recnx_shutdown()'s 'r->last_gasps++;' is dropped. The CLUB/CSB SHUTDOWN flags are still set and the last-gasp record is still emitted to the caller -- only the counter that tells an operator one was sent goes silent.";;
        require_fail) cat <<'EOF'
counted once
EOF
                      ;;
        esac;;

    ldwv-refusal-uncounted)
        case "$_f" in
        facility)     echo "the p. 6-33 Lock Directory Weight Vector's refusal accounting (cnxman_ldwv_rebuild(): a mixed-kind or foreign-member refusal must be counted, the rd vms-1ee split-brain teeth)";;
        targets)      echo "kernel-core/vms_dlm_ldwv.c";;
        suites_red)   echo "test_dlm_ldwv";;
        isolation)    echo "isolated";;
        why)          echo "cnxman_ldwv_rebuild()'s 'club->ldwv_build_refused++;' in the survey-verdict refusal branch (VMS_LDWV_E_WEIGHTS/VMS_LDWV_E_FOREIGN) is dropped -- range-anchored to that FIRST refusal site only, leaving the second (ldwv_fill_club's own VMS_LDWV_E_TOOBIG path) untouched. The refusal itself (no vector left behind, the %CNXMAN log line) still fires -- only the count goes silent.";;
        require_fail) cat <<'EOF'
and counted in the CLUB
the refusal is counted
EOF
                      ;;
        esac;;

    dlm-lkid-guard-disabled)
        case "$_f" in
        facility)     echo "dlm_lkid_pair_put()'s fc8540ae INVLOCKID guard (the 'no lock-id-bearing builder accepts a placeholder lock id' rule test_codec_dlm.c's own header names as THE HARD LESSON -- a placeholder lock id crashed a real VAX)";;
        targets)      echo "kernel-core/vms_cluster_codec_dlm.c";;
        suites_red)   echo "test_codec_dlm";;
        isolation)    echo "isolated";;
        why)          echo "dlm_lkid_pair_put()'s shared guard ('req_lkid == VMS_DLM_LKID_UNSET || master_lkid == VMS_DLM_LKID_UNSET -> VMS_CODEC_E_INVAL'), which both vms_dlm_deq_build() and vms_dlm_blkast_build() call before writing a single byte, is disarmed. A $DEQ or BLKAST naming lock-id 0 -- the exact fc8540ae shape -- is built and sent instead of refused.";;
        require_fail) cat <<'EOF'
  master_lkid==0 REFUSED on $DEQ (0x03)
  req_lkid==0 REFUSED on $DEQ (0x03)
  both lock ids 0 REFUSED (they do not cancel out)
  master_lkid==0 REFUSED on BLKAST (0x04)
  req_lkid==0 REFUSED on BLKAST (0x04)
*** a refused build wrote NO byte at all ***
EOF
                      ;;
        esac;;

    dlm-requester-hash-refusal-uncounted)
        case "$_f" in
        facility)     echo "the DLM requester's directory-lookup refusal accounting (dq_route_check(): a lookup with no wire-learned hash must be COUNTED, the property that cures the grant storm)";;
        targets)      echo "kernel-core/vms_dlm_scs_fsm.c";;
        suites_red)   echo "test_dlm_requester";;
        isolation)    echo "isolated";;
        why)          echo "dq_route_check()'s 'f->hash_unknown_refused++;' in the to-directory/no-known-hash branch is dropped -- range-anchored to that FIRST site only, leaving the second (the redirect-resolve path's own copy) untouched. The refusal itself (DLM_REQ_E_NOHASH, nothing sent) still fires -- only the count goes silent. This route check is the SAME one both a fresh post AND every retransmit of an already-outstanding request run through, so both scenarios in the suite redden -- MEASURED, not merely the one the docstring first names.";;
        require_fail) cat <<'EOF'
counted
every refused attempt was counted
EOF
                      ;;
        esac;;

    codec-mscp-gus-tail2-invented)
        case "$_f" in
        facility)     echo "vms_mscp_gus_end_build()'s INV-6 honesty rule for the undecoded GUS END tail (body[50:52] must stay the zero the initial put_zero left it, never an invented value)";;
        targets)      echo "kernel-core/vms_cluster_codec_mscp.c";;
        suites_red)   echo "test_codec_mscp";;
        isolation)    echo "isolated";;
        why)          echo "an extra vms_wire_put_le16() is inserted right after the OBSERVED body[48:50] tail write, stamping a fabricated 0xBEEF into body[50:52] -- the second half of the tail the header doc comment says must stay zero because it is undecoded, never invented.";;
        require_fail) cat <<'EOF'
body[50:52] is left zero, never invented
EOF
                      ;;
        esac;;

    mscp-cl-glue-device-name-leaked)
        case "$_f" in
        facility)     echo "the FC-P7.1 class-driver glue's own negative-half discipline (test_mscp_cl.c's test_glue_source(): the glue must derive NO device name of its own -- the pure driver spells it, not vms_mscp_cl.c)";;
        targets)      echo "kernel-core/vms_mscp_cl.c";;
        suites_red)   echo "test_mscp_cl";;
        isolation)    echo "isolated";;
        why)          echo "vms_mscp_cl.c is not host-linkable (it names exec_kbackend.h), so its properties are proved by a source-scan of the SHIPPING file; a '$DUA' literal is inserted into the file, which is exactly the property test_glue_source()'s negative half exists to catch -- the glue spelling a device name instead of reading one the pure driver already derived.";;
        require_fail) cat <<'EOF'
and it spells NO device name -- the pure driver derives it from values this file read out of the executive
EOF
                      ;;
        esac;;

    mscp-cl-conn-refusal-uncounted)
        case "$_f" in
        facility)     echo "the E64 connect-admission FSM's refusal accounting (h_present_sweep(): a connect the port declined must be COUNTED, and the leg backed off, not silently retried)";;
        targets)      echo "kernel-core/vms_mscp_cl_conn_fsm.c";;
        suites_red)   echo "test_mscp_cl_conn";;
        isolation)    echo "isolated";;
        why)          echo "h_present_sweep()'s 'c->connect_refusals++;' in the ops->connect()-returned-failure branch is dropped -- range-anchored to that SECOND site only, leaving the first (ops/ops->connect == NULL) untouched. The refusal itself (the leg goes back to IDLE, no conid is kept) still fires -- only the count an operator or a later retry-backoff check reads goes silent.";;
        require_fail) cat <<'EOF'
the refusal is counted
the next beat does NOT retry a second later
EOF
                      ;;
        esac;;

    mscp-cl-fsm-unit-uncounted)
        case "$_f" in
        facility)     echo "the FC-P3.4 discovery FSM's unit-enumeration accounting (vms_mscp_cl_fsm_on_gus_end(): each GUS END walked must be COUNTED, matching src/vmsscs/scs_mscp.c's own observed enumeration)";;
        targets)      echo "kernel-core/vms_mscp_cl_fsm.c";;
        suites_red)   echo "test_mscp_cl_fsm";;
        isolation)    echo "isolated";;
        why)          echo "vms_mscp_cl_fsm_on_gus_end()'s 'f->units_found++;' is dropped. The walk cursor (f->next_unit, read from the peer's own answer) still advances correctly and the caller's unit struct is still filled in -- only the running count of units discovered goes silent, in EVERY test that walks more than one unit -- MEASURED: the suite's own multi-unit walk asserts the final tally by name, not merely the single-unit case the docstring first names.";;
        require_fail) cat <<'EOF'
one unit counted
exactly the two AVAILABLE units were counted, not the OFFLINE terminator
EOF
                      ;;
        esac;;

    mscp-cl-io-empty-cell-uncounted)
        case "$_f" in
        facility)     echo "the class driver's dispatch-table empty-cell accounting (cl_dispatch(): an event a state has no edge for must be COUNTED, never silently dropped)";;
        targets)      echo "kernel-core/vms_mscp_cl_io_fsm.c";;
        suites_red)   echo "test_mscp_cl";;
        isolation)    echo "isolated";;
        why)          echo "cl_dispatch()'s 'f->ignored_events++;' in the empty-table-cell branch (a valid state/event pair with no handler) is dropped -- range-anchored to that site only, leaving the out-of-range guard's own copy above it untouched. The event is still discarded (no handler is invoked) -- only the count goes silent.";;
        require_fail) cat <<'EOF'
and the empty cell was COUNTED
EOF
                      ;;
        esac;;

    mscp-srv-glue-end-message-leaked)
        case "$_f" in
        facility)     echo "the FC-P6.3 server glue's own negative-half discipline (test_mscp_srv.c's test_glue_source(): the glue must build NO end message of its own -- the pure server composes it, not vms_mscp_srv.c)";;
        targets)      echo "kernel-core/vms_mscp_srv.c";;
        suites_red)   echo "test_mscp_srv";;
        isolation)    echo "isolated";;
        why)          echo "vms_mscp_srv.c is not host-linkable (it names exec_kbackend.h), so its properties are proved by a source-scan of the SHIPPING file; an '_end_build' literal is inserted into the file, which is exactly the property test_glue_source()'s negative half exists to catch -- the glue composing an end message instead of handing the transfer to the pure server that does.";;
        require_fail) cat <<'EOF'
and it builds NO end message
EOF
                      ;;
        esac;;

    mscp-srv-fsm-writeprotect-uncounted)
        case "$_f" in
        facility)     echo "the server's Table B-2 write-protect refusal accounting (srv_write_protect_status()'s caller: a WRITE refused for hardware or software protection must be COUNTED)";;
        targets)      echo "kernel-core/vms_mscp_srv_fsm.c";;
        suites_red)   echo "test_mscp_srv";;
        isolation)    echo "isolated";;
        why)          echo "the WRITE-opcode branch's 'f->write_protect_refusals++;' is dropped. The refusal itself (status 0x2006/WRITE_PROT, zero bytes claimed, no buffer named, nothing written) still fires exactly as Table B-2 requires -- only the count an operator reads back goes silent.";;
        require_fail) cat <<'EOF'
the refusal is counted
EOF
                      ;;
        esac;;

    mscp-srv-io-worker-registers-handler)
        case "$_f" in
        facility)     echo "FC-P6.6's own negative-half discipline (test_mscp_srv.c's test_glue_source(): the WORKER TU must register NO fork-context handler, or it could become fork-context code and stall the HELLO cadence design SS3.2.6 forbids)";;
        targets)      echo "kernel-core/vms_mscp_srv_io.c";;
        suites_red)   echo "test_mscp_srv";;
        isolation)    echo "isolated";;
        why)          echo "vms_mscp_srv_io.c is not host-linkable (it names exec_kbackend.h), so its properties are proved by a source-scan of the SHIPPING file; a 'cf_set_work_handler' literal is inserted into the file, which is exactly the property test_glue_source()'s negative half exists to catch -- the worker TU registering a fork-context handler, which design SS3.2.6 forbids for anything that calls the blocking block seam.";;
        require_fail) cat <<'EOF'
and the worker TU registers NO fork-context handler, so it cannot become fork-context code
EOF
                      ;;
        esac;;

    join-own-connect-not-suppressed)
        case "$_f" in
        facility)     echo "the sec 4(O.11) REJOIN topology in join_open_cm() (the drive must ride the VMS$VAXcluster connection the EXECUTIVE already holds for the pair -- book p. 7-23 -- instead of opening a redundant second one and moving its own op-0x02 onto it)";;
        targets)      echo "kernel-core/vms_cnxman_join_fsm.c";;
        suites_red)   echo "test_cnxman_join";;
        isolation)    echo "isolated";;
        why)          echo "join_open_cm()'s 'if (join_cm_take_held(j)) return;' -- the read of the target CSB's own cdt_conid before step 4 dials -- is disarmed, so this node opens its OWN VMS\$VAXcluster connect even when the executive already holds the pair's one. The glue binds cdt_conid the instant SCS mints an outbound Con.ID, so that second connect also re-binds the executive's record away from the live member-initiated CDT and the op-0x02 that starts admission never reaches it: exactly the sec 4(O.11) rejoin failure. The first-join arm is untouched (nothing is dialling an unknown system, so cdt_conid is 0 and both behaviours are identical).";;
        require_fail) cat <<'EOF'
this node opens NO VMS$VAXcluster connect of its own when the executive already holds the pair's one
... its whole outbound census is the MSCP$DISK disk-client leg
... and the suppression is COUNTED, once
... and said on the console
the drive runs on the MEMBER-INITIATED Con.ID
... and the executive's own record was never re-bound away from it
admission started
three cat-0x01 originations on the member's connection
MODEL first (sec 4(o) row 1)
... then PARAMS (row 2)
... then op-0x02, the request that starts admission, on the MEMBER-INITIATED connection and not on one of this node's own
EOF
                      ;;
        esac;;

    join-relay-unanswered)
        case "$_f" in
        facility)     echo "the sitting member's answer to the coordinator's cat-0x01 op-0x12 RELAY (the Rule of Total Connectivity, VAXcluster Principles p. 7-39; spec 4(O.31) -- the relay sits between op 0x02 and op 0x03 and is the commit gate for admitting a THIRD node)";;
        targets)      echo "kernel-core/vms_cnxman_join_fsm.c";;
        suites_red)   echo "test_cnxman_join";;
        isolation)    echo "isolated";;
        why)          echo "join_h_relay() still SEES the coordinator's relay and still counts it, but returns before building the answer, so the member emits nothing. That is the pre-vms-4f0 behaviour in one property: measured against a real OpenVMS VAX V7.3 an unanswered relay made the VAX log a third node's membership request ~30 times without ever proposing it, and then stop transmitting altogether. The response recipe, the allowlist row and the table cells are untouched -- only the answer.";;
        require_fail) cat <<'EOF'
EXACTLY ONE frame went back -- the answer the coordinator's whole admission gates on
every body byte from [4] up is what the REAL OpenVMS VAX member put on the wire
  body[9]: the opcode, echoed
  body[20:24]: the epoch, LE u32
body[20:24] is OUR epoch, not the coordinator's
EOF
                      ;;
        esac;;

    *)
        echo "host_defects.sh: unknown defect '$_d'" >&2
        return 1;;
    esac
}

# ---------------------------------------------------------------------------
# apply_edit <file> <defect>
#
# The mutation itself. One sed per defect, anchored to an exact source line.
# The replacement text carries a `/* NEGCTL <name> */` marker so a second
# apply against the same file finds no match -- cmd_apply's pristine-compare
# then reports BROKEN FIXTURE instead of silently re-applying nothing.
# ---------------------------------------------------------------------------
apply_edit() {
    _file="$1"; _d="$2"
    case "$_d" in
    coord-genesis-refusal-uncounted)
        # c->genesis_refused_noquorum++; is unique in this file (grep -c is
        # 1) -- no line anchor needed. The refusal three lines below is left
        # completely alone.
        sed -i 's|c->genesis_refused_noquorum++;|/* NEGCTL coord-genesis-refusal-uncounted: the refusal is not counted */|' "$_file";;

    codec-vc-zero-incarnation-not-refused)
        # `if (incarnation == 0u)` is unique in this file (the credit-return
        # gate a few functions down reads `c->incarnation`, a different
        # literal) -- disarmed, not deleted, so the parameter stays used.
        sed -i 's|if (incarnation == 0u)|if (0 \&\& incarnation == 0u) /* NEGCTL codec-vc-zero-incarnation-not-refused */|' "$_file";;

    codec-cm-short-body-not-refused)
        # `if (body_len != VMS_CM_BODY_LEN)` is unique in this file.
        sed -i 's|if (body_len != VMS_CM_BODY_LEN)|if (0 \&\& body_len != VMS_CM_BODY_LEN) /* NEGCTL codec-cm-short-body-not-refused */|' "$_file";;

    codec-blk-no-trailer-not-honest)
        # `if (frame_len <= inner_frame_len)` is unique in this file.
        sed -i 's|if (frame_len <= inner_frame_len)|if (0 \&\& frame_len <= inner_frame_len) /* NEGCTL codec-blk-no-trailer-not-honest */|' "$_file";;

    barrier-bit0-uncounted)
        # `b->bitmap_bit0++;` is unique in this file, and it is the
        # braceless body of an `if (...)`. An empty COMPOUND statement
        # (`{ }`), not a bare `;` -- this library builds -Wall -Wextra
        # -Werror, and a lone `;` after `if` trips -Werror=empty-body. `{ }`
        # is a legal, warning-free empty statement.
        sed -i 's|b->bitmap_bit0++;|{ } /* NEGCTL barrier-bit0-uncounted: the impossible bit is not counted */|' "$_file";;

    phase2-count-mismatch-uncounted)
        # `st->count_mismatch++;` occurs ONCE in this file (the other
        # counters phase2_commit_count() bumps -- bitmap_short,
        # m_above_grounded -- have their own, differently-named lines), so no
        # range anchor is needed: grep -c is 1.
        sed -i 's|st->count_mismatch++;|/* NEGCTL phase2-count-mismatch-uncounted: the disagreement is not counted */|' "$_file";;

    recnx-last-gasp-uncounted)
        # `r->last_gasps++;` is unique in this file.
        sed -i 's|r->last_gasps++;|/* NEGCTL recnx-last-gasp-uncounted: the last gasp is not counted */|' "$_file";;

    ldwv-refusal-uncounted)
        # `club->ldwv_build_refused++;` occurs TWICE in this file (the
        # survey-verdict refusal this defect targets, and ldwv_fill_club's
        # own VMS_LDWV_E_TOOBIG refusal a few lines later) -- IDENTICAL text,
        # so a plain sed would mutate both and trip two properties at once.
        # Range-anchored (facility_defects.sh's devtab-owner-not-recorded
        # precedent), NOT `0,/re/` first-match: the range starts at
        # `ldwv_survey_club(club, &s);` (unique, immediately precedes the
        # verdict call) and ends at the FIRST `return st;` after it, which is
        # the closing statement of the block this defect targets and stops
        # short of the second occurrence entirely. Also idempotency-safe: a
        # second apply finds the range's own `club->ldwv_build_refused++;`
        # already gone, so cmd_apply's pristine-compare reports BROKEN
        # FIXTURE rather than silently moving on to the second occurrence.
        sed -i '/^\tldwv_survey_club(club, &s);$/,/^\t\treturn st;$/ s|club->ldwv_build_refused++;|/* NEGCTL ldwv-refusal-uncounted: the refusal is not counted */|' "$_file";;

    dlm-lkid-guard-disabled)
        # dlm_lkid_pair_put()'s guard is split across two source lines
        # (`if (req_lkid == ... ||` then `master_lkid == ...)` on the next),
        # so absorbing the FIRST line's `||` into a `0 &&` disarms the WHOLE
        # two-line condition with a single-line sed -- the second line then
        # reads as `0 && master_lkid == VMS_DLM_LKID_UNSET)`, always false,
        # with no edit needed there at all. The sibling guard in
        # vms_dlm_enq_response_build_grant_valblk folds both lock ids onto
        # ONE line (`... || master_lkid == ...)` with no line break), so this
        # exact multi-token line (ending in `||` with nothing after) cannot
        # match it -- it is unique in the file.
        sed -i 's@^\tif (req_lkid == VMS_DLM_LKID_UNSET ||$@\tif (0 \&\& /* NEGCTL dlm-lkid-guard-disabled: the fc8540ae guard no longer refuses */@' "$_file";;

    dlm-requester-hash-refusal-uncounted)
        # `f->hash_unknown_refused++;` occurs TWICE in this file (this
        # to-directory route-check refusal, and the redirect-resolve path's
        # own copy) -- IDENTICAL text, so range-anchored (facility_defects.sh
        # devtab-owner-not-recorded precedent) to the unique guard line that
        # immediately precedes THIS occurrence. The range END is the
        # `return DLM_REQ_E_NOHASH;` two lines below the target (NOT the
        # target text itself, ldwv's own precedent): idempotency-safe,
        # because a second apply's range still resolves to this same narrow
        # window but finds no `f->hash_unknown_refused++;` left inside it to
        # replace.
        sed -i '/if (to_directory && !p->dir_hash_known) {/,/return DLM_REQ_E_NOHASH;/ s|f->hash_unknown_refused++;|/* NEGCTL dlm-requester-hash-refusal-uncounted: the refusal is not counted */|' "$_file";;

    codec-mscp-gus-tail2-invented)
        # The OBSERVED body[48:50] tail write is unique in this file (and the
        # $-anchored, whole-line pattern) -- appending an EXTRA statement on
        # the SAME line fabricates the "undecoded, never invented" half two
        # bytes further in, instead of leaving it at the zero the function's
        # own initial put_zero() left there. Idempotency-safe: the anchor is
        # itself consumed by the edit (the line no longer ends where the `$`
        # anchor requires), so a second apply matches nothing.
        sed -i 's@^\tvms_wire_put_le16(&w, VMS_OFF_MSCP_GUS_E_TAIL, VMS_MSCP_GUS_TAIL_OBSERVED);$@\tvms_wire_put_le16(\&w, VMS_OFF_MSCP_GUS_E_TAIL, VMS_MSCP_GUS_TAIL_OBSERVED); vms_wire_put_le16(\&w, VMS_OFF_MSCP_GUS_E_TAIL + 2, 0xBEEFu); /* NEGCTL codec-mscp-gus-tail2-invented: the undecoded tail half is invented, not left zero */@' "$_file";;

    mscp-cl-glue-device-name-leaked)
        # vms_mscp_cl.c is not host-linkable (exec_kbackend.h) -- its own
        # suite (test_mscp_cl.c) proves its properties by SOURCE-SCANNING the
        # shipping file, including the negative half "the glue spells NO
        # `$DUA` device name". Nothing here is compiled for the host suite,
        # so extending the file's own unique trailing #include line with a
        # trailing comment carrying the forbidden literal is the minimal
        # single-property injection. Idempotency-safe: the $-anchored,
        # whole-line pattern is consumed by the edit, so a second apply
        # matches nothing.
        sed -i 's@^#include "vms_mscp_cl.h"$@#include "vms_mscp_cl.h"  /* NEGCTL mscp-cl-glue-device-name-leaked: this file must never spell a $DUA device name */@' "$_file";;

    mscp-cl-conn-refusal-uncounted)
        # `c->connect_refusals++;` occurs THREE times in this file (the
        # ops/ops->connect==NULL branch, the ops->connect()-failed branch
        # this defect targets, and a third site in a different handler) --
        # range-anchored to the unique guard line immediately preceding the
        # SECOND occurrence. The range END is the `conn_goto(...)` line right
        # after the target (NOT the target text itself, ldwv's own
        # precedent): idempotency-safe, because a second apply's range still
        # resolves to this same narrow window but finds nothing left inside
        # it to replace.
        sed -i '/if (c->ops->connect(c->ops->ctx, p->sysid, &conid) != 0 ||/,/conn_goto(c, p, MSCP_CL_CONN_IDLE, now);/ s|c->connect_refusals++;|/* NEGCTL mscp-cl-conn-refusal-uncounted: the refusal is not counted */|' "$_file";;

    mscp-cl-fsm-unit-uncounted)
        # `f->units_found++;` is unique in this file.
        sed -i 's|f->units_found++;|/* NEGCTL mscp-cl-fsm-unit-uncounted: the unit is not counted */|' "$_file";;

    mscp-cl-io-empty-cell-uncounted)
        # `f->ignored_events++;` occurs TWICE in this file (the out-of-range
        # state/event guard, and the empty-table-cell branch this defect
        # targets) -- range-anchored to the unique `h == (cl_handler_t)0`
        # guard line immediately preceding the SECOND occurrence, so the
        # first (out-of-range) is untouched.
        sed -i '/if (h == (cl_handler_t)0) {/,/f->ignored_events++;/ s|f->ignored_events++;|/* NEGCTL mscp-cl-io-empty-cell-uncounted: the empty cell is not counted */|' "$_file";;

    mscp-srv-glue-end-message-leaked)
        # vms_mscp_srv.c is not host-linkable (exec_kbackend.h) -- its own
        # suite (test_mscp_srv.c) source-scans the shipping file, including
        # the negative half "the glue builds NO end message (`_end_build`)".
        # Extending the file's own unique trailing #include line with a
        # trailing comment carrying the forbidden literal is the minimal
        # single-property injection. Idempotency-safe: the $-anchored,
        # whole-line pattern is consumed by the edit, so a second apply
        # matches nothing.
        sed -i 's@^#include "vms_mscp_srv.h"$@#include "vms_mscp_srv.h"  /* NEGCTL mscp-srv-glue-end-message-leaked: this file must never call an _end_build composer */@' "$_file";;

    mscp-srv-fsm-writeprotect-uncounted)
        # `f->write_protect_refusals++;` is unique in this file.
        sed -i 's|f->write_protect_refusals++;|/* NEGCTL mscp-srv-fsm-writeprotect-uncounted: the refusal is not counted */|' "$_file";;

    mscp-srv-io-worker-registers-handler)
        # vms_mscp_srv_io.c is not host-linkable (exec_kbackend.h) -- its own
        # suite (test_mscp_srv.c) source-scans the shipping file, including
        # the negative half "the worker TU registers NO fork-context handler
        # (`cf_set_work_handler`/`cf_set_rx_handler`)". Extending the file's
        # own unique #include "vms_mscp_srv_fsm.h" line (ASCII-only, unlike
        # the neighboring exec_kbackend.h include's UTF-8 section-mark
        # comment) with a trailing comment carrying the forbidden literal is
        # the minimal single-property injection. Idempotency-safe: the
        # $-anchored, whole-line pattern is consumed by the edit, so a second
        # apply matches nothing.
        sed -i 's@^#include "vms_mscp_srv_fsm.h"  /\* MSCP_SRV_BLOCK_SIZE, enum mscp_srv_io_op    \*/$@#include "vms_mscp_srv_fsm.h"  /* MSCP_SRV_BLOCK_SIZE, enum mscp_srv_io_op    */  /* NEGCTL mscp-srv-io-worker-registers-handler: this file must never call cf_set_work_handler */@' "$_file";;

    join-own-connect-not-suppressed)
        # `if (join_cm_take_held(j))` is unique in this file (the function's
        # own definition a few lines above is `static int
        # join_cm_take_held(struct cnxman_join *j)`, a different literal).
        # Disarmed with `0 &&` rather than deleted, so the helper stays
        # referenced and the file still builds -Wall -Wextra -Werror.
        # Idempotency-safe: the edit consumes the pattern, so a second apply
        # matches nothing and cmd_apply reports BROKEN FIXTURE.
        sed -i 's|if (join_cm_take_held(j))|if (0 \&\& join_cm_take_held(j)) /* NEGCTL join-own-connect-not-suppressed: this node dials even when the executive already holds the connection */|' "$_file";;

    join-relay-unanswered)
        # `j->relays_seen++;` is unique in this file (grep -c is 1):
        # join_h_relay()'s own accounting line. An early return is INSERTED
        # after it, so the handler still runs, still counts, and still takes
        # its argument -- the file builds -Wall -Wextra -Werror and only the
        # ANSWER disappears. Idempotency-safe: the guard the edit inserts
        # carries the NEGCTL marker, and a second apply would insert a
        # second copy AFTER the first return, changing nothing -- which
        # cmd_apply's pristine-compare reports as BROKEN FIXTURE only if the
        # file is unchanged, so the marker is matched instead.
        if grep -q 'NEGCTL join-relay-unanswered' "$_file"; then
            return 0    # already injected: leave it byte-identical so
        fi              # cmd_apply's pristine-compare reports BROKEN FIXTURE
        sed -i 's|\tj->relays_seen++;|\tj->relays_seen++;\n\tif (j != (struct cnxman_join *)0)\n\t\treturn CNXMAN_JOIN_RX_CONSUMED; /* NEGCTL join-relay-unanswered: the member answers the coordinator nothing */|' "$_file";;

    *)
        echo "host_defects.sh: unknown defect '$_d'" >&2
        return 1;;
    esac
}

cmd_list() { echo "$DEFECTS"; }

cmd_field() {
    [ $# -eq 2 ] || { echo "usage: host_defects.sh field <defect> <field>" >&2; return 2; }
    defect_field "$1" "$2"
}

# ---------------------------------------------------------------------------
# cmd_apply <defect> <src-root>...
#
# Applies the defect to every copy of every target file that exists under the
# given src roots, and PROVES the edit landed by comparing each file against a
# pristine copy taken immediately before the edit. An anchor that no longer
# matches is a BROKEN FIXTURE: the build must fail loudly rather than produce
# an unmutated binary that then "proves" the gate caught nothing.
# (Same shape as tests/qemu/facility_defects.sh's cmd_apply.)
# ---------------------------------------------------------------------------
cmd_apply() {
    [ $# -ge 2 ] || { echo "usage: host_defects.sh apply <defect> <src-root>..." >&2; return 2; }
    _d="$1"; shift

    defect_field "$_d" targets >/dev/null || return 2
    _targets=$(defect_field "$_d" targets)

    command -v cmp >/dev/null 2>&1 || {
        echo "FATAL: cmp(1) unavailable -- cannot verify the injection landed" >&2
        return 3
    }

    _touched=0
    for _root in "$@"; do
        [ -d "$_root" ] || continue
        for _t in $_targets; do
            _f="$_root/$_t"
            [ -f "$_f" ] || continue
            cp "$_f" "$_f.negctl-pristine" || return 3
            apply_edit "$_f" "$_d" || return 3
            if cmp -s "$_f" "$_f.negctl-pristine"; then
                echo "FATAL: BROKEN FIXTURE (not a broken gate)." >&2
                echo "  defect '$_d' did not change $_f -- its sed anchor no longer matches." >&2
                echo "  The source moved; re-anchor the mutation in tests/cluster/host/host_defects.sh." >&2
                rm -f "$_f.negctl-pristine"
                return 3
            fi
            rm -f "$_f.negctl-pristine"
            echo "  injected '$_d' into $_f"
            _touched=$((_touched + 1))
        done
    done

    if [ "$_touched" -eq 0 ]; then
        echo "FATAL: BROKEN FIXTURE (not a broken gate)." >&2
        echo "  defect '$_d' names target(s) [$_targets] but none exist under: $*" >&2
        return 3
    fi
    return 0
}

# ---------------------------------------------------------------------------
# cmd_coverage (vms-181)
#
# "Every cluster-family TU this R1 host ladder OWNS (HOST_OWNED_UNITS,
# above) has a host-native negative control" -- the same translation-unit
# coverage idea as tests/qemu/facility_defects.sh's cmd_coverage section 1,
# but scoped to just the 31 TUs this ladder claims: THAT script floors
# "every executive facility"; this one only has to floor "the domain I said
# I owned". CAN-FAIL, hard-RED: HOST_OWNED_UNITS is compared against
# DEFECTS' own `targets` fields, not declared once and trusted, so adding an
# entry above with no matching defect turns this red immediately. See
# cmd_selftest's negative-control meta-check (INV-6) for the proof this can
# actually go red.
# ---------------------------------------------------------------------------
cmd_coverage() {
    _cov_all_targets=""
    for _cov_d in $DEFECTS; do
        _cov_all_targets="$_cov_all_targets $(defect_field "$_cov_d" targets)"
    done

    _cov_missing=""
    for _cov_u in $HOST_OWNED_UNITS; do
        case " $_cov_all_targets " in
            *" $_cov_u "*) ;;
            *) _cov_missing="$_cov_missing $_cov_u";;
        esac
    done

    if [ -n "$_cov_missing" ]; then
        echo "FAIL: HOST_OWNED_UNITS translation unit(s) with NO host-native negative control:$_cov_missing"
        return 1
    fi
    echo "PASS: every HOST_OWNED_UNITS translation unit is named by some defect's targets declaration"
    return 0
}

# ---------------------------------------------------------------------------
# cmd_selftest <repo-root>
#
# STATIC, like facility_defects.sh's own selftest: proves the sed anchor
# still matches the current tree (a real sed(1) run against a throwaway
# copy, then diffed) and that re-applying to the same copy is refused as a
# BROKEN FIXTURE rather than silently landing nothing. Does NOT compile or
# run anything -- that claim belongs to run_host_negctl.sh alone.
# ---------------------------------------------------------------------------
cmd_selftest() {
    [ $# -eq 1 ] || { echo "usage: host_defects.sh selftest <repo-root>" >&2; return 2; }
    _st_repo="$1"
    _st_root="$_st_repo/src"
    _st_tests="$_st_repo/tests/cluster/host"
    _st_tmp=$(mktemp -d) || return 2
    _st_rc=0

    for _st_d in $DEFECTS; do
        for _st_fld in facility targets suites_red isolation why require_fail; do
            if [ -z "$(defect_field "$_st_d" "$_st_fld")" ]; then
                echo "FAIL: $_st_d: metadata field '$_st_fld' is empty"
                _st_rc=1
            fi
        done

        rm -rf "$_st_tmp/tree"
        mkdir -p "$_st_tmp/tree"
        if ! cp -a "$_st_root/kernel-core" "$_st_tmp/tree/" 2>/dev/null; then
            echo "FAIL: cannot copy $_st_root/kernel-core for the self-test"
            rm -rf "$_st_tmp"
            return 2
        fi

        if cmd_apply "$_st_d" "$_st_tmp/tree" >/dev/null 2>&1; then
            echo "  ok: $_st_d injects into the current tree"
        else
            echo "FAIL: $_st_d: its sed anchor no longer matches the source tree."
            echo "      The mutation would inject NOTHING and run_host_negctl.sh would then"
            echo "      certify a defect that was never applied. Re-anchor it."
            _st_rc=1
            continue
        fi

        if _st_out=$(cmd_apply "$_st_d" "$_st_tmp/tree" 2>&1); then
            echo "FAIL: $_st_d: applying it TWICE succeeded. Either the mutation is repeatable"
            echo "      (so it is not the single minimal edit it claims to be) or the"
            echo "      injection-landed check never fires -- in which case a dead anchor"
            echo "      would be reported as a caught defect."
            _st_rc=1
        elif echo "$_st_out" | grep -qF 'BROKEN FIXTURE'; then
            echo "  ok: $_st_d: a no-op re-apply is reported as BROKEN FIXTURE (the check has teeth)"
        else
            echo "FAIL: $_st_d: re-apply failed, but not with BROKEN FIXTURE: $_st_out"
            _st_rc=1
        fi
    done

    rm -rf "$_st_tmp"

    # Every require_fail text has to EXIST literally in the suite source, or
    # the driver can never observe it and the manifest entry is a typo, not a
    # property. Loose normalisation (quotes/backslashes stripped, whitespace
    # collapsed) for the same reason facility_defects.sh's selftest does it:
    # this catches a typo, not a C parse.
    for _st_d in $DEFECTS; do
        for _st_suite_glob in $(defect_field "$_st_d" suites_red); do
            _st_src="$_st_tests/$_st_suite_glob.c"
            [ -f "$_st_src" ] || { echo "FAIL: $_st_d: suites_red names '$_st_suite_glob' but $_st_src does not exist"; _st_rc=1; continue; }
            _st_flat=$(tr -d '"\\' <"$_st_src" | tr '\n\t' '  ' | tr -s ' ')
            defect_field "$_st_d" require_fail | while IFS= read -r _st_txt; do
                [ -n "$_st_txt" ] || continue
                _st_needle=$(printf '%s' "$_st_txt" | tr -d '"\\' | tr -s ' ')
                case "$_st_flat" in
                *"$_st_needle"*) : ;;
                *) echo "FAIL: $_st_d: require_fail text absent from $_st_src: [$_st_txt]";;
                esac
            done
        done
    done

    # -----------------------------------------------------------------------
    # Negative control for cmd_coverage itself (vms-181, INV-6): a coverage
    # gate that has never been shown able to go red is exactly the
    # "tautology with printf calls" problem this file's own header opens
    # with. Run cmd_coverage's logic once with HOST_OWNED_UNITS augmented by
    # a path no defect could ever cover, and assert it (a) fails and
    # (b) names that exact path. Run inside a subshell so the augmented list
    # never leaks into the real HOST_OWNED_UNITS anything else here reads.
    # -----------------------------------------------------------------------
    _st_bogus="kernel-core/__negctl_bogus__.c"
    _st_cov_out=$( (HOST_OWNED_UNITS="$HOST_OWNED_UNITS
$_st_bogus"; cmd_coverage) 2>&1 )
    _st_cov_rc=$?
    if [ "$_st_cov_rc" -eq 0 ]; then
        echo "FAIL: cmd_coverage did not go red when HOST_OWNED_UNITS was augmented with a"
        echo "      path no defect could ever cover ($_st_bogus) -- the coverage check has no teeth."
        _st_rc=1
    elif ! printf '%s' "$_st_cov_out" | grep -qF "$_st_bogus"; then
        echo "FAIL: cmd_coverage went red under the augmented HOST_OWNED_UNITS, but its"
        echo "      output did not name $_st_bogus -- it failed for the wrong reason."
        _st_rc=1
    else
        echo "  ok: cmd_coverage's own negative control fires (a bogus owned unit reddens it, named)"
    fi

    if [ "$_st_rc" -eq 0 ]; then
        echo "PASS: every defect's sed mutation injects into the current tree (executed,"
        echo "      real sed + cmp against a throwaway copy), its injection-landed check"
        echo "      demonstrably fires on a no-op re-apply, every require_fail text"
        echo "      appears literally in its suite's source (a text search, not a run --"
        echo "      only run_host_negctl.sh actually builds and runs anything), and"
        echo "      cmd_coverage's own negative control (vms-181) is proven able to fire."
    fi
    return $_st_rc
}

case "${1:-}" in
    list)     shift; cmd_list "$@";;
    field)    shift; cmd_field "$@";;
    apply)    shift; cmd_apply "$@";;
    owned)    shift; cmd_owned "$@";;
    coverage) shift; cmd_coverage "$@";;
    selftest) shift; cmd_selftest "$@";;
    *)  echo "usage: host_defects.sh {list|field|apply|owned|coverage|selftest} ..." >&2; exit 2;;
esac
