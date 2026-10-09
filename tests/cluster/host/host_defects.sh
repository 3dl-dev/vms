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
#   dlm-dir-remove-by-anyone                 vms_dlm_dir.c   (rd vms-8219)
#   dlm-learner-unbounded                    vms_lock.c      (rd vms-4e9)
#   dlm-own-directory-not-consulted          vms_lock.c      (rd vms-025)
#   dlm-self-claim-answered-you-master       vms_dlm_dir.c   (rd vms-db2a)
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
# GROWN (vms-6d3d) with the COLD-FORMATION vote sum, the property that decides
# whether an executive may mint a cluster system id:
#
#   quorum-form-set-ignores-peers        vms_cnxman_quorum.c
#   makes cnxman_quorum_form_set() sum only the LOCAL system's votes, i.e.
#   restores the pre-vms-6d3d rule that a node may found only a cluster its
#   OWN votes already carry. Every refusal in the negctl suite still holds
#   (a lone or non-voting node still founds nothing); what goes is the
#   documented two-node VMScluster -- VOTES=1 / EXPECTED_VOTES=2 on both --
#   which two real OpenVMS VAX V7.3 systems do form
#   (tests/lab/captures/vms-6d3d-coldform-ev2-20260924/) and which no pair of
#   OVMX nodes could form under the old rule.
#
# GROWN (vms-1ac) with the four properties of an admission this node
# COORDINATES -- the defect class that bugchecked a real OpenVMS VAX V7.3:
#
#   coord-relay-epoch-advanced-early      vms_cnxman_coord_fsm.c
#   coord-membrec-epoch-zero              vms_cnxman_coord_fsm.c
#   coord-admission-not-selected-disarmed vms_cnxman_coord_fsm.c
#   coord-admission-open-gate-disarmed    vms_cnxman_coord_fsm.c
#
# GROWN (vms-f297) with the Phase 1 cells an OVMX coordinator now carries to a
# real VAX member, and the cluster facts behind them:
#
#   coord-open-cells-not-attached         vms_cnxman_coord_fsm.c
#   coord-open-withhold-disarmed          vms_cnxman_coord_fsm.c
#   club-open-facts-unlearned             vms_cnxman_barrier_fsm.c
#   removal-pair-not-rederived            vms_cnxman_phase2.c
#   join-swallows-step-reports            vms_cnxman_join_fsm.c
#   coord-step-ack-unmarked               vms_cluster_codec_cm.c
#   coord-open-skips-records-wait         vms_cnxman_coord_fsm.c
#   coord-ignores-rejection               vms_cnxman_coord_fsm.c
#   join-asks-before-telling-members      vms_cnxman_join_fsm.c
#   scs-accept-conndata-dropped           vms_scs_fsm.c
#   csb-accept-resume-disarmed            vms_cnxman_csb.c
#   csb-resume-reintroduces               vms_cnxman_csb.c
#
SELF="$0"

DEFECTS="coord-genesis-refusal-uncounted
coord-open-cells-not-attached
coord-open-withhold-disarmed
club-open-facts-unlearned
removal-pair-not-rederived
join-swallows-step-reports
coord-step-ack-unmarked
coord-open-skips-records-wait
coord-ignores-rejection
join-asks-before-telling-members
scs-accept-conndata-dropped
csb-accept-resume-disarmed
csb-resume-reintroduces
pe-receive-hold-disarmed
csb-abandoned-connect-keeps-conid
quorum-form-set-ignores-peers
codec-vc-zero-incarnation-not-refused
codec-cm-short-body-not-refused
codec-blk-no-trailer-not-honest
barrier-bit0-uncounted
phase2-count-mismatch-uncounted
recnx-last-gasp-uncounted
ldwv-refusal-uncounted
dlm-dir-remove-by-anyone
dlm-learner-unbounded
dlm-own-directory-not-consulted
dlm-self-claim-answered-you-master
dlm-lkid-guard-disabled
dlm-requester-hash-refusal-uncounted
dlm-hash-empty-name-not-refused
codec-mscp-gus-tail2-invented
mscp-cl-glue-device-name-leaked
mscp-cl-conn-refusal-uncounted
mscp-cl-fsm-unit-uncounted
mscp-cl-io-empty-cell-uncounted
mscp-srv-glue-end-message-leaked
mscp-srv-fsm-writeprotect-uncounted
mscp-srv-io-worker-registers-handler
join-own-connect-not-suppressed
join-relay-unanswered
coord-relay-epoch-advanced-early
coord-membrec-epoch-zero
coord-admission-not-selected-disarmed
coord-admission-open-gate-disarmed
coord-removal-open-gate-disarmed
pe-last-gasp-once-per-port
pe-late-frame-revives-channel
codec-conndata-ack-cell-dropped
csb-reconnect-never-carries
csb-resume-ignores-peer-position
pe-start-refusal-silent
pe-reformation-stacks-before-it-starts
pe-peer-start-keeps-dead-echo
codec-remove-nodemap-unread
csb-dead-found-by-sysid
pe-station-filter-disarmed
recnx-attempt-supersedes-in-flight
csb-dropped-spare-reads-as-loss
csb-continued-dialogue-not-followed
csb-conndata-count-ignored
join-asks-the-last-discovered
join-connectivity-gate-disarmed
join-unheard-gate-disarmed
join-same-breath-gate-disarmed
join-follow-csb-conn-disarmed
join-member-count-to-foreign
join-asks-a-system-in-no-cluster
join-does-not-reach-ours
coord-genesis-ignores-says-member
join-no-member-backoff-not-cut
join-drive-stays-on-a-joiner
csb-phase1-named-not-carried
barrier-phase1-not-marked
barrier-phase1-not-cleared
join-transition-loss-redriven
join-transition-reoffers-burst
glue-close-abandons-held-transition
csb-new-incarnation-carried
barrier-stalled-refuses-new-transition
barrier-stalled-ignores-new-go
join-reply-to-the-join-target"

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
kernel-core/vms_dlm_dir.c
kernel-core/vms_dlm_hash.c
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

    quorum-form-set-ignores-peers)
        case "$_f" in
        facility)     echo "cluster COLD-FORMATION quorum: p. 7-6 step 1's proposed set is this system PLUS the systems it can see, and the votes weighed against quorum are their COMBINED votes (rd vms-6d3d)";;
        targets)      echo "kernel-core/vms_cnxman_quorum.c";;
        suites_red)   echo "test_cnxman_genesis";;
        isolation)    echo "isolated";;
        why)          echo "cnxman_quorum_form_set() sums only the LOCAL CSB's votes, so a peer whose PARAMS really arrived over a really-open circuit contributes nothing -- the pre-vms-6d3d rule that a node may found only a cluster its OWN votes already carry. Every refusal still refuses; what stops forming is the documented two-node VMScluster (VOTES=1/EXPECTED_VOTES=2 on both), which two real OpenVMS VAX V7.3 systems do form.";;
        require_fail) cat <<'EOF'
VOTES=1/EV=2 + a seen VOTES=1/EV=2 peer: FOUNDS on 2 votes
a peer expecting 3 votes: quorum 2, and 2 votes meet it
VOTES=2 + a seen VOTES=1/EV=5 peer: 3 votes meet quorum 3
VOTES=1/EV=2 founds on the COMBINED two votes
... at generation 1, CSV slot 1
... and phase2 committed it a member
EXACTLY ONE of the two founds
... it stands down for the other candidate
... but now because that system takes precedence
... and it is named
... by its real sysid
EOF
                      ;;
        esac;;

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

    dlm-learner-unbounded)
        case "$_f" in
        facility)     echo "the DLM directory-hash learner's bound (dir_hash_learn_room(), rd vms-4e9): a learned hash gets a NEW resource block only while the table is under VMS_DLM_LEARN_RES_CAP; a declined learn is counted";;
        targets)      echo "kernel-core/vms_lock.c";;
        suites_red)   echo "test_lock_dir";;
        isolation)    echo "isolated";;
        why)          echo "the learner's bound is disarmed, so every distinct root name a VMS member puts on the wire leaves a preserved resource block behind for good -- the unbounded growth of a long-lived mixed cluster the bound exists to stop.";;
        require_fail) cat <<'EOF'
the learner keeps exactly its bound of new names
and counts every name it declined
EOF
                      ;;
        esac;;

    dlm-own-directory-not-consulted)
        case "$_f" in
        facility)     echo "the ENGINE's consultation of THIS NODE's own lock directory before it masters anything (dlm_route_own_directory(), rd vms-025): a name some other system already masters is routed THERE, never mastered a second time here";;
        targets)      echo "kernel-core/vms_lock.c";;
        suites_red)   echo "test_dlm_mixed_master";;
        isolation)    echo "isolated";;
        why)          echo "dlm_resolve_master()'s call to dlm_route_own_directory() is short-circuited, which is EXACTLY the measured origin/main behaviour: with a real VAX in the membership the all-OVMX gate makes dir_resolve() answer 'this node' for every name, so a resource a VAX already masters (its lookup answered and recorded in this node's own directory) is mastered HERE as well -- two masters for one resource, which is a lock manager agreeing that two systems may both grant EX on the same file.";;
        require_fail) cat <<'EOF'
*** and the resource's MASTER is the VAX -- this node did NOT master it a second time ***
  the engine does not claim mastery
exactly one frame left this node
*** addressed to the VAX the directory named ***
  as an op-0x01 request
*** carrying the hash the VAX ITSELF put on the wire for that name -- never a computed one ***
  and OUR OWN handle, the one this executive minted
*** and NOTHING is granted yet: the requester waits for the VAX's master, it does not grant itself ***
the VAX's grant reply is accepted by the shipping FSM
*** the $ENQ is REFUSED honestly: the VAX masters it and this node has no hash to address it with ***
  and it did NOT become a second master instead
  and the refusals to RECORD are counted, not hidden
*** and the mastery is RECORDED in this node's own directory -- which is what stops the next asker being told to master it ***
*** the directory answers THIS NODE MASTERS IT (p. 6-51), not 'you master it' ***
  nothing was answered 'you master it'
EOF
                      ;;
        esac;;

    dlm-self-claim-answered-you-master)
        case "$_f" in
        facility)     echo "the lock directory's NAME-ONLY self-claim (dir_self_claims(), rd vms-db2a): a VMS system's lookup for a name THIS NODE masters is answered 'this node masters it' (p. 6-51), never 'you master it'";;
        targets)      echo "kernel-core/vms_dlm_dir.c";;
        suites_red)   echo "test_dlm_dir";;
        isolation)    echo "isolated";;
        why)          echo "vms_dlm_dir_lookup()'s consultation of the name-only self-claim is disarmed. A real VAX's lookup carries ITS group, ITS access mode and ITS hash -- none of which this executive holds for a name it mastered first -- so it finds no exact-identity entry and is answered 'you master it': the VAX becomes a second master of a resource this node already masters.";;
        require_fail) cat <<'EOF'
*** the VAX's lookup is answered THIS NODE MASTERS IT (p. 6-51), not 'you master it' ***
  nothing was answered 'you master it'
  and the outcome is counted
another resource domain of that name is answered the same way (over-serialize, never two masters)
EOF
                      ;;
        esac;;

    dlm-dir-remove-by-anyone)
        case "$_f" in
        facility)     echo "the lock directory's entry removal (vms_dlm_dir_remove(), rd vms-8219): only the system the entry names as master may remove it -- a removal from any other system is refused and counted";;
        targets)      echo "kernel-core/vms_dlm_dir.c";;
        suites_red)   echo "test_dlm_dir";;
        isolation)    echo "isolated";;
        why)          echo "vms_dlm_dir_remove()'s master check is disarmed, so a VMS system's op-0x04 removal deletes ANOTHER system's directory entry; the next lookup is answered 'you master it' and the resource has two masters -- the data-integrity failure the directory exists to prevent.";;
        require_fail) cat <<'EOF'
