# Design — wire the executive clean-cluster-departure path (vms-abd, V0.7 blocker)

Status: OPTION A, ⭐⭐-approved (conductor) + Baron ruled HOLD V0.7 for full-A (2026-09-16).
V0.7 tags only after A lands green + boot-verified (a real VAX ACCEPTS the DISCONNECT_REQ on
a clean OVMX shutdown). The design-first + stop-if-it-doesn't-hold discipline caught three
errors in the first drain spec (in-frame-pump deadlock; 5-10s wedge; env-var-in-kernel) BEFORE
any never-crash code — this doc is the corrected Option A.

## 0. What the impl-attempt (cluster-impl-o5) proved
The regression is DEEPER than "vms_cnxman_stop forgot to disconnect": `vms_cnxman_stop()`,
`vms_scs_stop()`, `vms_pe_stop()` are ALL uncalled; there is NO `VMS_IOCTL_CLUSTER_STOP`, no
DCL/SHUTDOWN path — **no clean-shutdown sequence exists at all**. The executive-resident build
VANISHES on exit where real VMS (and the retired scsd.c) announced departure at both the PE
(last-gasp) and SCS (symmetric DISCONNECT_REQ) layers. So the fix is to WIRE the clean-
departure path, then the disconnect rides it.

## 1. The fix = a real clean-cluster-departure path
### 1a. NEW `VMS_IOCTL_CLUSTER_STOP` (symmetric to `vms_ioctl_cluster_start`, vms_devtab.c:2985)
Process context, ioctl thread, **fork thread LIVE, fork mutex NOT held** — this is the context
that makes the drain legal (it dodges the in-frame-pump self-deadlock: the only pump,
`cf_dispatch_one`, takes the non-recursive fork mutex itself). This is the missing seam entry a
clean OVMX shutdown calls to depart gracefully. No new ioctl STRUCT (arg-less / reuses a bare
control arg) — #928 clean.

### 1b. The CLUSTER_STOP handler sequence (kill-switch-gated, SYSGEN default-ON)
1. Lower `f->cfg.disconnect_timeout_ms` to ~400ms via the public `scs_fsm_set_cfg()` FOR THE
   DURATION of the departure, so the FSM's OWN dead-peer guard (`h_timer_disconnect`,
   `SCS_TIMER_DISCONNECT`; default 5000ms, TWO expiries = 5s→op6 / 10s→CLOSED) collapses to fire
   op6 INSIDE a ~500ms drain. Fabricates nothing — it's a documented OVMX cfg value
   (vms_scs_fsm.h:538-552). (Needs a one-line glue twin in vms_scs.c: `struct vms_scs` is opaque
   to vms_cnxman.c.)
2. Enumerate the OPEN peer connections from LIVE state: each in-use `club->csb[i].cdt_conid`
   (vms_cnxman.c:381), `cn->cur_conid` (+`cur_conid_valid`), and the join's `mscp_conid`/
   `cm_conid` (+`mscp_open`/`cm_open`, vms_cnxman_join_fsm.h:640-642). All executive-held.
3. For each, `scs_disconnect(cl->scs, conid, 0u)` — REUSE the grounded path
   (scs_disconnect → scs_fsm_disconnect → SCS_EV_LOCAL_DISCONNECT → h_local_disconnect → 8→9→6).
   NO new encoder; every DISCONNECT_REQ field comes from the live CDT/FSM (⭐⭐).
   IDEMPOTENCE IS FREE: `SCS_EV_LOCAL_DISCONNECT` exists in exactly ONE dispatch row
   (`[VMS_SCS_CDT_OPEN]`, vms_scs_fsm.c:1955); a LISTEN/half-open/DISC_* CDT hits a NULL cell,
   bumps `ignored_events`, returns `SCS_ERR_NOTOPEN`. No new predicate, no state-branch on the
   diagnostics-only `scs_cdt_view`.
4. BOUNDED-WAIT DRAIN (~500ms), NOT holding the fork mutex: the LIVE fork thread dispatches the
   op9, releasing `disc_emit_req` → op6 (h_rx_credit_rsp, vms_scs_fsm.c:1626). Loop:
   `fork_enter → read per-conn disc progress (DISC_SENT/CLOSED) → fork_leave → bounded wait`
   until all done or the deadline. Uses the NEW seam primitive (§2). The lowered timeout (step 1)
   guarantees the FSM self-emits op6 within the window even if a peer never answers op9.
