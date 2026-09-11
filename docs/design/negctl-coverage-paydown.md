# negctl coverage paydown — determination and plan (rd vms-050c, vms-4b4a)

**Status: DESIGN ONLY.** No manifest edit, no defect authored, no QEMU run.
Everything below is grounded in `origin/main` @ `884c3336` (the SHA whose CI run
34267503252 shard 2 is red). *The local `main` checkout was 49 commits stale when
this was written; every source citation is from a `git archive origin/main`
extract, never the worktree.*

The failure list was **reproduced locally**, not copied from the CI log:

```
$ sh tests/qemu/facility_defects.sh coverage src tests/qemu     # origin/main tree
FAIL: executive translation unit(s) with NO negative control: ... (38)
FAIL: derived suite(s) NAMED BY NO defect's suites_red: ... (18)
FAIL: anchor(s) that disagree with the manifest:            (3 lines)
FAIL: defect(s) with NO /* negctl: ... */ anchor:           (2)
FAIL: in-scope suite(s) with NO anchor at all:              (18)
PASS: 130 defect(s) >= floor 110
```

---

## 0. Two findings that change the shape of the fix

### 0.1 `SCOPE_OUT_UNIT_DIRS` **cannot** scope out a `kernel-core` TU today

`cmd_coverage` section 1 (facility_defects.sh:7473-7493) walks
`$root/kernel/*.c` and `$root/kernel-core/*.c` and tests membership in
`$_all_targets` **and nothing else**. `SCOPE_OUT_UNIT_DIRS` is consulted only by
section 3 (the consistency check, :7497) and by the printed `SCOPE:` line
(:7865). It is a list of *directories under `src/`*, and `src/kernel-core` is
already scanned by section 1 unconditionally.

Consequence: **there is no honest way to scope out an individual executive TU
without a machinery change.** The only ways to make section 1 pass today are
(a) name the TU in some defect's `targets`, or (b) move the file out of the
`kernel-core/*.c` glob (e.g. into `src/kernel-core/cluster/`) — which is
evasion, not a declaration, and would silently un-scan the file forever.

**Required machinery change if any scope-out is approved:** add a per-file
`SCOPE_OUT_UNITS` (paths relative to the `src/` root), consult it in section 1,
extend section 3's "declared out of scope but named by a defect" check to it,
and make `scope_out_why` mandatory and printed. That edit is itself part of the
implementation dispatch, not a freebie.

### 0.2 The cluster stack is architecturally outside this gate's universe — but it is **not** untested

`facility_defects.sh`'s premise (header, "WHAT A 'FACILITY' IS HERE") is *the
ioctl groups `src/kernel/vms_module.c` dispatches*. That premise held while
`kernel-core` contained only ioctl-backed facilities (`vms_access.c`,
`vms_ast.c`, `vms_ef.c`, `vms_devtab.c`, …). #1052 landed 37 cluster TUs that
are, **by design** (`docs/design-faithful-cluster-executive.md` §3.9), pure
`[state][event]` FSMs and codecs with injected ops and no ioctl surface — their
prescribed rung is R1 host-unit, and they have one: **54 host suites** under
`tests/cluster/host/` (`test_codec_*`, `test_cnxman_*`, `test_pe_*`,
`test_scs_*`, `test_mscp_*`, `test_dlm_*`), plus the R2 simulator under
`tests/cluster/sim/`.

So the honest framing is *not* "37 facades with no test". It is: **this gate's
universe (a single-node, `VAXCLUSTER=0` QEMU guest reached through `/dev/vms`)
cannot see most of them**, and the R1 ladder that can has **no mutation gate of
its own** — nothing proves those 54 suites can go red.

That is the real debt. Neither an allowlist nor a floor bump touches it.

---

## 1. Per-TU determination (all 38)

Reachability verdicts are grounded in the actual dispatch path on the node this
gate boots: `vms_dev_ioctl` (`src/kernel/vms_module.c:1940-2100`) →
`vms_ioctl_cluster_*` (`src/kernel-core/vms_devtab.c:1820-2465`) → the
kernel-core entry points. On that node `cl->pe`, `cl->scs` and `cl->cnxman` are
all `NULL` and `cl->state == VMS_CLUSTER_OFF`
(`test_syssvc_cluster_negctl.c` header states, and the harness confirms,
"VAXCLUSTER is 0 … the connection manager therefore never started").

### Bucket A — REAL-DEFECT, reachable from an EXISTING tests/qemu suite (8 TUs)