a removal from a system that is not the master removes nothing
the master's removal does
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
  master_lkid==0 REFUSED on BLKAST (0x05)
  req_lkid==0 REFUSED on BLKAST (0x05)
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

    dlm-hash-empty-name-not-refused)
        case "$_f" in
        facility)     echo "vms_dlm_name_hash()'s honest refusal of a name the wire cannot carry (rd vms-66fe: a value nobody can compute is never invented, INV-6)";;
        targets)      echo "kernel-core/vms_dlm_hash.c";;
        suites_red)   echo "test_dlm_hash";;
        isolation)    echo "isolated";;
        why)          echo "the 'name_len == 0u' half of vms_dlm_name_hash()'s range check is dropped, so a ZERO-LENGTH resource name is hashed -- the identity block's own length byte says 1..31 -- and *out is written for an identity the caller never had. The hash itself is untouched: the 1216 captured-value rows stay green, which is the point. A hash that answers for an empty name is exactly the shape of defect that puts a confident number on the wire for a resource that does not exist, and the directory node then masters it to the sender (memory cluster-promotion-gap).";;
        require_fail) cat <<'EOF'
a zero-length name is refused
a refusal writes nothing (INV-6)
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
and nothing at all was put on a connection of ours: there is not one
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
the relay is answered, on the member's new connection
EOF
                      ;;
        esac;;

    join-reply-to-the-join-target)
        case "$_f" in
        facility)     echo "WHERE a VMS\$VAXcluster 0x81 response goes (spec 4(p): \"the dialogue rides ONE VC per peer -- answer on whichever the request arrived on\"; rd vms-e8b)";;
        targets)      echo "kernel-core/vms_cnxman_join_fsm.c";;
        suites_red)   echo "test_cnxman_join";;
        isolation)    echo "isolated";;
        why)          echo "join_emit_reply() transmits through the join's own jops->send_msg on j->cm_conid instead of ops->respond -- i.e. every answer leaves on the connection to the member THIS JOIN DRIVES THROUGH, whoever asked. That is the shipped behaviour before rd vms-e8b, and it is exactly what put a real VAX1's class-0x04 departure-commit response onto the OVMX<->VAX2 connection: VAX2 took a fatal CNXMGRERR 193 us later, twice out of two attempts (tests/lab/captures/vms-e8b-cnxmgrerr-removenode-20261008/). The envelope is still stamped from the arrival CSB, so this is a single-factor mutation of the DESTINATION alone. The same mutation reopens stall-rig arm DX-1 (rd vms-f297): a member whose connection closed during its admission and came back as a NEW Con.ID answers on the dead handle the join is still holding, is refused (no-open-vc), and the real VAX coordinator never proposes the next joiner.";;
        require_fail) cat <<'EOF'
nothing goes out on the connection this join still holds ...
... the answer rides the connection it ARRIVED on (rd vms-e8b)
... stamped out of THAT connection's dialogue, at 1
... and the first message on it carries send-msg# 1
the answer goes to the member that ASKED
and NOT to the member this join drives through -- the frame that bugchecked VAX2 CNXMGRERR
... one 132-byte body, on that connection
... stamped out of THAT connection's dialogue
... acking what that peer really sent on it
the relay is answered, on the member's new connection
EOF
                      ;;
        esac;;

    coord-relay-epoch-advanced-early)
        case "$_f" in
        facility)     echo "WHEN the cluster epoch advances during an admission this node coordinates (spec 4(r) + rd vms-1ac: the op-0x12 RELAY carries the epoch the cluster IS at, the op-0x03 COMMIT one past it -- 133/133 real relay/commit pairs)";;
        targets)      echo "kernel-core/vms_cnxman_coord_fsm.c";;
        suites_red)   echo "test_cnxman_coord";;
        isolation)    echo "isolated";;
        why)          echo "coord_claim_club() advances the epoch again, as it did before rd vms-1ac, so the op-0x12 RELAY goes out at N+1 instead of N. Everything downstream still reads N+1 and still agrees with itself -- which is exactly why this went unnoticed against OVMX peers and only showed up when a real OpenVMS VAX answered the relay with ITS OWN epoch N and every answer was counted as an epoch_mismatch.";;
        require_fail) cat <<'EOF'
op-0x12 RELAY carries the epoch the cluster is at
...and the CLUB has not moved yet either
EOF
                      ;;
        esac;;

    coord-membrec-epoch-zero)
        case "$_f" in
        facility)     echo "the epoch field of every op-0x05 MEMBERSHIP RECORD this node originates (rd vms-1ac; 791/791 real records carry their transition's epoch, and cm-membrec-oracle.spec is one of them)";;
        targets)      echo "kernel-core/vms_cnxman_coord_fsm.c";;
        suites_red)   echo "test_cnxman_coord";;
        isolation)    echo "isolated";;
        why)          echo "coord_send_membrec() stops filling rec.epoch, so every membership record goes out with body[12:16] zero -- the shipped behaviour before rd vms-1ac. The records are otherwise complete and correct, which is the point: a receiver cannot place them in any transition, and a real OpenVMS VAX V7.3 handed them took a fatal CNXMGRERR.";;
        require_fail) cat <<'EOF'
EVERY op-0x05 record carries the transition's epoch -- the field OVMX used to leave at zero
EOF
                      ;;
        esac;;

    coord-admission-not-selected-disarmed)
        case "$_f" in
        facility)     echo "the receiver half of coordinator selection (spec 4(p): 'a NON-COORDINATOR peer SILENTLY DISCARDS op 0x02'), implemented as cnxman_coord_select()'s outranked-by-a-member gate";;
        targets)      echo "kernel-core/vms_cnxman_coord_fsm.c";;
        suites_red)   echo "test_cnxman_coord";;
        isolation)    echo "isolated";;
        why)          echo "cnxman_coord_select()'s coord_outranked_for_admission() call is disarmed with '0 &&', so this node takes EVERY op-0x02 it is handed and appoints itself transition coordinator even when another member outranks it. That is the shipped behaviour before rd vms-1ac, and on the lab rig it is how a sitting OVMX member came to drive a transition at a real OpenVMS VAX V7.3 that the VAX bugchecked on.";;
        require_fail) cat <<'EOF'
NOT the selected coordinator: no relay
...and not one frame of any kind: the op-0x02 is DISCARDED, which is what a real non-coordinator does
the discard is COUNTED
...and named
SILENTLY: not one console line, because a joiner that retries would otherwise print one per retry
and no transition was opened
EOF
                      ;;
        esac;;

    pe-receive-hold-disarmed)
        case "$_f" in
        facility)     echo "the VC receive hold (rd vms-ec2): a sequenced frame that arrives ahead of the hole, inside the receive credit this circuit granted, is kept and delivered in order once the hole fills -- as a real V7.3 port does";;
        targets)      echo "kernel-core/vms_pe_fsm.c";;
        suites_red)   echo "test_pe_vc";;
        isolation)    echo "isolated";;
        why)          echo "vc_hold() refuses every frame, so OVMX is back to the window-1 receiver that discards a reordered frame and makes a real VAX wait ~3 s to re-send it -- 44 of 94 back-to-back pairs on the stall rig's jittered tap (tests/lab/captures/vms-ec2-vc-reorder-20261005).";;
        require_fail) cat <<'EOF'
...and KEPT, inside the window it granted
6 filled the hole: OPEN circuit acknowledges 7 (highest contiguous)
6 filled the hole: and the WIRE carries 7
6 and the held 7 were both delivered
the held one through the hold
4 and 3 are KEPT
2 fills it: the frontier is 4
*** one cumulative ack of the highest -- 4, like the real VAX3's 17 ***
2, 3, 4 delivered, each exactly once
two of them from the hold
a frame beyond the receive credit this circuit granted is not held
and moves nothing
EOF
                      ;;
        esac;;

    csb-abandoned-connect-keeps-conid)
        case "$_f" in
        facility)     echo "h_connect_abandoned() (rd vms-04b): an initial connect that dies returns the block to NEW holding no Con.ID, so the joiner dials it again";;
        targets)      echo "kernel-core/vms_cnxman_csb.c";;
        suites_red)   echo "test_cnxman_csb";;
        isolation)    echo "isolated";;
        why)          echo "the block keeps the dead Con.ID at NEW, join_reach_ours() never dials it again, and a joiner stalled mid-connect waits forever for connectivity to a member -- stall-rig arms K-5/K-11.";;
        require_fail) cat <<'EOF'
*** and it claims NO connection, so it is dialled again ***
EOF
                      ;;
        esac;;

    pe-reformation-stacks-before-it-starts)
        case "$_f" in
        facility)     echo "the ORDER of a re-formation's two 0x41 frames (rd vms-18a: the oracle's four frames are VAX1 START, VAX2 START, VAX1 STACK, VAX2 STACK, and a START sent AFTER the STACK is discarded)";;
        targets)      echo "kernel-core/vms_pe_fsm.c";;
        suites_red)   echo "test_pe_vc";;
        isolation)    echo "isolated";;
        why)          echo "h_vc_rx_start()'s own-START is disarmed, so a re-forming circuit whose channel was ALREADY back answers the peer's START with a STACK and nothing else -- h_vc_own_start() cannot help, because the CHANNEL_UP it needs was spent before the START arrived. MEASURED consequence, rig arm D-5: the real OpenVMS VAX V7.3 discarded the late START, re-STARTed every 5 s for the rest of the run, and 20 s later each side removed the other. The FIRST-formation case is untouched (the same verifies>1 guard).";;
        require_fail) cat <<'EOF'
...and, because this is a RE-formation, answering it ALSO starts one from this side -- the frame the real VAX waits for and never got
and it answered with TWO 0x41 frames, not one
EOF
        ;;
        esac;;

    pe-peer-start-keeps-dead-echo)
        case "$_f" in
        facility)     echo "the SS4(i).B echo a formation the PEER starts stamps (rd vms-1f40: taken only at CHANNEL_UP, a re-formation whose START beat the b4 stamped the dead generation's number)";;
        targets)      echo "kernel-core/vms_pe_fsm.c";;
        suites_red)   echo "test_pe_vc";;
        isolation)    echo "isolated";;
        why)          echo "h_vc_rx_start() no longer takes the echo from its channel, so a circuit re-formed by the PEER'S START keeps stamping the number the previous generation was formed with. MEASURED consequence, rig arm M2-19: the real OpenVMS VAX V7.3 advertised 2, its START beat its b4 to the circuit, this node answered START+STACK stamped 1, and the VAX discarded both and re-STARTed every 5 s until each side removed the other. CHANNEL_UP's own take is untouched, which is why the b4-first order (arm M2-7) still passes.";;
        require_fail) cat <<'EOF'
every 0x41 in answer carries 2, the number the peer advertises now -- never the dead generation's 1 the real VAX discarded for 20 s
and the circuit keeps stamping 2 on everything after
EOF
        ;;
        esac;;

    recnx-attempt-supersedes-in-flight)
        case "$_f" in
        facility)     echo "one reconnect attempt in flight per CSB (rd vms-1f40: the once-a-second beat superseded an attempt the peer was still answering)";;
        targets)      echo "kernel-core/vms_cnxman_recnx_fsm.c";;
        suites_red)   echo "test_cnxman_recnx";;
        isolation)    echo "isolated";;
        why)          echo "recnx_tick_one()'s in-flight guard is disarmed, so the beat issues a new VMS\$VAXcluster CONNECT_REQ every second whether or not the previous one is still being answered. MEASURED consequence, rig arm P-9: a node woken from a 20 s stall answered ~1 s late, every ACCEPT arrived after the block had been re-bound to a newer attempt, six CONNECT_REQs went out, the peer accepted five, the block owned none, and the window ran out with a member removed. Attempts that ENDED (failed, no path) still re-fire once a second -- only the held beat goes.";;
        require_fail) cat <<'EOF'