5. At the deadline, FORCE-LOCAL-CLOSE any still-open CDT via `scs_fsm_stop()` ("nothing goes on
   the wire", vms_scs.c:699-702) so nothing leaks and shutdown NEVER hangs.
6. Then the existing `vms_cnxman_stop()` teardown: last-gasp (`pe_send_last_gasp`, kept),
   `scs_sysap_unlisten`, timer-cancel, `cnxman_free`. (i.e. CLUSTER_STOP drives 1-5 then calls
   the current vms_cnxman_stop, or 1-5 fold into it — implementer's call, keep the order.)

## 2. NEW bounded-wait seam primitive (both substrates, #928 no ioctl-struct change)
`exec_kbackend.h` has no msleep/delay/yield and only `exec_cv_wait_timeout` (needs a cv CNXMAN
doesn't own). Add a narrow process-context bounded-wait entry — e.g. `cf_wait_idle_ms(fork, ms)`
or an exec-seam `exec_bounded_wait(ms)` — implemented on BOTH the Linux and NetBSD backends. It
must be a plain bounded sleep/yield in process context (fork thread keeps pumping); no new
ioctl struct. This is the one genuinely-new substrate object; keep it minimal + symmetric.

## 3. Kill-switch = SYSGEN param (VMS-faithful), default-ON
A cluster "clean-departure" toggle in the SYSGEN params (not env — kernel code has zero getenv;
not a module param). Default ON (faithful). Pick a VMS-style name (e.g. `CLEAN_DEPART` /
reuse an existing cluster toggle if one fits). It gates step 1b (the new disconnect drain);
off ⇒ the old behavior (unlisten + free, vanish) — the wire-visible-change kill-switch.

## 4. Twin-test (crash guard — TWO-PROOF; vms_cnxman.c is NOT host-linkable)
Per tests/cluster/host/CMakeLists.txt:263-271, drive the real `scs_fsm` through
`scs_test_harness.h` for the ALGORITHM, then SOURCE-SCAN the shipped vms_cnxman.c / the
CLUSTER_STOP handler for the WIRING (OVMX_KCORE_DIR). Port the `t_eight_before_disconnect`
assertions (test_scs_fsm_credit.c:352-408).
- HAPPY: shutdown/depart with ≥1 OPEN peer conn → op8 then (inject op9) op6 PER conn,
  byte-shaped like the confirmed h_local_disconnect path.
- DEAD-PEER (mandatory never-crash): op8 sent, op9 NEVER injected → with the ~400ms
  disconnect_timeout the FSM's own guard emits op6 + CLOSED inside the drain → NO hang, NO
  crash, counters moved.
- KILL-SWITCH (SYSGEN off): ZERO op8/op6 on depart.
- IDEMPOTENCE: a listening/half-open/DISC_* CDT is NOT disconnected (hits the NULL cell).
- NON-VACUITY: `credit_msgs_sent`/`disc_sent` moved.

## 5. Constraints (locked)
- ⭐⭐: every DISCONNECT_REQ field from live CDT/FSM via the existing scs_disconnect path;
  nothing templated. Reuse, no new encoder.
- DON'T REGRESS: the join-FSM disconnect (cnxman_jop_disconnect) + the PE last-gasp are
  untouched; this ADDS the shutdown-time per-connection disconnect on the new CLUSTER_STOP path.
- CLEAN-vs-CRASH free: only CLUSTER_STOP runs this; a crash closes /dev/vms → vms_proc_free
  WITHOUT CLUSTER_STOP → no disconnect → peer times out (correct; a crashed node must not fake
  a graceful depart).
- #928: the bounded-wait primitive is added on BOTH substrates; no ioctl-struct change; verify
  the NetBSD cross-compile of the shared executive core is green.

## 6. Verify (after twin-test green + merged)
Rebuild artifacts with the fix: `gh workflow run build-boot-artifacts.yml --ref <merge sha>` →
`gh run download` → restage to /data/training/vax/cluster/ovmx-boot-artifacts/<sha>/. Join
vaxlab-4, drive a clean CLUSTER_STOP depart → assert the wire shows a real DISCONNECT_REQ AND
the VAX ACCEPTS it (msgtype-7 answer, NOT "%PEA0 Inappropriate SCA"). That closes the V0.7
fidelity gate → cut V0.7.