| TU | Owning suite | Verdict | Injectable defect (what to break) | `require_fail` text |
|---|---|---|---|---|
| `kernel-core/vms_pe.c` | `test_kmod_cluster_vc_diag` | REAL-DEFECT | `vms_pe_vc_snapshot()` (`vms_pe.c:814`): the `cl->pe == NULL` guard returns `SS__NORMAL` instead of `SS__NOSUCHDEV` → a VC slot the executive does not hold is reported as a live circuit. `memset(out,0)` precedes the guard, so the row stays zero and only the *status* lies — exactly the INV-6 fabrication class. | `row VC, index far past any table: SS$_NOSUCHDEV, not a crash` |
| `kernel-core/vms_scs.c` | `test_kmod_cluster_conn_diag` | REAL-DEFECT | `vms_scs_cdt_snapshot()` (`vms_scs.c:738`): same one-token flip of the `cl->scs == NULL` guard → every CDL index answers `SS$_NORMAL`, i.e. a placeholder connection. | `row CDT, index far past any CDL: SS$_NOSUCHDEV, not a crash` (+ `knock_on`: `every projected CDT row carries a real minted Local Con.ID, never 0 (the wire's own 'not bound yet' value)` — the walk now sees 256 "live" rows with `local_conid == 0`) |
| `kernel-core/vms_cnxman.c` | `test_kmod_cluster_membership_diag` | REAL-DEFECT | `cnxman_get_csb()` (`vms_cnxman.c:1997`): flip the `cl->cnxman == NULL` guard to `SS__NORMAL` → an index past the high-water mark answers as a member row. `cnxman_get_club()` is untouched, so the CLUB assertions stay green (minimality). | `an index far past the high-water mark refuses (SS$_NOSUCHDEV), never a wrapped/aliased row` |
| `kernel-core/vms_cluster_api.c` | `test_syssvc_cluster_negctl` | REAL-DEFECT | `cluster_api_getsyi_project()` (`vms_cluster_api.c:72`): drop the `if (club->local_csid_valid)` guard so `node_csid_valid` is asserted with no learned CSID. This is integration-note E30's exact fabrication ("0 means none assigned, never node zero"). Called **unconditionally** by `vms_ioctl_cluster_getsyi` even with `cl->cnxman == NULL` — verified. | `... node_csid_valid CLEAR -- the cluster assigned no CSID, and 0 means 'none assigned', never 'node zero'` |
| `kernel-core/vms_l2.c` | `test_syssvc_l2_datalink` | REAL-DEFECT | `l2_priv_check()` (`vms_l2.c:116`): `bool ok = (cur_privs & VMS_PRV_M_PHY_IO) != 0;` → `bool ok = true;` The executive stops gating the raw SCS datalink on real PHY_IO. | `L2_OPEN without PHY_IO -> SS$_NOPRIV` (+ `knock_on`: `L2_OPEN without PHY_IO mints no handle`) |
| `kernel-core/vms_cluster_fork.c` | `test_kmod_cluster_fork_hammer` | REAL-DEFECT | `cf_deliver_work()` (`vms_cluster_fork.c:1066`): drop `f->st.work_dispatched++`. The executive's OWN drain-convergence rule (`work_dispatched >= work_posted`) then never converges. Precedent for a value-not-decision mutation: the three `kstat-*-mismapped` entries. | `every posted work item was dispatched exactly once (no lost wakeup, no stuck poster)` (+ `knock_on`: `cf_stats converged (dispatched >= enqueued/posted) within the 2s bound -- no lost wakeup`) |
| `kernel-core/vms_cluster_fork_bind.c` | `test_kmod_cluster_fork_hammer` | REAL-DEFECT | `vms_cluster_fork_worker_start()` (`vms_cluster_fork_bind.c:402`): return `SS__NORMAL` **without** spawning the FC-P6.6 worker kthread — the "reports success while doing nothing" class. `WORKER_START=1`, `IO_HANDLER_CALLS=0`. | `the WORKER kthread really ran the blocking I/O callback (this is where exec_blockdev_read_block sits on a served unit)` (+ `knock_on`: the `WORK_DURING_IO > 0` assertion) |
| `kernel/vms_bg_pollfd.c` | `test_syssvc_bgsock_poll` | REAL-DEFECT | `vms_bg_pollfd_poll()` (`vms_bg_pollfd.c:46`): return `EPOLLIN|EPOLLOUT` unconditionally instead of delegating to the host socket's `->poll`. This is the **executive-side** twin of the existing userspace `bgsock-poll-always-ready`; both may name the same assertion, at different layers. | `poll() reports NOT readable before any data arrives (readiness reflects the socket)` |

Also anchorable in Bucket A, and needed to give `test_kmod_cluster_seam` an
anchor (it targets a header, so it adds no TU coverage):