t=2000: NO second CONNECT over the one being answered
t=3000: still none
one attempt, not three
and the two held beats are COUNTED, not silent
EOF
        ;;
        esac;;

    join-asks-the-last-discovered)
        case "$_f" in
        facility)     echo "the joiner's rank of whom to ask (rd vms-e88: trios A and A2 -- a real V7.3 joiner asks the highest-SCSSYSTEMID member, not the one it discovered last)";;
        targets)      echo "kernel-core/vms_cnxman_join_fsm.c";;
        suites_red)   echo "test_cnxman_join";;
        isolation)    echo "isolated";;
        why)          echo "join_outranks() answers yes to every later candidate, so the member asked is the last askable block in CLUB (discovery) order -- the p. 7-38 queue-tail rule this code ran before rd vms-e88. MEASURED consequence, rig arm S-1: the joining OVMX node discovered the other OVMX node 2 s before the real VAX, asked it -- an outranked member that discards the request by design -- and was never admitted.";;
        require_fail) cat <<'EOF'
trio A/A2 order: the higher member is asked though the lower was discovered last (nearest the CLUB tail)
nobody has spoken: the drive starts toward the highest
a connected system that has said nothing moves nothing
its PARAMS says it is a member: the drive moves to it
... on that system
EOF
        ;;
        esac;;

    join-connectivity-gate-disarmed)
        case "$_f" in
        facility)     echo "the joiner's p. 7-37 connectivity gate (rd vms-e88: trio C3 -- a real V7.3 joiner that could reach one of two members asked nobody for five minutes)";;
        targets)      echo "kernel-core/vms_cnxman_join_fsm.c";;
        suites_red)   echo "test_cnxman_join";;
        isolation)    echo "isolated";;
        why)          echo "join_admission_held() no longer holds a request while the members advertise more members than this node has connectivity with, so the join asks the one member it can see. MEASURED consequence, rig arm S-1: that member was the outranked OVMX node, which discards the request by design (rd vms-1ac), and the joiner was never admitted.";;
        require_fail) cat <<'EOF'
... a RETARGET, not a decline: nobody had been asked
... the third is op-0x02
MODEL, PARAMS, then the request, on ITS connection
and VMS's own console line names whom it asked
and said on the console
held for CONNECTIVITY
no op-0x02 while one of two members is out of reach -- thirty beats, as the real joiner waited five minutes
nobody was asked, so nobody was declined
the join moves to the higher member once it is in reach
... that system
and not again while that connect stands
held: the member counts two, this node reaches one
so it dials the OVMX system nobody had connected, once
EOF
        ;;
        esac;;

    join-unheard-gate-disarmed)
        case "$_f" in
        facility)     echo "no membership request before the member's own PARAMS (rd vms-e88: every real V7.3 joiner's op-0x02 followed the member's op-0x01)";;
        targets)      echo "kernel-core/vms_cnxman_join_fsm.c";;
        suites_red)   echo "test_cnxman_join";;
        isolation)    echo "isolated";;
        why)          echo "join_admission_held() no longer waits for the member being asked to send its own PARAMS; a member that has said nothing is treated as one that said 'no cluster'. MEASURED consequence, rig arm T-15: the join acted on the real VAX before the VAX had said a word on that connection, and the VAX never answered the request.";;
        require_fail) cat <<'EOF'
... held as UNHEARD
a member that has sent no PARAMS is not asked
and said on the console
and still nothing was asked of anybody
held as UNHEARD
the silence window later it is given up on, like an unanswered member
... and the request acks exactly the one message it then really sent there
... but not yet a request: it has not said what it is (rd vms-e88)
the new member hears this node's identity on ITS OWN connection at once
EOF
        ;;
        esac;;

    join-same-breath-gate-disarmed)
        case "$_f" in
        facility)     echo "op-0x02 is never in the identity burst (spec sec 4(o): sending 0x02 inside the initial burst leaves the peer silent; every real V7.3 joiner's request came >= 0.7 s after its own PARAMS)";;
        targets)      echo "kernel-core/vms_cnxman_join_fsm.c";;
        suites_red)   echo "test_cnxman_join";;
        isolation)    echo "isolated";;
        why)          echo "join_admission_held() no longer waits one beat after this node's own MODEL/PARAMS, so MODEL, PARAMS and CONFIG leave in one burst. MEASURED consequence, rig arm T-15: exactly that three-frame burst went to the real VAX and the VAX never acknowledged the request.";;
        require_fail) cat <<'EOF'
... and the re-offer counter does not move
... but NOT op-0x02 in the same breath (rd vms-e88; spec sec 4(o): 0x02 inside the identity burst leaves the peer silent)
... re-offered on that beat, once
MODEL and PARAMS, and not op-0x02 with them
and the identity records reached SCS this time
held as the same breath
op 0x02 is due, but never in the same breath as the identity records it follows (rd vms-e88)
EOF
        ;;
        esac;;

    join-follow-csb-conn-disarmed)
        case "$_f" in
        facility)     echo "the request rides the connection the executive records for the member (rd vms-e88: rig arm T-15 -- two crossed connects, the CSB kept the one the real VAX keeps, the join held the other)";;
        targets)      echo "kernel-core/vms_cnxman_join_fsm.c";;
        suites_red)   echo "test_cnxman_join";;
        isolation)    echo "isolated";;
        why)          echo "join_admission_held() no longer moves the join onto the CSB's own Con.ID, so after a crossing the join keeps the connection the real VAX abandons; its E77 gate then refuses to stamp there and no request reaches any connection.";;
        require_fail) cat <<'EOF'
E77: the kept connection's dialogue opens at 1
on the kept connection, after this node's identity there
one request in the world
the join moves to the connection the executive records
EOF
        ;;
        esac;;

    join-member-count-to-foreign)
        case "$_f" in
        facility)     echo "a member's count goes only to a peer running this implementation (rd vms-e88; the rd vms-1ac rule -- member-only bytes OVMX does not ground are symmetric between two OVMX nodes and a bugcheck risk in front of a VAX)";;
        targets)      echo "kernel-core/vms_cnxman_join_fsm.c";;
        suites_red)   echo "test_cnxman_join";;
        isolation)    echo "isolated";;
        why)          echo "join_members_for() drops the peer_is_ours test, so a foreign connection manager is sent a member-form count beside the zero member-only fields OVMX cannot ground: a record in a shape no real node writes.";;
        require_fail) cat <<'EOF'
a foreign connection manager still hears 0
and nothing new goes to the foreign one
EOF
        ;;
        esac;;

    join-asks-a-system-in-no-cluster)
        case "$_f" in
        facility)     echo "no membership request to a system whose own PARAMS say it belongs to no cluster (rd vms-e88: trio B -- a real V7.3 joiner never asked a system that was itself still joining)";;
        targets)      echo "kernel-core/vms_cnxman_join_fsm.c";;
        suites_red)   echo "test_cnxman_join";;
        isolation)    echo "isolated";;
        why)          echo "join_admission_held() asks the member it is driving through even when that member's PARAMS carried a member count of 0 -- a system that is joining too and cannot admit anybody. MEASURED consequence, rig arm E2-1 (the first build of this item): the joining OVMX node's first request went to the other OVMX node while it was still being admitted, went unanswered for five seconds, and admission waited for the re-issue.";;
        require_fail) cat <<'EOF'
... and not one request went out
... as an exhausted round, the fact the founding election reads
... said on the console
every system in sight says no cluster: the attempt ends with nobody asked
no request to a system that is itself joining, while another in sight has not yet said what it is
once the other says it is a member, the request goes to it
held as NO_MEMBER
and with only a joining system left, the round ends
the join is back in IDLE to ask again, not parked
while nobody is a member, the start waits out RECNXINTERVAL
... counted as a back-off cut short
a member that stayed silent is waited out in full (E80's rate bound), though it says it is a member
the joining system is never asked
EOF
        ;;
        esac;;

    join-does-not-reach-ours)
        case "$_f" in
        facility)     echo "the joiner dials the systems of its own implementation nobody has connected (rd vms-e88: the Rule of Total Connectivity, Davis p. 7-39; trio C3 -- a real joiner dials each member it discovers)";;
        targets)      echo "kernel-core/vms_cnxman_join_fsm.c";;
        suites_red)   echo "test_cnxman_join";;
        isolation)    echo "isolated";;
        why)          echo "join_hold() no longer dials, so a join held for connectivity waits for a connection nobody will make: an OVMX member dials nobody its own join is not driving through, and the joiner is driving through the real VAX. MEASURED consequence, stall-rig arm M-8 of this item: OVMXB held 'waiting for connectivity to every cluster member' for the whole run, the VAX counting two members and OVMXB reaching one.";;
        require_fail) cat <<'EOF'
... that system
and not again while that connect stands
so it dials the OVMX system nobody had connected, once
EOF
        ;;
        esac;;

    coord-genesis-ignores-says-member)
        case "$_f" in
        facility)     echo "a system whose own PARAMS say it belongs to a cluster bars founding (rd vms-e88; the peer-in-cluster clause of the founding election, SS8b (1))";;
        targets)      echo "kernel-core/vms_cnxman_coord_fsm.c";;
        suites_red)   echo "test_cnxman_genesis_negctl";;
        isolation)    echo "isolated";;
        why)          echo "coord_peer_in_cluster() no longer reads what the system advertised in its op-0x01 PARAMS, so only a CSID this node learned or a MEMBER flag bars founding. MEASURED consequence, stall-rig arm N-3 of this item: a voting OVMX node beside the real VAX -- which had said it was a member -- founded a cluster of its own with the other OVMX node, and the real VAX admitted that node into a second cluster.";;
        require_fail) cat <<'EOF'
... because that system belongs to a cluster
EOF
        ;;
        esac;;

    join-no-member-backoff-not-cut)
        case "$_f" in
        facility)     echo "a joiner whose last round found no member asks the first member that appears, not RECNXINTERVAL later (rd vms-e88)";;
        targets)      echo "kernel-core/vms_cnxman_join_fsm.c";;
        suites_red)   echo "test_cnxman_join";;
        isolation)    echo "isolated";;
        why)          echo "join_backoff_pending() no longer ends a no-member back-off when a system says it is a member, so the joiner sits out the whole RECNXINTERVAL. MEASURED consequence, stall-rig arm N-3 of this item: the joiner found only the other joining OVMX node, backed off 20 s, and while it waited that node founded a cluster of its own.";;
        require_fail) cat <<'EOF'
a member appearing: the next start runs, a second later, not twenty
... counted as a back-off cut short
EOF
        ;;
        esac;;

    join-drive-stays-on-a-joiner)
        case "$_f" in
        facility)     echo "a join driving toward a system that is not a member moves, before ADMIT, to a connected system that says it is one (rd vms-e88)";;
        targets)      echo "kernel-core/vms_cnxman_join_fsm.c";;
        suites_red)   echo "test_cnxman_join";;
        isolation)    echo "isolated";;
        why)          echo "join_drive_to_member() never moves the drive, so a join that started toward another joining system waits in VC CONNECT for a connect that system never accepts. MEASURED consequence, stall-rig arm P-2 of this item: the joiner drove toward the other OVMX node (the highest SCSSYSTEMID at CLUSTER_START) while it was being admitted, and sat in VC CONNECT for the rest of the run beside a real VAX that had dialled it and said it was a member.";;
        require_fail) cat <<'EOF'