| Target | Suite | Defect | `require_fail` |
|---|---|---|---|
| `kernel/exec_kbackend_linux.h` | `test_kmod_cluster_seam` | `lan-open-leaves-link-down`: `exec_lan_open` (`:930`) `if (dev->flags & IFF_UP)` → `if (1)`, skipping `dev_change_flags`. This is the *real* shipped bug (vms-fc-e51): TX reports success over a down interface. | `the transmitted frame's payload is byte-exact on the peer` (+ `knock_on`: `exec_lan_xmit from vseam0 was captured on the peer vseam1`) |

> **Anchor-text trap, measured while writing this.** The anchor check greps the
> **source** segment after the anchor for a `require_fail` string, but the driver
> compares against the **runtime** `"  FAIL: …"` text. `test_kmod_cluster_seam.c`
> builds several messages by macro concatenation (`"exec_lan_xmit from " IF_A
> " was captured on the peer " IF_B`), so the source reads `IF_A` where the
> runtime prints `vseam0`. **Anchor only above macro-free assertions**; put any
> concatenated assertion in `knock_on_fail` (spelled as it prints).

### Bucket B — REAL-DEFECT after ONE small new suite (1 TU)

| TU | New suite | Verdict | Defect |
|---|---|---|---|
| `kernel-core/vms_cluster_sysgen.c` | **new** `tests/qemu/test_syssvc_cluster_sysgen.c` (~130 lines: raw `VMS_IOCTL_SYSGEN_LOAD`, honest skip-77) | REAL-DEFECT | `cluster_sysgen_params_valid()` (`vms_cluster_sysgen.c:14`): drop the `vaxcluster >= 1 && scsnode_len == 0` refusal → the executive commits a cluster identity with no SCSNODE and answers `SS$_NORMAL`. Suite asserts: `VAXCLUSTER=2` + empty SCSNODE → `SS$_BADPARAM` **and** `params_valid` unchanged; with SCSNODE → `SS$_NORMAL`. Today only `tests/qemu/test_sysboot_cluster_params_e2e.sh` covers this, and `.sh` files are outside both the coverage universe and the driver. |

### Bucket C — REAL-DEFECT after the ONE-NODE CLUSTER BRING-UP harness (11 TUs)

**The harness (`FC-negctl-R3`, one new item):** a `OVMX_KTEST_CLUSTER_START`
module knob + `tests/qemu/test_kmod_cluster_start.c`, modelled *exactly* on the
two shipped precedents (`OVMX_KTEST_CLUSTER_SEAM` /
`vms_ktest_cluster_fork_hammer_run`, `src/kernel/vms_module.c:308` and `:799`).
It creates a veth pair by raw rtnetlink inside the guest, `SYSGEN_LOAD`s
`VAXCLUSTER=2` + a real `SCSNODE`, issues `CLUSTER_START`, captures the node's
own HELLO on the peer veth end via `AF_PACKET`, and reads `CLUSTER_DIAG_CONN /
_CSB / _JOIN` back. Nothing is simulated: this is a genuine one-node cluster
start on a real executive.

Grounded in `vms_ioctl_cluster_start` (`vms_devtab.c:2434`) and
`vms_cnxman_start` (`vms_cnxman.c:1828-1935`), that path really runs:

| TU | Reached at CLUSTER_START by | Observable as | Verdict |
|---|---|---|---|
| `vms_pe_fsm.c` | `vms_pe_start()` → port FSM + HELLO cadence; `pe_fsm_view_project` | `DIAG_PORT` row + HELLO on the veth peer | REAL-DEFECT |
| `vms_cluster_codec.c` | HELLO build (the only TU that knows a wire offset) | HELLO bytes on the peer | REAL-DEFECT |
| `vms_cluster_codec_hello.c` | `vms_hello_build` | HELLO bytes on the peer | REAL-DEFECT |
| `vms_cluster_emit_guard.c` | emit-time drop/warn table (note E82) | a mutated threshold either drops a correct HELLO (no frame captured) or admits an unsafe one | REAL-DEFECT |
| `vms_scs_fsm.c` | `vms_scs_start()` registers `SCS$DIRECTORY`, **minting a Con.ID** | `DIAG_CONN` row SCS: `n_sysaps >= 1`, `local_conid != 0` (`SCS_ERR_NOCONID` refusal is the real behaviour) | REAL-DEFECT |
| `vms_scs_dir.c` | the one SYSAP registry the listen registers into | the CDL row's Local SYSAP renders `SCS$DIRECTORY` in `LISTEN` | REAL-DEFECT |
| `vms_cluster_codec_scs.c` | `scs_sysap_listen` name/format encoding | the CDT row's SYSAP name + the connect verb on the wire | REAL-DEFECT |
| `vms_cnxman_csb.c` | `cnxman_club_init()` + the local CSB | `DIAG_CSB` row CLUB/CSB; `CLUSTER_MEMBER_GET` renders `csb_member_state_name` | REAL-DEFECT |
| `vms_cnxman_join_fsm.c` | `cnxman_join_init` + `cnxman_start_join_or_wait` | `DIAG_JOIN` ring: `join_state`, `ignored_events` | REAL-DEFECT |
| `vms_cnxman_diag.c` | `cnxman_diag_init(cn->diag, 1)`; every join transition | `DIAG_JOIN` `n_rows`/`recorded`/coalescing (`repeat`) | REAL-DEFECT |
| `vms_cnxman_quorum.c` | **CONTINGENT** — `cnxman_quorum_recompute()` was *not* observed on the start path in `vms_cnxman_start`; it is called on membership change. Verify at implementation time whether the local-CSB seed recomputes CEVOTES/QUORUM on a single node. If it does not, this TU moves to Bucket D. | `DIAG_CSB` row CLUB `quorum`/`cevotes`/`expected_votes`; `$GETSYI` CLUSTER_QUORUM/VOTES | REAL-DEFECT (contingent) |