its PARAMS says it is a member: the drive moves to it
... that system
... straight to ADMIT, where every hold still applies
the request goes to the member, on its own connection
... after this node's own identity on that connection
once, and the joiner never got one
EOF
        ;;
        esac;;

    csb-phase1-named-not-carried)
        case "$_f" in
        facility)     echo "a system named in a transition this node answered at Phase 1 re-establishes with its dialogue carried (rd vms-eb3)";;
        targets)      echo "kernel-core/vms_cnxman_csb.c";;
        suites_red)   echo "test_cnxman_csb";;
        isolation)    echo "isolated";;
        why)          echo "csb_dialogue_may_continue() ignores cm_phase1_named, so before Phase 2 has set SELECTED a lost connection to the coordinator restarts its dialogue at 1/0. MEASURED consequence, stall-rig arm P-3: the joiner, frozen between its Phase-1 answer and the GO, re-sent MODEL/PARAMS at send-msg# 1 on the connection the real VAX was re-establishing as a member's, and the VAX bugchecked CNXMGRERR.";;
        require_fail) cat <<'EOF'
and the send side
counted as CARRIED
not SELECTED yet, but in the answered transition: the re-established connection carries the ack (F5: 266)
EOF
        ;;
        esac;;

    barrier-phase1-not-marked)
        case "$_f" in
        facility)     echo "the participant's barrier names the transition's systems on their CSBs at Phase 1 (rd vms-eb3)";;
        targets)      echo "kernel-core/vms_cnxman_barrier_fsm.c";;
        suites_red)   echo "test_cnxman_barrier";;
        isolation)    echo "isolated";;
        why)          echo "barrier_h_open() no longer records the coordinator and the nodemap's systems on their CSBs, so nothing entitles a connection lost before the GO to carry its dialogue -- the P-3 CNXMGRERR shape.";;
        require_fail) cat <<'EOF'
and the member the nodemap names
it stands through the GO and the barrier
the coordinator's block is named at Phase 1
and the new transition names who is in it
EOF
        ;;
        esac;;

    barrier-phase1-not-cleared)
        case "$_f" in
        facility)     echo "the Phase-1 record ends with the transition (rd vms-eb3)";;
        targets)      echo "kernel-core/vms_cnxman_barrier_fsm.c";;
        suites_red)   echo "test_cnxman_barrier";;
        isolation)    echo "isolated";;
        why)          echo "barrier_phase1_clear() clears nothing, so a system stays entitled to carry its dialogue after the transition that named it has ended -- a later, unrelated loss would carry a dialogue E77 says must restart.";;
        require_fail) cat <<'EOF'
an abandoned transition un-names too
and ends with the transition
and so does one whose coordinator was lost for good
EOF
        ;;
        esac;;

    join-transition-loss-redriven)
        case "$_f" in
        facility)     echo "a connection lost inside a transition this node answered is held, not re-driven (rd vms-eb3)";;
        targets)      echo "kernel-core/vms_cnxman_join_fsm.c";;
        suites_red)   echo "test_cnxman_join";;
        isolation)    echo "isolated";;
        why)          echo "join_cm_connection_gone() no longer holds an ADMIT join whose transition is open, so the loss sends it to VC CONNECT and it re-drives MODEL/PARAMS and a second request. MEASURED consequence, stall-rig arm P-3: the real VAX, re-establishing the joiner as a member, bugchecked CNXMGRERR on the fresh identity burst.";;
        require_fail) cat <<'EOF'
... and said on the console
... it is held, counted
no MODEL, no PARAMS and no second request on the re-established connection (F5/F6: none)
once the transition is abandoned, the ADMIT beat takes the not-yet-admitted path
the loss does NOT send the join back to VC CONNECT
EOF
        ;;
        esac;;

    join-transition-reoffers-burst)
        case "$_f" in
        facility)     echo "nothing is re-offered on a connection re-established inside an answered transition (rd vms-eb3)";;
        targets)      echo "kernel-core/vms_cnxman_join_fsm.c";;
        suites_red)   echo "test_cnxman_join";;
        isolation)    echo "isolated";;
        why)          echo "join_reoffer_burst() re-offers the identity burst and the membership request on a re-established connection while the transition runs, because the burst mask is per-Con.ID. A real V7.3 joiner re-offered nothing there (oracle F5/F6).";;
        require_fail) cat <<'EOF'
no MODEL, no PARAMS and no second request on the re-established connection (F5/F6: none)
with exactly the one membership request it made
EOF
        ;;
        esac;;

    glue-close-abandons-held-transition)
        case "$_f" in
        facility)     echo "a path-loss close does not abandon the participant's transition; the end of the coordinator's window does (rd vms-eb3)";;
        targets)      echo "kernel-core/vms_cnxman.c";;
        suites_red)   echo "test_cnxman_glue";;
        isolation)    echo "isolated";;
        why)          echo "cnxman_transition_peer_lost() abandons the participant's barrier on every close again, so a joiner that loses its coordinator's connection across the GO drops the transition the coordinator is about to re-send -- the P-3 CNXMGRERR shape.";;
        require_fail) cat <<'EOF'
vms-c06: the participant half fires only for the block the barrier is taking its transition FROM
EOF
        ;;
        esac;;

    csb-new-incarnation-carried)
        case "$_f" in
        facility)     echo "a system back as a new incarnation starts a new conversation (p. 7-24/7-25; rd vms-eb3)";;
        targets)      echo "kernel-core/vms_cnxman_csb.c";;
        suites_red)   echo "test_cnxman_csb";;
        isolation)    echo "isolated";;
        why)          echo "csb_dialogue_may_continue() ignores cm_new_incarnation, so a member re-establishes a re-incarnated system's connection as the old conversation, with the 'already introduced' mask. MEASURED consequence, stall-rig arm F-13: the joiner, removed after a 30 s stall and back after CLUEXIT, never heard the OVMX member's PARAMS and waited for connectivity for the rest of the run.";;
        require_fail) cat <<'EOF'
a NEW incarnation is a new conversation: 1/0 (p. 7-25)
and nothing is recorded as already said to it -- this node introduces itself again
the fresh bind ends the record
EOF
        ;;
        esac;;

    barrier-stalled-refuses-new-transition)
        case "$_f" in
        facility)     echo "a new transition's open supersedes a committed, stalled barrier (rd vms-eb3, oracle F7)";;
        targets)      echo "kernel-core/vms_cnxman_barrier_fsm.c";;
        suites_red)   echo "test_cnxman_barrier";;
        isolation)    echo "isolated";;
        why)          echo "barrier_h_reopen() refuses a new epoch while a barrier is committed. MEASURED consequence, stall-rig arms F-13/H-13: an OVMX member stuck in the addition barrier of a joiner the real VAX had timed out refused the VAX's removal, and the VAX later timed the member out too.";;
        require_fail) cat <<'EOF'
...and is now the transition
the removal supersedes the stalled barrier
EOF
        ;;
        esac;;

    barrier-stalled-ignores-new-go)
        case "$_f" in
        facility)     echo "a new transition's bare GO supersedes a committed, stalled barrier (rd vms-eb3)";;
        targets)      echo "kernel-core/vms_cnxman_barrier_fsm.c";;
        suites_red)   echo "test_cnxman_barrier";;
        isolation)    echo "isolated";;
        why)          echo "barrier_h_go_in_step() treats every GO in STEP as a repeat, so a class-0x03 removal that starts at op 0x0a never reaches a member whose barrier stalled.";;
        require_fail) cat <<'EOF'
a bare removal GO supersedes it too, and its barrier starts
EOF
        ;;
        esac;;

    csb-conndata-count-ignored)
        case "$_f" in
        facility)     echo "csb_resume_from_conndata() (rd vms-ba4): the count a peer's CONNECT_REQ advertises it has taken resumes the dialogue on the accepted connection, before anyone speaks";;
        targets)      echo "kernel-core/vms_cnxman_csb.c";;
        suites_red)   echo "test_cnxman_csb";;
        isolation)    echo "isolated";;
        why)          echo "the resume from the connect data is disarmed, so a node that accepts a real VAX's re-establishing connect (cd[12:14] = 3) opens at send 1 / ack 0 -- the CNXMGRERR of stall-rig arm N2-4.";;
        require_fail) cat <<'EOF'
the next origination is 4 -- what the VAX waits for
and it acks the 3 this node advertised, not 0
the next origination is 3 -- where the VAX continues
and it acks the 2 our CONNECT advertised
what this node said about itself stands on the new connection: nothing is re-introduced
the bind resumes from it too
EOF
                      ;;
        esac;;

    csb-continued-dialogue-not-followed)
        case "$_f" in
        facility)     echo "cnxman_csb_dialogue_adopt() (rd vms-ba4): a peer whose first envelope after this node reset the dialogue carries send-msg# > 1 is continuing it, and this node resumes from the peer's ack";;
        targets)      echo "kernel-core/vms_cnxman_csb.c";;
        suites_red)   echo "test_cnxman_csb";;
        isolation)    echo "isolated";;
        why)          echo "the adoption is disarmed, so a pre-admission joiner answers a real VAX that continued the dialogue (send 3, ack 2) with send 1, ack 0 -- the CNXMGRERR measured on stall-rig arms L-3 and L-9.";;
        require_fail) cat <<'EOF'
the peer continued: this node resumes from its ack, so the next origination is 3, not the 1 that bugchecked it
the transaction id carries
counted
armed for ONE frame only
an unbind does not lose the frame it waits for
its ack of 2 makes this node's next send 3, not 1
EOF
                      ;;
        esac;;

    csb-dropped-spare-reads-as-loss)
        case "$_f" in
        facility)     echo "two VMS\$VAXcluster connections for one pair (rd vms-1f40: the peer disconnecting the redundant one of a crossing is not a loss of that system)";;
        targets)      echo "kernel-core/vms_cnxman_csb.c";;
        suites_red)   echo "test_cnxman_csb";;
        isolation)    echo "isolated";;
        why)          echo "cnxman_csb_second_closed() no longer recognises the pair's spare connection, so the peer disconnecting the redundant half of a crossing falls through to the ordinary remote-disconnect path: this node stops asking and the window ends in a removal while the connection the peer kept stands open. MEASURED consequence, rig arm Q-2: both ends re-dialled, both accepted, the real OpenVMS VAX V7.3 dropped the one it had initiated and kept this node's, and this node removed the VAX from its cluster.";;
        require_fail) cat <<'EOF'
the peer closing the spare is NOT a loss of the system
the spare is gone
EOF
        ;;
        esac;;

    pe-start-refusal-silent)
        case "$_f" in
        facility)     echo "saying WHY a circuit refused to form (rd vms-18a: counted-but-silent made a refused formation indistinguishable on a real console from one that was never asked)";;
        targets)      echo "kernel-core/vms_pe_fsm.c";;
        suites_red)   echo "test_pe_vc";;
        isolation)    echo "isolated";;
        why)          echo "vc_log_start_refused() returns before saying anything, so vc_begin_formation() puts the circuit back in CLOSED with nothing on the console -- the state that looks exactly like a node that never rejoins ('%PEA0, channel verified' and then silence). The counters are untouched, so the INV-6 accounting is unaffected: only the line goes.";;
        require_fail) cat <<'EOF'
and it is SAID on the console, not only counted
...naming WHICH read came back empty
EOF
        ;;
        esac;;


    codec-conndata-ack-cell-dropped)
        case "$_f" in
        facility)     echo "content[106:108] of the VMS\$VAXcluster connect data -- where this node's receive stream from the peer it is dialling stands (rd vms-8c54), and the [2]/[11] form that follows it";;
        targets)      echo "kernel-core/vms_cluster_codec_cm.c";;
        suites_red)   echo "test_codec_cm";;
        isolation)    echo "isolated";;
        why)          echo "vms_cm_conndata_build() writes 0 into content[106:108] instead of the caller's peer_ack_msg, so the two bytes that follow it collapse to the short form as well. The four rd vms-b87 rows are all ack-0 and stay green; only the rd vms-8c54 oracle rows -- two real OpenVMS VAX V7.3 members reconnecting, whose connects carry 14811 and 10249 -- go red. MEASURED consequence, rig arms N-6 and V-1: a zero there says 'I have taken nothing from you' to a peer that holds this node as a member, and the real VAX's connection manager bugchecked CNXMGRERR in the same millisecond as its own ACCEPT_RSP.";;
        require_fail) cat <<'EOF'