`vms_cnxman_recnx_fsm.c` is also *initialised* on this path
(`cnxman_recnx_start`, `vms_cnxman.c:1932`) but has **no readback surface** —
kept in Bucket D until it gets one (a diag row) or a peer.

### Bucket D — needs a PEER that speaks SCA back at the node (8 TUs)

These implement genuine executive decisions but cannot be driven without a
second party. The harness that reaches them (`FC-negctl-R4`) is
`test_kmod_cluster_start.c` grown a **wire responder**: the test program already
holds the far end of the veth and already injects raw 0x6007 frames
(`test_kmod_cluster_seam.c` proves the mechanism), so it can answer the node's
HELLO, form a VC, and drive a CM open/commit round. This is the R2 simulator's
job executed inside the guest.

| TU | Decision it owns | Verdict |
|---|---|---|
| `vms_cluster_codec_vc.c` | VC formation + sequenced-message envelope | REAL-DEFECT (needs peer) |
| `vms_cluster_codec_cm.c` | the 132-byte CM SYSAP body; the **length gate** that replaced the ethertype gate (E73 — the gate that lost VAX2's op-0x03 COMMIT) | REAL-DEFECT (needs peer) |
| `vms_cluster_codec_blk.c` | SCA block-transfer header, two call sites sharing one layout | REAL-DEFECT (needs peer) |
| `vms_cnxman_barrier_fsm.c` | barrier step/release | REAL-DEFECT (needs peer) |
| `vms_cnxman_coord_fsm.c` | coordinator election/role | REAL-DEFECT (needs peer) |
| `vms_cnxman_phase2.c` | the p. 7-42 PHASE 2 COMMIT, one impl two callers | REAL-DEFECT (needs peer) |
| `vms_cnxman_recnx_fsm.c` | reconnect window (p. 7-30) | REAL-DEFECT (needs peer) |
| `vms_dlm_ldwv.c` | Lock Directory Weight Vector — **the hash is never computed here** (Rule 8); the decision is the weight walk | REAL-DEFECT (needs peer) |

### Bucket E — needs a peer that SERVES or CONSUMES (MSCP / DLM arms) (10 TUs)

| TU | Verdict |
|---|---|
| `vms_cluster_codec_dlm.c` | REAL-DEFECT (needs DLM peer). Note the **lock-id refusal** (the `fc8540ae` hard lesson) is precisely the assertion worth reddening — it now lives in `vms_dlm_deq_build()` / `vms_dlm_blkast_build()` and on the PARSE side too, `vms_dlm_completion_build()` having been retired by the vms-c03 supersession (the completion/commit pair was a phantom). |
| `vms_dlm_scs_fsm.c` | REAL-DEFECT (needs DLM peer) |
| `vms_cluster_codec_mscp.c` | REAL-DEFECT (needs MSCP peer) |
| `vms_mscp_cl.c`, `vms_mscp_cl_conn_fsm.c`, `vms_mscp_cl_fsm.c`, `vms_mscp_cl_io_fsm.c` | REAL-DEFECT (needs a serving peer) |
| `vms_mscp_srv.c`, `vms_mscp_srv_fsm.c`, `vms_mscp_srv_io.c` | REAL-DEFECT (needs a consuming peer + a mounted volume) |

**Count check:** 8 (A) + 1 (B) + 11 (C) + 8 (D) + 10 (E) = **38**. ✔

### There is no HONEST-SCOPE-OUT row in this table

Every one of the 38 implements an executive decision whose failure mode is the
fabrication class INV-6 exists to kill — the "pure serialization" codecs
included: `vms_cluster_codec_dlm.c` *refuses* to emit a lock id it does not
hold; `vms_cluster_emit_guard.c` *drops* (never clamps) an unsafe frame;
`vms_cluster_codec_cm.c`'s length gate decides whether a real COMMIT is seen at
all. Calling any of them "no independently-injectable executive behavior" would
be false, and a scope-out justified by a false statement is the exact dodge
vms-050c forbids.

---

## 2. The 18 unanchored suites → which defect's `suites_red` each joins

| Suite | Defect (N = new, E = existing, widened) | Target | Anchor goes above |
|---|---|---|---|
| `test_kmod_cluster_conn_diag` | N `scs-cdt-snapshot-fabricates-connection` | `kernel-core/vms_scs.c` | `suite_cdt_negctl`'s `row CDT, index far past any CDL…` CHECK (`:219`) |
| `test_kmod_cluster_membership_diag` | N `cnxman-csb-snapshot-fabricates-member` | `kernel-core/vms_cnxman.c` | `an index far past the high-water mark refuses…` (`:172`) |
| `test_kmod_cluster_vc_diag` | N `pe-vc-snapshot-fabricates-circuit` | `kernel-core/vms_pe.c` | `row VC, index far past any table…` (`:158`) |
| `test_kmod_cluster_seam` | N `lan-open-leaves-link-down` | `kernel/exec_kbackend_linux.h` | `the transmitted frame's payload is byte-exact on the peer` (`:350`) — **not** the `IF_A`/`IF_B` line |
| `test_kmod_cluster_fork_hammer` | N `fork-work-dispatch-uncounted` (+ N `fork-worker-start-reports-success-unstarted`) | `kernel-core/vms_cluster_fork.c`, `…_fork_bind.c` | `:334` and `:374` respectively |
| `test_syssvc_cluster_negctl` | N `getsyi-csid-reported-without-valid` | `kernel-core/vms_cluster_api.c` | the `node_csid_valid CLEAR` CHECK in `check_getsyi_ioctl()` |
| `test_syssvc_l2_datalink` | N `l2-open-bypasses-phy-io` | `kernel-core/vms_l2.c` | `L2_OPEN without PHY_IO -> SS$_NOPRIV` |
| `test_kmod_errcnt` | N `devtab-io-error-not-charged` | `kernel-core/vms_devtab.c` (`vms_devtab_note_io_error`) — TU already covered, this is a suite anchor | `one genuine ACP block-read failure incremented VDA100: ERRCNT by exactly one` |
| `test_kmod_exit` | N `setexit-status-not-recorded` | `kernel-core/vms_proctab.c:1338` (`vms_ioctl_setexit`) | `B reads back the EXACT condition value A recorded` |
| `test_kmod_spawn_notify` | N `spawn-notify-flag-not-set` | `kernel-core/vms_proctab.c:1256` (`vms_ioctl_spawn_notify`) | `the executive SET the parent's completion event flag` |
| `test_syssvc_bg_fork_inherit` | **E** `fork-inherit-disabled` — widen `suites_red` from `test_syssvc_bg_fork_close_inherit` to add this suite, and add its assertion to `require_fail` | `kernel/vms_bg_forkinherit.c` | `a FORKED child can operate the BG channel its parent created (executive fork-inheritance)` |
| `test_syssvc_creprc_inherit` | N `register-subprocess-identity-self-declared` | `kernel/vms_module.c` (`VMS_IOCTL_REGISTER_SUBPROCESS`, `:1954`) | `A: the subprocess INHERITED the creator's user name (SYSTEM) -- a readback, not a self-declaration` |
| `test_syssvc_libspawn_reg` | N `libspawn-prcnam-dropped` | `libvms/syssvc/sys_process.c` | `the lib$spawn'd subprocess is EXECUTIVE-REGISTERED (resolvable BY prcnam)` |
| `test_syssvc_spawn_pipeline` | **E** `setexit-status-not-recorded` (widen) | as above | `the failing stage's actual $STATUS (SS$_ABORT) is surfaced` |
| `test_syssvc_crtl_rms_veneer` | N `crtl-fwrite-bypasses-rms` | `vmsrms/crtl_rms_stdio.c` | `2a: sys$search VENEER.DAT;* finds exactly the veneer-written file (independent ACP reader -- ramfs cannot produce this)` |
| `test_syssvc_rms_filelock` | N `rms-open-no-file-access-enq` | `vmsrms/rms_core.c` | `open#1 holds a real file-access lkid` |
| `test_syssvc_rms_reclock` | N `rms-record-lock-not-enqueued` | `vmsrms/rms_core.c` (record arm) | `rab1 stashed a real record lkid` (+ `knock_on` `rab2 stashed NO lock on the RMS$_RLK conflict`) |
| `test_syssvc_rms_workload` | N `rms-create-supersedes-version` | `vmsrms/rms_core.c` (`sys$create` versioning) | `A4: sys$open WKOBJ.OBJ;1 still reads the ;1 payload 'V1' -- both versions COEXIST (the VMS-versioning teeth a POSIX overwrite cannot fake)` |

Two of the eighteen (`bg_fork_inherit`, `spawn_pipeline`) join an existing
defect. Widening is legitimate **only** if the suite really detects that defect;
both must be MEASURED under injection, not asserted here — and the manifest's
own rule ("keep `suites_red` as narrow as the measurement") means a widening
that does not redden gets reverted rather than kept.

---

## 3. Scope-out proposal

**Recommended: NONE. Do not add anything to `SCOPE_OUT_UNIT_DIRS` or
`SCOPE_OUT_SUITES`.** §1 shows no TU qualifies, and §0.1 shows the machinery
cannot express a per-file exclusion anyway.

That leaves the gate **red on main until Buckets A–E all land** — the coverage
check is all-or-nothing, so partial paydown does not turn it green.

If the operator decides main must be green before Buckets D/E land, the **only**
honest form is a per-file, dated, rd-tracked `SCOPE_OUT_UNITS` whose printed
reason states the truth and nothing better:

```
SCOPE_OUT_UNITS = kernel-core/vms_cluster_codec_vc.c kernel-core/vms_cluster_codec_cm.c \
                  kernel-core/vms_cluster_codec_blk.c kernel-core/vms_cluster_codec_dlm.c \
                  kernel-core/vms_cluster_codec_mscp.c kernel-core/vms_cnxman_barrier_fsm.c \
                  kernel-core/vms_cnxman_coord_fsm.c kernel-core/vms_cnxman_phase2.c \
                  kernel-core/vms_cnxman_recnx_fsm.c kernel-core/vms_dlm_ldwv.c \
                  kernel-core/vms_dlm_scs_fsm.c kernel-core/vms_mscp_cl.c \
                  kernel-core/vms_mscp_cl_conn_fsm.c kernel-core/vms_mscp_cl_fsm.c \
                  kernel-core/vms_mscp_cl_io_fsm.c kernel-core/vms_mscp_srv.c \
                  kernel-core/vms_mscp_srv_fsm.c kernel-core/vms_mscp_srv_io.c
```

with `scope_out_why` printing, verbatim:

> These 18 cluster TUs are NOT declared free of injectable executive behaviour —
> every one of them makes a real decision (a class/length gate, a lock-id
> refusal, a quorum walk, an MSCP unit-state transition). They are excluded
> because THIS GATE'S UNIVERSE cannot reach them: it boots ONE node with
> VAXCLUSTER=0, and these TUs execute only against a peer that speaks SCA back.
> Their controls are tracked by rd <FC-negctl-R4/R5 items>. Until those land the
> only tests exercising them are the 54 R1 host-unit suites under
> tests/cluster/host/ and the R2 simulator under tests/cluster/sim/, NEITHER OF
> WHICH HAS A MUTATION GATE — so nothing anywhere proves these TUs' tests can go
> red. This is a disclosed, priced gap, not a claim of coverage.

That text weakens a gate, so **it is an operator decision, not mine**. My
recommendation is to refuse it and pay the debt down instead: the printed line
above is an admission that 18 executive TUs are unproven, and it will read
exactly that way in every CI log until the harness lands.

**Floor:** `facility_defects_floor.txt` currently reads 110 against 130 entries.
Paydown ADDS entries; raise the floor to the new count in the same PR that lands
them (raising a floor is not the tamper direction — lowering is).

---

## 4. Effort estimate (implementation dispatch sizing)

"Proven" = injected in QEMU by `tests/qemu/run_facility_negctl.sh`, red set
observed to EQUAL `require_fail + knock_on_fail` exactly, and the resulting
`facility_negctl_observed.tsv` committed.

| # | Work item | New defects | New suites / harness | New QEMU injection proofs | Notes |
|---|---|---|---|---|---|
| 1 | **Machinery**: `SCOPE_OUT_UNITS` (only if §3 is approved) + `scope_out_why` | 0 | 0 | 0 | ~60 lines in `facility_defects.sh`; `facility_record_negctl.sh` needs a matching negctl |
| 2 | **vms-4b4a**: 2 unanchored bgsock defects + 2 anchor drifts | 0 | 0 | 0 (re-run only) | §5 below; 3 one-line edits |
| 3 | **Bucket A cluster**: pe/scs/cnxman/cluster_api/l2/fork/fork_bind + seam | 8 | 0 | 8 | all reachable on today's harness |
| 4 | **Bucket A bg**: `bg-pollfd-always-ready` | 1 | 0 | 1 | |
| 5 | **Non-cluster suite anchors** (errcnt, exit, spawn_notify, creprc_inherit, libspawn_reg, crtl_rms_veneer, rms_filelock, rms_reclock, rms_workload) | 9 | 0 | 9 | mechanical: assertions already exist |
| 6 | **Widenings** (bg_fork_inherit → `fork-inherit-disabled`; spawn_pipeline → `setexit-…`) | 0 | 0 | 2 re-proofs | must MEASURE the widened red set |
| 7 | **Bucket B**: `test_syssvc_cluster_sysgen.c` | 1 | 1 suite (~130 lines) | 1 | |
| 8 | **Bucket C harness** (`FC-negctl-R3`): `OVMX_KTEST_CLUSTER_START` knob + `test_kmod_cluster_start.c` | 0 | 1 knob + 1 suite (~600 lines) | 0 | the enabling item; modelled on the two shipped ktest knobs |
| 9 | **Bucket C defects**: pe_fsm, codec, codec_hello, emit_guard, scs_fsm, scs_dir, codec_scs, cnxman_csb, join_fsm, cnxman_diag (+ quorum if contingency holds) | 10–11 | 0 | 10–11 | |
| 10 | **Bucket D harness** (`FC-negctl-R4`): grow the suite into a wire responder (HELLO answer → VC → CM open/commit) | 0 | ~800 lines | 0 | R2-in-guest; the largest single piece |
| 11 | **Bucket D defects**: codec_vc, codec_cm, codec_blk, barrier, coord, phase2, recnx, dlm_ldwv | 8 | 0 | 8 | |
| 12 | **Bucket E harness** (`FC-negctl-R5`): peer serves a volume / consumes one + a DLM arm | 0 | ~600 lines | 0 | |
| 13 | **Bucket E defects**: codec_dlm, dlm_scs_fsm, codec_mscp, mscp_cl×4, mscp_srv×3 | 10 | 0 | 10 | |
| | **TOTAL** | **47–48 new defects** | **3 harnesses + 2 suites (~2100 lines of test code)** | **49–50 injection proofs** | |

Grouped by cluster subsystem, the new defects needing QEMU proof are:
**port/PE 3** (`vms_pe.c`, `vms_pe_fsm.c`, seam) · **fork context 2** ·
**codec 6** (`codec`, `_hello`, `_scs`, `_vc`, `_cm`, `_blk`) + **emit guard 1**
· **SCS 3** (`vms_scs.c`, `_fsm`, `_dir`) · **CNXMAN 8**
(`vms_cnxman.c`, `_csb`, `_join_fsm`, `_diag`, `_quorum`, `_barrier_fsm`,
`_coord_fsm`, `_phase2`, `_recnx_fsm` — 9 if quorum is separable) ·
**DLM 3** (`_ldwv`, `_scs_fsm`, `codec_dlm`) · **MSCP 8** ·
**sysgen/api/l2 3** · **non-cluster 10**.

**Dispatch shape I recommend:** items 2+3+4+5+6+7 are one PR (19 defects, 1 new
suite, 21 proofs) and are worth doing regardless of the scope-out decision —
they are real coverage of reachable behaviour. Items 8+9 are a second PR. Items
10+11 and 12+13 are separate epics and should be rd items *before* any scope-out
text is written, so the `scope_out_why` can cite live item IDs rather than a
promise.

---

## 5. vms-4b4a — the 2 unanchored defects + the 3 anchor drifts

All four are one-line fixes. Reproduced verbatim from the coverage run:

```
test_syssvc_bgsock_exec.c:156 anchors 'bgsock-recv-length-zeroed', but that defect's
  suites_red does not match test_syssvc_bgsock_exec …
test_syssvc_bgsock_exec.c:156 anchors 'bgsock-recv-length-zeroed' but the statement
  under it names none of that defect's require_fail/knock_on_fail texts.
test_syssvc_ident.c:1936 anchors 'bind-client-no-register' but the statement under it
  names none of that defect's require_fail/knock_on_fail texts.
FAIL: defect(s) with NO /* negctl: ... */ anchor: bgsock-exec-handle-not-readopted
                                                  bgconn-getname-addr-zeroed
```

### 5.1 `bgsock-exec-handle-not-readopted` (missing anchor) — **and both `bgsock_exec` drifts** are ONE typo

`tests/qemu/test_syssvc_bgsock_exec.c:156` reads:

```c
    /* negctl: bgsock-recv-length-zeroed */
    CHECK(WIFEXITED(cst) && WEXITSTATUS(cst) == 0,
          "a fork()+exec()'d child drove ovmx_send/recv on the INHERITED veneer handle byte-exact (self-describing handle survives exec, vms-0cd)");
```

That CHECK text **is** `bgsock-exec-handle-not-readopted`'s only `require_fail`,
and `bgsock-recv-length-zeroed`'s `suites_red` is `test_syssvc_bgsock_echo`
alone. The anchor carries the wrong defect name.

**Fix (one word, line 156):**
`/* negctl: bgsock-recv-length-zeroed */` → `/* negctl: bgsock-exec-handle-not-readopted */`

This clears three of the five reported failures at once: the missing anchor for
`bgsock-exec-handle-not-readopted`, and both `bgsock-recv-length-zeroed` drift
lines. `bgsock-recv-length-zeroed` keeps its correct anchor at
`test_syssvc_bgsock_echo.c:248`.

### 5.2 `bgconn-getname-addr-zeroed` (missing anchor)

`suites_red` = `test_syssvc_bg_materialize_fd`; its `require_fail` is
`getpeername() on the materialized fd returns the TRUE peer (the real connection
endpoint from the executive socket), not a synthesized value`, which is the
CHECK spanning `tests/qemu/test_syssvc_bg_materialize_fd.c:217-222`.

**Fix:** insert, immediately above line 217 (inside the `if` block, above
`CHECK(gr == 0 && pn.sin_family == AF_INET && …`):

```c
        /* negctl: bgconn-getname-addr-zeroed */
```

Note the sibling `bg-materialize-fd-not-routed` names the same suite and must
keep its own anchor elsewhere in the file — two anchors in one suite is normal
and the checker handles it.

### 5.3 `bind-client-no-register` drift at `test_syssvc_ident.c:1936`

The anchor is a `negctl-knockon:` above:

```
"F: F$GETJPI CURPRIV renders SYSTEM/ALL's actual enforced privilege names
 (CMKRNL,CMEXEC,SYSNAM,GRPNAM,SETPRV,WORLD,MOUNT,PHY_IO), not merely completes
 without rendering anything"
```

The manifest's `knock_on_fail` still carries the **pre-vms-7eb** spelling
(`…,WORLD,MOUNT)` — no `PHY_IO`). vms-7eb added `PRV$M_PHY_IO` to
`VMS_PRV_M_ENFORCED` (because `vms_l2.c` now genuinely gates on it) and updated
the suite but not the manifest.

**Fix:** in `facility_defects.sh`, `bind-client-no-register`'s `knock_on_fail`
heredoc, replace `…,SETPRV,WORLD,MOUNT), not merely…` with
`…,SETPRV,WORLD,MOUNT,PHY_IO), not merely…`. This is a **manifest** edit, not a
suite edit — the suite is right and the manifest is stale. (It also has to be
re-proven: the assertion text is part of the driver's exact-equality set.)

### 5.4 `kernel/vms_bg_pollfd.c` (the TU half of vms-4b4a)

Covered in §1 Bucket A: new defect `bg-pollfd-always-ready`, target
`kernel/vms_bg_pollfd.c`, suite `test_syssvc_bgsock_poll`, `require_fail`
`poll() reports NOT readable before any data arrives (readiness reflects the
socket)`. It is the executive-side twin of the existing userspace
`bgsock-poll-always-ready`; both may name that assertion, at different layers,
and each must be proven separately.

---

## 6. Open questions for the orchestrator / architect

1. **Scope-out or stay red?** §3 recommends staying red and paying it down. The
   alternative weakens a gate and is operator-reserved.
2. **`vms_cnxman_quorum.c` contingency** (§1 Bucket C): does `cnxman_start` on a
   single VAXCLUSTER=2 node recompute quorum from the local CSB seed? Must be
   MEASURED, not assumed; the answer moves one TU between buckets.
3. **Is `FC-negctl-R4`'s wire responder the right home for R2?** It duplicates
   what `tests/cluster/sim/` already models. An alternative is to give the R1/R2
   ladder its **own** mutation manifest (the same machinery, a different
   universe) instead of dragging 18 pure TUs into a `/dev/vms` harness. That is
   an architecture call, and it is the higher-leverage one: it would cover all
   of Buckets D and E without a QEMU peer at all.