VAX1 re-establishing: it carries the 14811 it had TAKEN from VAX2
VAX2 accepting it: its own 10249, not VAX1's number
VAX1 -> VAX3: 1798, NOT the 1799 VAX3 kept retransmitting unanswered
...and with ONE message taken: long form, and the 1 is little-endian at [12:14]
EOF
        ;;
        esac;;

    csb-resume-ignores-peer-position)
        case "$_f" in
        facility)     echo "a carried dialogue resuming from the PEER'S acknowledged position (rd vms-1f40: anything sent on the connection that died was never delivered, and continuing past it leaves a hole in a strictly-monotonic stream)";;
        targets)      echo "kernel-core/vms_cnxman_csb.c";;
        suites_red)   echo "test_cnxman_csb";;
        isolation)    echo "isolated";;
        why)          echo "cnxman_csb_dialogue_acked() takes the peer's position but never applies it, so a re-established connection resumes from this node's own last send. The arming, the one-frame scope and the never-forward rule are all untouched -- only the rewind itself goes. MEASURED consequence, rig arm M2-5: this node sent send=103 twice on the old Con.ID pair, the real OpenVMS VAX V7.3 re-established and acked 102 having never seen 103, this node's next frame carried 104, and the VAX bugchecked CNXMGRERR.";;
        require_fail) cat <<'EOF'
...and this node resumes THERE, so its next origination is 103 -- the number the peer is waiting for, not the 104 that bugchecked the VAX
a LATER ack does not walk the counter back: the resume is armed for exactly one frame
an ack AHEAD of this node's own send is refused
and it really originates 103
two more sent, in flight
EOF
        ;;
        esac;;

    csb-reconnect-never-carries)
        case "$_f" in
        facility)     echo "p. 7-24's reconnect window as the SAME conversation (rd vms-8c54: a connection re-established to a system the cluster still holds keeps its send/ack dialogue)";;
        targets)      echo "kernel-core/vms_cnxman_csb.c";;
        suites_red)   echo "test_cnxman_csb";;
        isolation)    echo "isolated";;
        why)          echo "csb_dialogue_may_continue() is disarmed to return 0, so cnxman_csb_bind_reconnect() always falls through to the E77 reset and a re-established connection restarts at send-msg# 1 / ack 0. Every NOT-entitled case still resets, so nothing about E77 changes -- only the entitled one, which is the case both real OpenVMS VAX V7.3 members took when they continued 10249 -> 10250 and 14811 -> 14812 across a new Con.ID pair.";;
        require_fail) cat <<'EOF'
...and this node resumes THERE, so its next origination is 103 -- the number the peer is waiting for, not the 104 that bugchecked the VAX
...which is the cell the connect data carries
a LATER ack does not walk the counter back: the resume is armed for exactly one frame
an ack AHEAD of this node's own send is refused
and NOT as a reset
and counts as no resume
and is not counted again
and it really originates 103
and so does the ack: this node really HAS taken 14811 from that system, and saying 0 to a peer that holds it as a member is the lie the VAX bugchecks on
and so does the correlation token -- restarting it at 1 offers the peer a correlation it never issued, and the VAX bugchecked on it (arm K-10)
and what this node told that system about itself moved with it: a re-established member does NOT re-introduce itself, and the VAX bugchecked when it did (arm F-4)
counted as CARRIED
counted as a reset
counted as a resume
the carry is intact across the rebind
the send side CONTINUES -- the next origination is 3, as VAX1's 10249 became 10250
the transaction id CARRIES: a re-established member does not renumber mid-conversation (oracle VAX1 ran txn 3 across it)
two more sent, in flight
and the dialogue is carried, never restarted
dialogue carried across the move
not SELECTED yet, but in the answered transition: the re-established connection carries the ack (F5: 266)
and the send side
the SAME incarnation re-established: carried
EOF
        ;;
        esac;;

    pe-station-filter-disarmed)
        case "$_f" in
        facility)     echo "the LAN adapter's address filter a real PEDRIVER sits behind: only this station's own address and the group multicast ever reach the port (rd vms-6b1)";;
        targets)      echo "kernel-core/vms_pe_fsm.c";;
        suites_red)   echo "test_pe_vc";;
        isolation)    echo "isolated";;
        why)          echo "pe_frame_reaches_port() accepts every Ethernet destination ('1 ||'), so a frame a flooding hub or a promiscuous adapter delivers -- VAX1 talking to ANOTHER node -- is bound to this node's circuit by its source address alone: the foreign START re-forms our OPEN circuit (SCS told it went down, a STACK sent back) and the foreign sequenced messages are taken as ours, which is exactly the in-browser L2 hub's flood (tools/cluster-web-demo/l2/hub.mjs) the item names.";;
        require_fail) cat <<'EOF'
a START for another station does not re-form our circuit
a sequenced message for another station is not taken
and SCS is told nothing went down
and nothing is sent back: no STACK, no re-ack
each one is counted where an operator can see it
and the console says so
nor read as a gap
while our own next message is taken as usual
EOF
        ;;
        esac;;

    codec-remove-nodemap-unread)
        case "$_f" in
        facility)     echo "the op-0x08 REMOVE open's post-transition nodemap at body[55] (spec sec 4(p).R, rd vms-af4): nine real captured opens, five removals, three coordinators, removed slots 2/3/4";;
        targets)      echo "kernel-core/vms_cluster_codec_cm.c";;
        suites_red)   echo "test_codec_cm";;
        isolation)    echo "isolated";;
        why)          echo "vms_cm_open_carries_nodemap() answers yes for op 0x09 only, so the REMOVE open is read as carrying no nodemap -- the pre-af4 reading. A participant then commits a removal with membership untouched and keeps the removed system SELECTED: MEASURED on the rig, a member SIGKILLed and booted again stayed a MEMBER row on the surviving OVMX node with its old CSID for 300 s and the returning node was never readmitted.";;
        require_fail) cat <<'EOF'
and it carries a nodemap
body[55] == the slots the removal keeps
its span is readable
EOF
        ;;
        esac;;

    csb-dead-found-by-sysid)
        case "$_f" in
        facility)     echo "p. 7-25: the SCSSYSTEMID belongs to the new incarnation's fresh block, never to the old incarnation's DEAD one (rd vms-af4)";;
        targets)      echo "kernel-core/vms_cnxman_csb.c";;
        suites_red)   echo "test_cnxman_csb";;
        isolation)    echo "isolated";;
        why)          echo "cnxman_club_find_sysid() matches a DEAD block, so every lookup by SCSSYSTEMID -- the accept path's csb_ensure(), the discovery sweep, the join -- lands on the old incarnation's block and the new incarnation is driven through a dead one: the REACCEPT-into-the-old-block failure the rig measured.";;
        require_fail) cat <<'EOF'
and it is what the system is found as from now on
the SCSSYSTEMID no longer finds the old incarnation
EOF
        ;;
        esac;;

    pe-late-frame-revives-channel)
        case "$_f" in
        facility)     echo "SS4(M)'s listen timeout as a fact about ELAPSED TIME (rd vms-8c54: a stalled guest wakes with its clock AND its receive queue jumped together, and the queued frames are consumed before any beat runs)";;
        targets)      echo "kernel-core/vms_pe_fsm.c";;
        suites_red)   echo "test_pe_vc";;
        isolation)    echo "isolated";;
        why)          echo "pe_channel_expire_if_due() is disarmed at its first line, so a receive path refreshes a deadline that has ALREADY passed and the channel is never declared lost. The tick still finds it on the next beat, so nothing about an ordinary timeout changes -- only the stalled-guest case, where the frames were DEFERRED rather than destroyed and arrive before any beat. MEASURED consequence, rig arm N-1: the woken member wrote sequenced traffic on a circuit the real OpenVMS VAX V7.3 had closed 8.2 s earlier and answered its fresh START with a STACK from a circuit it still believed was OPEN; 20 s later each side removed the other.";;
        require_fail) cat <<'EOF'
the deferred beat ran: the channel went, not the circuit's own TIMVCFAIL
and SCS was told the circuit is gone
the circuit is NOT still open on a channel that timed out
a late SEQUENCED message does not revive it either
SCS was told, once
EOF
        ;;
        esac;;

    pe-last-gasp-once-per-port)
        case "$_f" in
        facility)     echo "p. 7-29's departure announcement, once per INCARNATION (rd vms-8c54: CLUEXIT re-incarnates IN PLACE, so a guard scoped to the port lifecycle silences every announcement after the first)";;
        targets)      echo "kernel-core/vms_pe_fsm.c";;
        suites_red)   echo "test_pe_formation";;
        isolation)    echo "isolated";;
        why)          echo "pe_gasp_already_sent() is reverted to the once-per-PORT rule by returning 1 for any port that has ever gasped. A real node reboots between departures and so gets a fresh port each time; OVMX's CLUEXIT does not, so under this defect the SECOND and every later re-incarnation announces NOTHING and every peer keeps a CSB for a node that has already thrown its cluster state away -- the state the measured E81 CNXMGRERR family lives in. The last gasp is still built, still counted and still correct for the FIRST departure, so nothing but the per-incarnation property goes red.";;
        require_fail) cat <<'EOF'
a re-incarnated node ANNOUNCES ITS DEPARTURE AGAIN
and it is the same b1 marker, not some other frame
both are counted
still two
EOF
        ;;
        esac;;

    coord-removal-open-gate-disarmed)
        case "$_f" in
        facility)     echo "the INV-6 refusal to originate a class-0x03 REMOVAL open toward a connection manager this executive cannot build one for (rd vms-0f9: a real op-0x08 carries 44 bytes OVMX has no derivation for)";;
        targets)      echo "kernel-core/vms_cnxman_coord_fsm.c";;
        suites_red)   echo "test_cnxman_coord";;
        isolation)    echo "isolated";;
        why)          echo "cnxman_coord_select()'s DEPARTURE branch has its coord_open_is_grounded_for() call disarmed with '0 &&', so a survivor opens a class-0x03 removal toward a system that has NOT proved it runs this implementation -- putting an op-0x08 with 44 zero bytes where a real coordinator writes two VMS absolute-time quadwords and four longwords. MEASURED consequence, jittered three-node rig arm V2-1: the real OpenVMS VAX V7.3 put its last-gasp datagram on the multicast in the SAME millisecond.";;
        require_fail) cat <<'EOF'
the removal is REFUSED, not driven
...and named OPEN_UNGROUNDED
...and counted, so the gap is visible without a capture
NOTHING went on the wire -- above all no op 0x08 this node cannot build faithfully
and no transition was opened
EOF
        ;;
        esac;;

    coord-open-cells-not-attached)
        case "$_f" in
        facility)     echo "the Phase 1 cells of an ADD open (rd vms-f297): founder, formation time, slot counter, quorum, rebuild type, the subject's QDSKVOTES and op-0x02 count, the CSV block -- every one from executive state";;
        targets)      echo "kernel-core/vms_cnxman_coord_fsm.c";;
        suites_red)   echo "test_cnxman_coord";;
        isolation)    echo "isolated";;
        why)          echo "coord_send_open() never attaches the cells it filled, so the open a real VAX member would receive is the zero-celled one that bugchecked a real V7.3 (rd vms-1ac) -- and the send-time withhold then keeps it off the wire, so an admission a real VAX takes part in never completes.";;
        require_fail) cat <<'EOF'
the foreign member got its op 0x09
[20:22] the slot after the joiner's (4 -> 5)
[22:24] quorum: (CEVOTES + 2) / 2 = (6 + 2) / 2
[24] directory rebuild: the joiner weighs 1
[26:28] the JOINER's QDSKVOTES
[32:40] the formation time held
[40:48] this node's clock
[49:51] the founder, not us
[87:89] the count the joiner's own op 0x02 carried
[96:98] CEVOTES: the joiner's EXPECTED_VOTES 6 beats four votes
[98:100] lowest slot
[100:102] a first admission: one below its slot
[104:106] highest slot
[106:114] the -900 s delta
the admission is proposed
[24] = 1, the merge rebuild
EOF
                      ;;
        esac;;

    coord-open-withhold-disarmed)
        case "$_f" in
        facility)     echo "the send-time withhold (rd vms-f297): an open whose cells cannot be filled is never sent to a cluster with a foreign member";;
        targets)      echo "kernel-core/vms_cnxman_coord_fsm.c";;
        suites_red)   echo "test_cnxman_coord";;
        isolation)    echo "isolated";;
        why)          echo "a fact lost between the gate and the send (the clock, here) no longer withholds the open: a real VAX member receives an op 0x09 with zeros where its own coordinator writes facts -- the frame after which a real V7.3 bugchecked CNXMGRERR (rd vms-1ac).";;
        require_fail) cat <<'EOF'
the foreign member is sent no open at all
withheld from every participant alike, and counted
EOF
                      ;;
        esac;;

    club-open-facts-unlearned)
        case "$_f" in
        facility)     echo "a participant keeps the cluster facts a received open carries (rd vms-f297): founder, formation time, slot counter";;
        targets)      echo "kernel-core/vms_cnxman_barrier_fsm.c";;
        suites_red)   echo "test_cnxman_barrier";;
        isolation)    echo "isolated";;
        why)          echo "barrier_h_open() no longer records them, so an OVMX member admitted by a real VAX holds no founder or formation time and can never coordinate the next admission -- the vms-f297 stall (the VAX defers to the higher-numbered OVMX member, which refuses) comes back for good.";;
        require_fail) cat <<'EOF'
the founder's SCSSYSTEMID is kept
and the formation time
and the slot counter
EOF
                      ;;
        esac;;

    coord-ignores-rejection)
        case "$_f" in
        facility)     echo "a 0x81 answer without the accepting 0x01 is a rejection, and the coordinator abandons the transition (book p. 7-41; rd vms-f297)";;
        targets)      echo "kernel-core/vms_cnxman_coord_fsm.c";;
        suites_red)   echo "test_cnxman_coord";;
        isolation)    echo "isolated";;
        why)          echo "a member's status-00 answer to a membership record is taken as an acknowledgement and the open follows it -- the exact sequence after which a real OpenVMS VAX V7.3 bugchecked CNXMGRERR on lab arms PF-3 and PK-1.";;
        require_fail) cat <<'EOF'
no open goes out after a rejection
the rejection is counted
and the transition is abandoned
and said
EOF
                      ;;
        esac;;

    scs-accept-conndata-dropped)
        case "$_f" in
        facility)     echo "the initiating CDT keeps the peer's ACCEPT_REQ connect data for the SYSAP (rd vms-f297)";;
        targets)      echo "kernel-core/vms_scs_fsm.c";;
        suites_red)   echo "test_scs_fsm";;
        isolation)    echo "isolated";;
        why)          echo "h_rx_accept() drops the accept's connect data, so the VMS\$VAXcluster SYSAP cannot see that a real VAX accepting a re-dialled connection CONTINUED the conversation -- the CNXMGRERR of stall-rig arms GM-14, TG-3 and HM-11.";;
        require_fail) cat <<'EOF'
A's CDT holds the ACCEPT's connect data
EOF
                      ;;
        esac;;

    csb-accept-resume-disarmed)
        case "$_f" in
        facility)     echo "a re-dialled conversation resumes from the count the peer's ACCEPT states (rd vms-f297)";;
        targets)      echo "kernel-core/vms_cnxman_csb.c";;
        suites_red)   echo "test_cnxman_csb";;
        isolation)    echo "isolated";;
        why)          echo "cnxman_csb_note_accept_conndata() ignores the ACCEPT's count: this node speaks at send 1 / ack 0 on a connection a real VAX has just re-established at send 3 -- CNXMGRERR (GM-14, TG-3, HM-11).";;
        require_fail) cat <<'EOF'
the next origination is 3 -- where the VAX continues
and it acks the 2 our CONNECT advertised
what this node said about itself stands on the new connection: nothing is re-introduced
the bind resumes from it too
EOF
                      ;;
        esac;;

    csb-resume-reintroduces)
        case "$_f" in
        facility)     echo "a conversation resumed from the ACCEPT keeps what this node already said about itself (rd vms-f297)";;
        targets)      echo "kernel-core/vms_cnxman_csb.c";;
        suites_red)   echo "test_cnxman_csb";;
        isolation)    echo "isolated";;
        why)          echo "the resume leaves the advert mask on the dead connection, so this node re-introduces itself (MODEL/PARAMS) mid-stream on the continued conversation -- the frame a real VAX bugchecked on (rd vms-8c54 arm F-4).";;
        require_fail) cat <<'EOF'
what this node said about itself stands on the new connection: nothing is re-introduced
EOF
                      ;;
        esac;;

    join-asks-before-telling-members)
        case "$_f" in
        facility)     echo "a joiner tells every connected member who it is before it asks for admission, as a real V7.3 joiner does (rd vms-f297)";;
        targets)      echo "kernel-core/vms_cnxman_join_fsm.c";;
        suites_red)   echo "test_cnxman_join";;
        isolation)    echo "isolated";;
        why)          echo "the request goes out while a connected member has not been sent this node's MODEL/PARAMS: on lab arm PK-1 the coordinator's record about OVMXB reached a real VAX holding no PARAMS for it, which answered status 00 and bugchecked CNXMGRERR on the open.";;
        require_fail) cat <<'EOF'
the request is held while the other member is owed this node's identity
and the hold is counted
...which goes out to it at once
... and nothing was re-issued
every beat inside the window is counted, and none of them asks the cluster anything
five beats after the abort is not yet a decline
five beats of silence is not yet a decline
nor one second before it elapses
the back-off is not re-armed by a start that ran
the other member hears only who this node is, never a request
the other member is never asked: ONE op-0x02 per attempt, and it was answered -- it heard only who this node is
EOF
                      ;;
        esac;;

    coord-open-skips-records-wait)
        case "$_f" in
        facility)     echo "the coordinator's Phase 1 open waits for every op-0x05 membership record's 0x81/0x05 answer, as a real V7.3 coordinator's does (rd vms-f297)";;
        targets)      echo "kernel-core/vms_cnxman_coord_fsm.c";;
        suites_red)   echo "test_cnxman_coord";;
        isolation)    echo "isolated";;
        why)          echo "coord_enter_open() sends the open in the same instant as the records: a real OpenVMS VAX V7.3 member, sent op 05 and op 09 together by an OVMX coordinator, bugchecked CNXMGRERR (lab arm PF-3), where its own coordinator always sends op 05, takes the answer, then op 09 (lab run XF).";;
        require_fail) cat <<'EOF'
and NO open yet: their answers come first
the coordinator waits in RECORDS
each 0x81/0x05 consumed and counted, none unrouted
no open goes out after a rejection
the rejection is counted
and the transition is abandoned
and said
EOF
                      ;;
        esac;;

    coord-step-ack-unmarked)
        case "$_f" in
        facility)     echo "the coordinator's 0x81/0x0b step acknowledgement carries 10 <class> 01 at body[16:19], as every real one does (rd vms-f297)";;
        targets)      echo "kernel-core/vms_cluster_codec_cm.c";;
        suites_red)   echo "test_codec_cm";;
        isolation)    echo "isolated";;
        why)          echo "vms_cm_step_ack_build() drops the response marker at body[18]: the ack goes out 10 02 00, which OVMX's own participant never reads -- and a real OpenVMS VAX V7.3 member, sent such an ack by an OVMX coordinator, re-sent its step and bugchecked CNXMGRERR (lab arm PF-2).";;
        require_fail) cat <<'EOF'
  byte-identical to the real ack after the stamp
  body[18] = the response marker
EOF
                      ;;
        esac;;

    join-swallows-step-reports)
        case "$_f" in
        facility)     echo "join_forward() hands a member's op-0x0b step report on to this node's coordinator (rd vms-f297)";;
        targets)      echo "kernel-core/vms_cnxman_join_fsm.c";;
        suites_red)   echo "test_cnxman_join";;
        isolation)    echo "isolated";;
        why)          echo "the join FSM swallows every op-0x0b in [MEMBER], so a node that was itself admitted and then coordinates never sees its participants' step reports: the real VAX member of lab arm PF-1 reported step 1 and was never released.";;
        require_fail) cat <<'EOF'
a step REPORT is not the join's or the participant's: it is handed on
and is not counted as delivered to the participant
EOF
                      ;;
        esac;;

    removal-pair-not-rederived)
        case "$_f" in
        facility)     echo "a committed removal re-derives the last-reconfiguration (members, votes) pair every later open carries (rd vms-f297)";;
        targets)      echo "kernel-core/vms_cnxman_phase2.c";;
        suites_red)   echo "test_cnxman_barrier";;
        isolation)    echo "isolated";;
        why)          echo "cnxman_phase2_commit() skips cnxman_club_note_reconfig(), so after a removal this node's next ADD open still carries the pair from before it -- a different number from the one a real V7.3 member puts there (KR-1: 01 01 before the removal, 02 02 after).";;
        require_fail) cat <<'EOF'
the pair is held
its members: the two the removal keeps
its votes: theirs, 2 + 1
EOF
                      ;;
        esac;;

    coord-admission-open-gate-disarmed)
        case "$_f" in
        facility)     echo "the INV-6 refusal to originate a class-0x02 transition open toward a connection manager while this executive lacks a fact the open carries (rd vms-1ac, vms-f297)";;
        targets)      echo "kernel-core/vms_cnxman_coord_fsm.c";;
        suites_red)   echo "test_cnxman_coord";;
        isolation)    echo "isolated";;
        why)          echo "coord_admit_foreign()'s gap test is disarmed with '1 ||', so this node opens a class-0x02 transition toward a system that does not run this implementation even when it holds no founder, formation time or clock -- the relay and commit go out, and only the send-time withhold stands between a real VAX and the zero-celled op 0x09 after which a real OpenVMS VAX V7.3 bugchecked CNXMGRERR 1.3 ms later.";;
        require_fail) cat <<'EOF'
nothing is originated: no relay, no commit, and above all no op-0x09 this node cannot build faithfully
the refusal is COUNTED
...and named
...and SAID, because a stranded admission is a gap to close
  the refusal is counted
  and names the missing fact
and no transition was opened
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
    quorum-form-set-ignores-peers)
        # The ONE unconditional add of a CSB's votes into the cold-formation
        # sum; making it conditional on the LOCAL flag is exactly the old
        # own-votes-only rule, with nothing else in the walk touched.
        sed -i 's|^\t\tout->sum_votes += (uint32_t)csb->votes;$|\t\tif ((csb->flags \& VMS_CSB_F_LOCAL) != 0u)  /* NEGCTL quorum-form-set-ignores-peers */\n\t\t\tout->sum_votes += (uint32_t)csb->votes;|' "$_file";;

    coord-genesis-refusal-uncounted)
        # c->genesis_refused_noquorum++; is unique in this file (grep -c is
        # 1) -- no line anchor needed. The refusal three lines below is left
        # completely alone.
        sed -i 's|c->genesis_refused_noquorum++;|/* NEGCTL coord-genesis-refusal-uncounted: the refusal is not counted */|' "$_file";;

    dlm-hash-empty-name-not-refused)
        # `name_len == 0u || ` is unique in this file; dropping just that
        # disjunct leaves the upper bound armed, so the mutated build cannot
        # read past the test's name buffer -- the red set stays the two
        # assertions the manifest declares and nothing else.
        sed -i 's|if (name_len == 0u \|\| name_len > VMS_DLM_HASH_NAME_MAX)|if (name_len > VMS_DLM_HASH_NAME_MAX) /* NEGCTL dlm-hash-empty-name-not-refused */|' "$_file";;

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

    dlm-learner-unbounded)
        # `vms_res_blocks < VMS_DLM_LEARN_RES_CAP;` is unique in this file.
        sed -i 's|vms_res_blocks < VMS_DLM_LEARN_RES_CAP;|1; /* NEGCTL dlm-learner-unbounded */|' "$_file";;

    dlm-own-directory-not-consulted)
        # `if (!inbound && dlm_route_own_directory(res, route, dst_csid, &st))`
        # is unique in this file.
        sed -i 's|if (!inbound \&\& dlm_route_own_directory(res, route, dst_csid, \&st))|if (0 \&\& !inbound \&\& dlm_route_own_directory(res, route, dst_csid, \&st)) /* NEGCTL dlm-own-directory-not-consulted */|' "$_file";;

    dlm-self-claim-answered-you-master)
        # `if (dir_self_claims(d, id, self)) {` is unique in this file.
        sed -i 's|if (dir_self_claims(d, id, self)) {|if (0 \&\& dir_self_claims(d, id, self)) { /* NEGCTL dlm-self-claim-answered-you-master */|' "$_file";;

    dlm-dir-remove-by-anyone)
        # `if (i < 0 || d->slot[i].master != master) {` is unique in this file.
        sed -i 's#if (i < 0 || d->slot\[i\].master != master) {#if (i < 0 || (d->slot[i].master != master \&\& 0)) { /* NEGCTL dlm-dir-remove-by-anyone */#' "$_file";;

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

    coord-relay-epoch-advanced-early)
        # coord_claim_club()'s two lines, replaced with the pre-vms-1ac pair.
        # `c->epoch_advanced = 1u` keeps coord_advance_epoch() idempotent, so
        # the transition still runs -- only WHEN the epoch moved changes.
        sed -i 's|\tc->epoch = club->epoch;|\tc->epoch = club->epoch + 1u; club->epoch = c->epoch; /* NEGCTL coord-relay-epoch-advanced-early */|' "$_file"
        sed -i 's|\tc->epoch_advanced = 0u;|\tc->epoch_advanced = 1u;|' "$_file";;

    coord-membrec-epoch-zero)
        # `rec.epoch   = c->epoch;` is unique in this file.
        sed -i 's|rec.epoch   = c->epoch;.*|/* NEGCTL coord-membrec-epoch-zero: the record names no transition */|' "$_file";;

    coord-admission-not-selected-disarmed)
        sed -i 's|if (coord_outranked_for_admission(c, subject_csb)) {|if (0 \&\& coord_outranked_for_admission(c, subject_csb)) { /* NEGCTL coord-admission-not-selected-disarmed */|' "$_file";;

    coord-admission-open-gate-disarmed)
        # `if (gap == CNXMAN_OPEN_GAP_NONE)` is unique: coord_admit_foreign().
        sed -i 's|if (gap == CNXMAN_OPEN_GAP_NONE)$|if (1 \|\| gap == CNXMAN_OPEN_GAP_NONE) /* NEGCTL coord-admission-open-gate-disarmed */|' "$_file";;

    coord-open-cells-not-attached)
        sed -i 's|^\t\tcp = \&cells;$|\t\tcp = NULL; /* NEGCTL coord-open-cells-not-attached */|' "$_file";;

    coord-open-withhold-disarmed)
        sed -i 's|if (cp == NULL \&\& c->tr_class == VMS_CM_CLASS_ADD \&\&|if (0 \&\& cp == NULL \&\& c->tr_class == VMS_CM_CLASS_ADD \&\& /* NEGCTL coord-open-withhold-disarmed */|' "$_file";;

    club-open-facts-unlearned)
        sed -i 's|^\tcnxman_club_learn_open(\&b->cl->club,$|\tif (0) /* NEGCTL club-open-facts-unlearned */ cnxman_club_learn_open(\&b->cl->club,|' "$_file";;

    coord-ignores-rejection)
        sed -i 's|^\tif (vms_cm_response_accepted(m->body, m->len))$|\tif (1 \|\| vms_cm_response_accepted(m->body, m->len)) /* NEGCTL coord-ignores-rejection */|' "$_file";;

    scs-accept-conndata-dropped)
        sed -i 's|^\tcdt->peer_accept_conndata_valid = 1u;$|\tcdt->peer_accept_conndata_valid = 0u; /* NEGCTL scs-accept-conndata-dropped */|' "$_file";;

    csb-accept-resume-disarmed)
        sed -i 's|^\tif (csb == NULL \|\| peer_taken == 0u)$|\tif (1 \|\| csb == NULL \|\| peer_taken == 0u) /* NEGCTL csb-accept-resume-disarmed */|' "$_file";;

    csb-resume-reintroduces)
        sed -i 's|^\tif (csb->cm_resume_carries_advert)$|\tif (0 \&\& csb->cm_resume_carries_advert) /* NEGCTL csb-resume-reintroduces */|' "$_file";;

    join-asks-before-telling-members)
        sed -i 's|^\tif (join_peer_ident_owed(j))$|\tif (0 \&\& join_peer_ident_owed(j)) /* NEGCTL join-asks-before-telling-members */|' "$_file";;

    coord-open-skips-records-wait)
        sed -i 's|^\tif (coord_records_outstanding(c) != 0u) {$|\tif (0 \&\& coord_records_outstanding(c) != 0u) { /* NEGCTL coord-open-skips-records-wait */|' "$_file";;

    coord-step-ack-unmarked)
        sed -i 's|^\tvms_wire_put_u8(\&w, VMS_OFF_CM_RESP_MARK - VMS_OFF_SYSAP_BODY, 0x01u);$|\t/* NEGCTL coord-step-ack-unmarked */|' "$_file";;

    join-swallows-step-reports)
        sed -i 's|^\t    CNXMAN_BARRIER_RX_NOT_MINE)$|\t    CNXMAN_BARRIER_RX_NOT_MINE \&\& 0) /* NEGCTL join-swallows-step-reports */|' "$_file";;

    removal-pair-not-rederived)
        sed -i 's|^\tif (in->reconfig)$|\tif (0 \&\& in->reconfig) /* NEGCTL removal-pair-not-rederived */|' "$_file";;

    pe-receive-hold-disarmed)
        # `if (rx->frame == NULL || rx->len > PE_VC_FRAME_MAX ||` is unique.
        sed -i 's/if (rx->frame == NULL || rx->len > PE_VC_FRAME_MAX ||/if (1 || rx->frame == NULL || rx->len > PE_VC_FRAME_MAX || \/\* NEGCTL pe-receive-hold-disarmed \*\//' "$_file";;

    csb-abandoned-connect-keeps-conid)
        # The bind that follows the comment ending `with no connect in flight.`
        sed -i '/with no connect in flight\./{n;n;s|cnxman_csb_bind_connection(csb, 0u);|/* NEGCTL csb-abandoned-connect-keeps-conid */|}' "$_file";;

    pe-reformation-stacks-before-it-starts)
        # `if (ch != NULL && ch->verifies > 1u)` is unique in this file.
        sed -i 's|if (!vc->own_start_sent \&\& vc_is_reformation(f, vc))|if ((void)vc_is_reformation, 0) /* NEGCTL pe-reformation-stacks-before-it-starts */|' "$_file";;

    pe-peer-start-keeps-dead-echo)
        # vc_take_echo() is called at three formation sites; the range pins the
        # one in h_vc_rx_start (`vc->starts_rx++;` is unique in this file), and
        # the call VANISHES when replaced, so the mutation is not repeatable.
        sed -i '/vc->starts_rx++;/,/vc_learn_peer(f, vc, rx);/ s|vc_take_echo(f, vc);|(void)vc_take_echo; /* NEGCTL pe-peer-start-keeps-dead-echo */|' "$_file";;

    recnx-attempt-supersedes-in-flight)
        # `    csb->attempt_in_flight) {` is unique in this file.
        sed -i 's|    csb->attempt_in_flight) {|    0 \&\& csb->attempt_in_flight) { /* NEGCTL recnx-attempt-supersedes-in-flight */|' "$_file";;

    join-asks-the-last-discovered)
        # rd vms-e88: the edited line is unique in vms_cnxman_join_fsm.c and the
        # replacement no longer matches the pattern, so it is not repeatable.
        sed -i 's|	return a->sysid > b->sysid;|	return 1; /* NEGCTL join-asks-the-last-discovered: the last askable block wins, the old queue-tail rule */|' "$_file";;

    join-connectivity-gate-disarmed)
        # rd vms-e88: the edited line is unique in vms_cnxman_join_fsm.c and the
        # replacement no longer matches the pattern, so it is not repeatable.
        sed -i 's|	if ((uint32_t)v.max_advertised > v.members_connected)|	if (0 \&\& (uint32_t)v.max_advertised > v.members_connected) /* NEGCTL join-connectivity-gate-disarmed */|' "$_file";;

    join-unheard-gate-disarmed)
        # rd vms-e88: the edited line is unique in vms_cnxman_join_fsm.c and the
        # replacement no longer matches the pattern, so it is not repeatable.
        sed -i 's|	if (csb == NULL \|\| !csb->adv_valid)|	if (csb == NULL) /* NEGCTL join-unheard-gate-disarmed */|' "$_file";;

    join-same-breath-gate-disarmed)
        # rd vms-e88: the edited line is unique in vms_cnxman_join_fsm.c and the
        # replacement no longer matches the pattern, so it is not repeatable.
        sed -i 's|	if (join_ident_fresh(j))|	if (0 \&\& join_ident_fresh(j)) /* NEGCTL join-same-breath-gate-disarmed */|' "$_file";;

    join-follow-csb-conn-disarmed)
        # rd vms-e88: the edited line is unique in vms_cnxman_join_fsm.c and the
        # replacement no longer matches the pattern, so it is not repeatable.
        sed -i 's|	if (csb != NULL \&\& join_follow_csb_conn(j, csb))|	if (0 \&\& csb != NULL \&\& join_follow_csb_conn(j, csb)) /* NEGCTL join-follow-csb-conn-disarmed */|' "$_file";;

    join-member-count-to-foreign)
        # rd vms-e88: the edited line is unique in vms_cnxman_join_fsm.c and the
        # replacement no longer matches the pattern, so it is not repeatable.
        sed -i 's|	if (csb == NULL \|\| !csb->peer_is_ours \|\| !join_node_already_member(j))|	if (csb == NULL \|\| !join_node_already_member(j)) /* NEGCTL join-member-count-to-foreign */|' "$_file";;

    join-asks-a-system-in-no-cluster)
        # rd vms-e88: the edited line is unique in vms_cnxman_join_fsm.c and the
        # replacement no longer matches the pattern, so it is not repeatable.
        sed -i 's|	if (csb->adv_valid \&\& !join_says_member(csb))|	if (0 \&\& csb->adv_valid \&\& !join_says_member(csb)) /* NEGCTL join-asks-a-system-in-no-cluster */|' "$_file";;

    join-does-not-reach-ours)
        # rd vms-e88: the call is unique in vms_cnxman_join_fsm.c and VANISHES
        # when replaced, so the mutation is not repeatable.
        sed -i 's|		join_reach_ours(j);|		(void)join_reach_ours; /* NEGCTL join-does-not-reach-ours */|' "$_file";;

    coord-genesis-ignores-says-member)
        # rd vms-e88: the edited line is unique in vms_cnxman_coord_fsm.c and the
        # replacement no longer matches the pattern, so it is not repeatable.
        sed -i 's|	       coord_peer_says_member(csb);|	       (0 \&\& coord_peer_says_member(csb)); /* NEGCTL coord-genesis-ignores-says-member */|' "$_file";;

    join-no-member-backoff-not-cut)
        # rd vms-e88: the guard is unique in vms_cnxman_join_fsm.c and the
        # replacement no longer matches the pattern, so it is not repeatable.
        sed -i 's|	if (j->backoff_no_member \&\& j->cl != NULL \&\& join_member_in_sight(j)) {|	if (0 \&\& j->backoff_no_member \&\& j->cl != NULL \&\& join_member_in_sight(j)) { /* NEGCTL join-no-member-backoff-not-cut */|' "$_file";;

    join-drive-stays-on-a-joiner)
        # rd vms-e88: the guard is unique in vms_cnxman_join_fsm.c and the
        # replacement no longer matches the pattern, so it is not repeatable.
        sed -i 's|	if (v.best == NULL \|\| v.best_slot == j->target_csb \|\||	if (1 \|\| v.best == NULL \|\| v.best_slot == j->target_csb \|\| /* NEGCTL join-drive-stays-on-a-joiner */|' "$_file";;

    csb-phase1-named-not-carried)
        # rd vms-eb3: the matched text is unique in its file and the replacement
        # no longer matches, so the mutation is not repeatable.
        sed -i 's|	if ((csb->flags \& VMS_CSB_F_SELECTED) == 0u \&\& !csb->cm_phase1_named)|	if ((csb->flags \& VMS_CSB_F_SELECTED) == 0u) /* NEGCTL csb-phase1-named-not-carried */|' "$_file";;

    barrier-phase1-not-marked)
        # rd vms-eb3: the matched text is unique in its file and the replacement
        # no longer matches, so the mutation is not repeatable.
        sed -i 's|^	barrier_phase1_mark(b);$|	(void)barrier_phase1_mark; /* NEGCTL barrier-phase1-not-marked */|' "$_file";;

    barrier-phase1-not-cleared)
        # rd vms-eb3: the matched text is unique in its file and the replacement
        # no longer matches, so the mutation is not repeatable.
        sed -i 's|	cnxman_club_phase1_clear(\&b->cl->club);|	(void)b; /* NEGCTL barrier-phase1-not-cleared */|' "$_file";;

    join-transition-loss-redriven)
        # rd vms-eb3: the matched text is unique in its file and the replacement
        # no longer matches, so the mutation is not repeatable.
        sed -i '/^static void join_cm_connection_gone/,/^}/ s|	if (join_transition_in_progress(j)) {|	if (0 \&\& join_transition_in_progress(j)) { /* NEGCTL join-transition-loss-redriven */|' "$_file";;

    join-transition-reoffers-burst)
        # rd vms-eb3: the matched text is unique in its file and the replacement
        # no longer matches, so the mutation is not repeatable.
        sed -i '/^static void join_reoffer_burst/,/^}/ s|	if (join_transition_in_progress(j))$|	if (0 \&\& join_transition_in_progress(j)) /* NEGCTL join-transition-reoffers-burst */|' "$_file";;

    glue-close-abandons-held-transition)
        # rd vms-eb3: the matched text is unique in its file and the replacement
        # no longer matches, so the mutation is not repeatable.
        sed -i 's|	if (window_over \&\& cn->barrier.coordinator_csb == idx)|	if (cn->barrier.coordinator_csb == idx) /* NEGCTL glue-close-abandons-held-transition */|' "$_file";;

    csb-new-incarnation-carried)
        # rd vms-eb3: the matched text is unique in its file and the replacement
        # no longer matches, so the mutation is not repeatable.
        sed -i 's|	if (csb->cm_new_incarnation)|	if (0 \&\& csb->cm_new_incarnation) /* NEGCTL csb-new-incarnation-carried */|' "$_file";;

    barrier-stalled-refuses-new-transition)
        # rd vms-eb3: the matched text is unique in its file and the replacement
        # no longer matches, so the mutation is not repeatable.
        sed -i 's|		barrier_supersede_committed(b);\n|X|; s|^		barrier_h_open(b, m);$|		barrier_h_open(b, m);|; /^	if (b->state == (uint8_t)CNXMAN_BARRIER_STEP) {$/,/^	}$/ s|^		barrier_supersede_committed(b);$|		barrier_respond_echo(b, m); return; /* NEGCTL barrier-stalled-refuses-new-transition */|' "$_file";;

    barrier-stalled-ignores-new-go)
        # rd vms-eb3: the matched text is unique in its file and the replacement
        # no longer matches, so the mutation is not repeatable.
        sed -i 's|	    go.role != VMS_CM_ROLE_GO \|\| go.epoch == b->epoch) {|	    1) { /* NEGCTL barrier-stalled-ignores-new-go */|' "$_file";;

    csb-conndata-count-ignored)
        # `	csb->cm_send_msg = taken;` is unique in this file.
        sed -i 's|^\tcsb->cm_send_msg = taken;|\treturn; /* NEGCTL csb-conndata-count-ignored */|' "$_file";;

    csb-continued-dialogue-not-followed)
        # `csb->cm_send_msg = peer_ack_msg;` occurs TWICE in this file; the
        # adoption's is the one followed by `csb->cm_txn = csb->cm_prev_txn;`.
        sed -i '/^\tcsb->cm_send_msg = peer_ack_msg;$/{N;s|^\tcsb->cm_send_msg = peer_ack_msg;\n\tcsb->cm_txn = csb->cm_prev_txn;|\treturn; /* NEGCTL csb-continued-dialogue-not-followed */\n\tcsb->cm_txn = csb->cm_prev_txn;|}' "$_file";;

    csb-dropped-spare-reads-as-loss)
        # `	if (csb->alt_conid == conid) {` is unique in this file.
        sed -i 's|	if (csb->alt_conid == conid) {|	if (0 \&\& csb->alt_conid == conid) { /* NEGCTL csb-dropped-spare-reads-as-loss */|' "$_file";;

    pe-start-refusal-silent)
        # The CALL is unique and VANISHES when replaced, so the mutation is not
        # repeatable; the function stays referenced so nothing goes unused.
        sed -i 's|vc_log_start_refused(f, vc);|(void)vc_log_start_refused; /* NEGCTL pe-start-refusal-silent */|' "$_file";;


    codec-conndata-ack-cell-dropped)
        # `out[12] = (uint8_t)(in->peer_ack_msg & 0xffu);` is unique here;
        # zeroing the low byte is enough to break the cell AND the form,
        # because out[2]/out[11] are derived from the same input.
        sed -i 's|out\[2\] = (uint8_t)(in->peer_ack_msg != 0u ? 0x02u : 0x01u);|out[2] = 0x01u; /* NEGCTL codec-conndata-ack-cell-dropped */|' "$_file"
        sed -i 's|out\[11\] = (uint8_t)(in->peer_ack_msg != 0u ? 0x0au : 0x08u);|out[11] = 0x08u;|' "$_file"
        sed -i 's|out\[12\] = (uint8_t)(in->peer_ack_msg \& 0xffu);|out[12] = 0u;|' "$_file"
        sed -i 's|out\[13\] = (uint8_t)((in->peer_ack_msg >> 8) \& 0xffu);|out[13] = 0u;|' "$_file";;

    csb-resume-ignores-peer-position)
        # `csb->cm_send_msg = peer_ack_msg;` occurs twice since rd vms-ba4;
        # the resume's is the one indented two tabs.
        sed -i 's|^\t\tcsb->cm_send_msg = peer_ack_msg;|\t\t/* NEGCTL csb-resume-ignores-peer-position: the position is not taken */|' "$_file";;

    csb-reconnect-never-carries)
        # `if (csb->cm_dialogue_conid == 0u)` is unique in this file.
        sed -i 's|if (csb->cm_dialogue_conid == 0u)|if (1) /* NEGCTL csb-reconnect-never-carries: never entitled */|' "$_file";;

    pe-station-filter-disarmed)
        # `if (pe_station_accepts(f, hdr.eth_dst))` is unique in this file.
        sed -i 's|if (pe_station_accepts(f, hdr.eth_dst))|if (1 \|\| pe_station_accepts(f, hdr.eth_dst)) /* NEGCTL pe-station-filter-disarmed */|' "$_file";;

    codec-remove-nodemap-unread)
        sed -i 's#return opcode == VMS_CM_OP_XITION_ADD || opcode == VMS_CM_OP_XITION_REM;#return opcode == VMS_CM_OP_XITION_ADD; /* NEGCTL codec-remove-nodemap-unread */#' "$_file";;

    csb-dead-found-by-sysid)
        sed -i 's#\t\tif (club->csb\[i\].state == (uint8_t)VMS_CNXMAN_CSB_DEAD)#\t\tif (0 \&\& club->csb[i].state == (uint8_t)VMS_CNXMAN_CSB_DEAD) /* NEGCTL csb-dead-found-by-sysid */#' "$_file";;

    pe-late-frame-revives-channel)
        # `if (ch->deadline_ms == 0u)` is unique in this file.
        sed -i 's|if (ch->deadline_ms == 0u)|if (1) /* NEGCTL pe-late-frame-revives-channel: a late frame refreshes the deadline */|' "$_file";;

    pe-last-gasp-once-per-port)
        # The `return f->last_gasp_incarnation == f->id.incarnation_time;`
        # line is unique in this file: grep -c is 1.
        sed -i 's|return f->last_gasp_incarnation == f->id.incarnation_time;|return 1; /* NEGCTL pe-last-gasp-once-per-port: once per PORT, as before */|' "$_file";;

    join-reply-to-the-join-target)
        # The ONE response transmit in this file (grep -c for
        # `j->ops->respond(` is 1): replaced with the pre-vms-e8b target send.
        # The envelope stamp above it is untouched, so the mutation changes the
        # DESTINATION and nothing else. Idempotency-safe: the edit consumes the
        # pattern, so a second apply matches nothing and cmd_apply reports
        # BROKEN FIXTURE.
        sed -i 's|\trc = j->ops->respond(j->ops->ctx, j->scratch, VMS_CM_BODY_LEN);|\trc = j->jops->send_msg(j->jops->ctx, j->cm_conid, j->scratch,\n\t\t\t       VMS_CM_BODY_LEN); /* NEGCTL join-reply-to-the-join-target: every answer leaves on the join target'"'"'s connection, whoever asked */|' "$_file";;

    coord-removal-open-gate-disarmed)
        sed -i 's|if (!coord_open_is_grounded_for(c, subject_csb, 0)) {|if (0 \&\& !coord_open_is_grounded_for(c, subject_csb, 0)) { /* NEGCTL coord-removal-open-gate-disarmed */|' "$_file";;

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

        # rd vms-f297: the tree is copied ONCE (below the loop's first pass)
        # and each defect gets back pristine copies of exactly the files it
        # edits -- every target is a kernel-core file. Re-copying the whole of
        # kernel-core per defect made this self-test run ~20 s against its
        # 30 s ctest TIMEOUT and time out under a parallel ctest.
        if [ ! -d "$_st_tmp/tree/kernel-core" ]; then
            mkdir -p "$_st_tmp/tree"
            if ! cp -a "$_st_root/kernel-core" "$_st_tmp/tree/" 2>/dev/null; then
                echo "FAIL: cannot copy $_st_root/kernel-core for the self-test"
                rm -rf "$_st_tmp"
                return 2
            fi
        fi
        for _st_t in $(defect_field "$_st_d" targets); do
            cp -p "$_st_root/$_st_t" "$_st_tmp/tree/$_st_t" 2>/dev/null || {
                echo "FAIL: $_st_d: target $_st_t is not a file under $_st_root"
                _st_rc=1
            }
        done

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
            # flattened once per suite, not once per defect naming it
            [ -n "${_st_flatdir:-}" ] || _st_flatdir=$(mktemp -d)
            _st_flatf="$_st_flatdir/flat-$_st_suite_glob"
            [ -f "$_st_flatf" ] ||
                tr -d '"\\' <"$_st_src" | tr '\n\t' '  ' | tr -s ' ' >"$_st_flatf"
            _st_flat=$(cat "$_st_flatf")
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
    [ -n "${_st_flatdir:-}" ] && rm -rf "$_st_flatdir"

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
