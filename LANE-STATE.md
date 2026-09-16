# LANE-STATE — Seat B: Reference & Design Docs cleanup (rd vms-ff74)

Worktree: `.worktrees/cleanup-docs-ref` · Branch: `work/vms-ff74-cleanup-docs-ref` (off origin/main @ d60633a6).
Scope: `docs/design-*.md`, `docs/audit-*.md`, `docs/api-*.md`, `docs/*-spec.md`, `docs/eval-*.md`,
`docs/research-*.md`, `docs/conformance-gap-report.md`, `docs/dcl-verb-fidelity-scoreboard.md`,
`docs/qualifier-audit.md`. (README/index.md/roadmaps/release-notes/product-vision/getting-started/
building = Seat C, NOT touched.)

Ground truth reconciled against: rendered compat register (`docs/compat/`, `build/compat-surface.json`,
65 facilities / 461 items) and the code on this branch (== origin/main). Full inventory built by
6 parallel read-only passes (executive/boot, cluster/DLM, GCC/self-host, networking/openssh,
RMS/ODS2/DCL, program/meta).

Operator hard rules honored: positive descriptions only (never "no longer / does not / not
implemented"); if a doc's whole content is false → delete, don't negate. No new index/ledger — the
compat register is the one capability ledger. Consolidate INTO existing canonical docs.

---

## DONE (Wave 1 — this PR)

| Action | Doc | Rationale (verified) |
|---|---|---|
| CORRECT | `docs/api-rtl.md` | `lib$tparse` documented as "Stub / Returns SS$_UNSUPPORTED"; it is implemented (real backtracking FSM, `src/libvms/rtl/lib_tparse.c`, `lib.yaml` status=implemented/real, closed vms-9f6). Rewrote to a positive description of the parser + real return codes. |
| CORRECT | `docs/design-imgact-vms-activation-context.md` | Status line read "DESIGN (no code)"; vms-f60d landed and was proven e2e on real `/dev/vms` (commit 66c4c1c5, PR #807). Updated status to LANDED. |
| CORRECT | `docs/design-image-rundown-resource-classes.md` | Stale path cites — `src/kernel/vms_access.c`/`vms_lnm.c` moved to `src/kernel-core/`. Repointed both citations. |
| DELETE (`git rm`) | `docs/design-booted-cluster-node.md` | Superseded strawman (auto-start `scsd` userspace daemon, retired 2026-09-02); the codebase has no `src/vmsscs`/`scsd.c`; its one durable fact (zero-raw-socket HELLO-over-executive-datalink) is already harvested into `design-faithful-cluster-executive.md`. 0 inbound refs. History preserved in git. |
| CORRECT | `docs/design-self-host-mmk-spine.md` | §1.3/§2/§4 verdict read "CONDITIONAL GO — NO-GO until P1-P5 closed" and "blocked by P2"; all five gaps (CLI$/CLD, real `lib$table_parse`, `sys$filescan`, `sys$setddir`, `lib$get_foreign`) and vms-486 (`parse_tables.mar` hand-port) are landed on `origin/main` (`lib_cli.c`, `lib_tparse.c` — a real 401-line FSM, `sys_filescan.c`, `sys_misc.c`, `lib_output.c`, `tests/libvms/mmk_parse_tables.c` — 683 lines), corroborated by `design-self-host-spine5-mmk-component.md` showing MMK.EXE driving real in-guest TCC/LIBRARIAN/LINK builds through spine #7. Rewrote to GO/landed, kept design rationale as history. (Resolves open question G below — the compat register's `mms-mmk.yaml` disagreement is with the register, not this fix; the code citations here are direct, not register-derived.) |
| CORRECT | `docs/design-openssh-port-ovmx.md` | §7 ("Implementation status") asserted the retired `-DOVMX_VENEER` glue/patch set (`ovmx_ssh_glue.c`, `build-openssh.sh`, `run_ssh_build.sh`) as "Landed"; confirmed none of those paths exist under `third-party/openssh/` on `origin/main` (only `VENDOR-REV`). Replaced §7 with a pointer to `docs/design-openssh-devener-map.md` (the doc that actually carries current status) and `docs/compat/facilities/ssh.yaml`. §§0-6 (original port plan) kept as background. |
| CORRECT | `docs/design-cluster-membership-executive.md` | The only userspace-scsd-era cluster doc in the set without a SUPERSEDED header; it specifies the exact `vms_cluster_member vms_cluster_members[96]` ioctl-populated-by-scsd scheme that `src/kernel-core/vms_cluster.h:25-27` names as "the strawman it replaces" (retired in the 2026-09-02 reset). Added the same SUPERSEDED-but-kept-for-the-NOTMEMBER/NOSUCHDEV-invariant header pattern used on its sibling cluster docs, pointing at `design-faithful-cluster-executive.md`. |
| CORRECT | `docs/conformance-gap-report.md` | Dated 2026-07-27 one-time triage snapshot with no pointer to a maintained source. Added a positive dated-snapshot note pointing at `tests/conformance/corpus_baseline.json`/`run_corpus.sh` for current numbers; kept the §2/§3 root-cause triage (still valid, orthogonal to the compat register). |
| DELETE (`git rm`) | `docs/eval-revert-userspace-ods2.md` | Its whole premise — should OVMX revert the userspace ODS-2 adapter — is moot: `ods2_sysdisk.c`/`vmsfs_volume.c` no longer exist anywhere in `src/`, confirming the revert already happened. The doc's own header ("evaluation only — nothing executed") is now false, so per the positive-only rule this is a delete, not a rewrite. (The §3.4 codec-test follow-up this eval flagged is a separate, narrower question — if unresolved it belongs in an rd item, not preserved by keeping a now-false eval doc alive.) |
| DELETE (`git rm`) | `docs/qualifier-audit.md` | 16-line pure redirect stub ("do not maintain or trust... authoritative source is `docs/compat/facilities/dcl-qualifiers.yaml`"). Zero unique content beyond the pointer, which `docs/design-compat-surface-register.md` already gives more completely. Single-ledger violation with no history value. |
| DELETE (`git rm`) | `docs/design-openssh-sshd-glue-ovmx.md` | Header already said RETIRED/SUPERSEDED (vms-d916, code deleted), but the body still asserted an "Implementation status (LANDED)" section citing `ovmx_sshd_auth.c`/`sshd_auth.c`/`sshd_session.c`/`test_syssvc_ssh_server.c` — confirmed none exist on `origin/main` (only `cred_drop.c`/`ssh_ident.c`/`term_map.c` survive, as the header itself says). Whole design + its "LANDED" claim is false; the surviving-seam summary is already fully captured in the header note and in `design-openssh-devener-map.md`. 0 inbound refs (only `docs/index.md`, Seat C's file — flagged below). |

**Cross-seat flag:** `docs/index.md` (Seat C's file, not touched here) references
both `dcl-verb-fidelity-scoreboard.md` and `qualifier-audit.md`. The latter is
deleted this wave — index.md's "Compatibility & parity tracking" bullet needs
its `qualifier-audit.md` mention dropped. `dcl-verb-fidelity-scoreboard.md` is
NOT deleted (see cluster D below), so no index.md change needed for it yet.

---

## CANONICAL-DOC DECISIONS (per subsystem — for successor waves)

- **Cluster / SCS / CNXMAN:** canonical = `design-faithful-cluster-executive.md` (matches code:
  `vms_cnxman*.c`, `vms_pe*.c`, `vms_dlm_scs*.c`, `vms_mscp_srv*.c`). Wire-protocol reference =
  `cluster-protocol-spec.md` (8146 lines; other docs correctly cite it). Genesis =
  `design-cluster-genesis.md`. Config authoring = `design-cluster-config-authoring.md`.
  `design-cluster-node.md` is KEPT (cited as clean-room provenance by `cluster-protocol-spec.md:36`
  and `clean-room/PROVENANCE.md:74` — do NOT delete).
- **DLM:** `design-dlm-distributed-deadlock.md` (landed feature), `research-dlm-directory-algorithm.md`
  (algorithm/book grounding), `research-alpha-dlm-wire.md` (wire-capture oracle) are three
  complementary docs — keep all.
- **ODS-2 / Files-11:** canonical "how it works now" = `design-files11-acp-executive.md`
  (matches `ods2.yaml`, `vmsfs_acp.c`). `design-ods2-runtime-flip.md` (A1 userspace adapter) is
  self-superseded history.
- **RMS locking:** `design-rms-file-lock.md` and `design-rms-record-lock.md` are genuinely distinct
  granularities — keep both.
- **Image activation:** umbrella = `design-image-activation.md`.
- **VAX boot:** capstone plan = `design-p4-netbsd-vax-boot.md`; strategy/decision upstreams =
  `design-ovmx-netbsd-syskrnl.md`, `design-netbsd-executive-core.md`.
- **GCC-port F2a host surface:** the trio `design-gcc-port-host-surface-{demands,gaps,gate}.md` are
  three distinct measured rungs — keep all.
- **Compat mechanism:** `design-compat-surface-register.md` is the legit design doc for the register
  itself — keep. No competing capability-ledger docs remain (`design-vms-parity-map.md` already
  self-retired its status tables 2026-09-14).
- **Networking:** DECnet canonical = `design-decnet-ovmx.md` + `design-decnet-net-qio-*` pair;
  sockets veneer = `design-bgsockets-veneer-ovmx.md`; OpenSSH de-veneer ladder =
  `design-openssh-devener-map.md`. (`design-openssh-sshd-glue-ovmx.md` was already deleted upstream
  by commit 61998c0a / vms-d916 — the forbidden hand-rolled-shim doc is gone.)

---

## REMAINING CLUSTERS (next waves — NOT done here)

### A. Activation-cluster merge (3 docs, interdependent — do as ONE unit)
- Fold `design-in-process-activation.md` (806 ln) and `design-image-rundown-resource-classes.md`
  (100 ln) into `design-image-activation.md` as sections.
- BLOCKER/care: `design-in-process-activation.md`'s status header AND its whole "Current state"
  body say fork-per-image is what OVMX does today and Option A (in-process) is the 1.0 target — but
  code shows Option A is LIVE (`src/vmsdcl/dcl_cmd_process.c:1805-1850`, vms-68f/vms-db2); fork is
  now the fallback for ineligible images. This is a full-doc rewrite (positive-only), not a header
  patch — a header-only fix would make the doc self-contradict. Verify the current activation path
  in `dcl_cmd_process.c` first, then rewrite coherently.
- `design-image-rundown-resource-classes.md`'s real parent is `design-in-process-activation.md`
  (§A.6.1), so fold it in the same unit.
- `design-imgact-vms-activation-context.md` stays separate (distinct GCC-port-crt0 audience) — its
  status was already corrected in Wave 1.

### B. VAX boot merge
- Fold `design-vms-7b1-netbsd-vax-boot-disk.md` (133 ln, 0 inbound; concrete boot-to-PID1 milestone
  with unique `ovmx_boot_netbsd.c` backend pins) into `design-p4-netbsd-vax-boot.md` (623 ln) as a
  "milestone" section, then `git rm` 7b1. Content is complementary (a rung under P4's plan), not
  contradictory — read P4 fully to place it.
- `design-vms-9f5-device-native-system-disk.md` stays independent (device naming, not boot flow).

### C. GCC-port gap-analysis lineage (needs rd coordination — flag conductor)
- `design-gcc-vms-port-surface-gaps.md` (Aug-22 first analysis) and
  `design-gcc-port-surface-gaps-register.md` (Aug-31 ledger w/ rd cross-refs) are sequential
  snapshots subsumed by the F2a trio. gaps-register duplicates rd's *work-item* ledger role
  (single-ledger). Retiring them cleanly means folding still-open rows into rd items first (rd
  mutations, from repo root) — beyond doc-lane. gaps-register is cited by `design-chf-condition-
  handling.md:3`, `design-decc-bug-compat-architecture.md:5`, `design-gcc-port-host-surface-demands.md:23`
  — those refs must be repointed before deletion. DEFER; recommend conductor decides fold-to-rd.

### D. DCL fidelity trackers (cross-seat: index.md refs — needs Seat C)
- `dcl-verb-fidelity-scoreboard.md` (stale hand-count, self-flags single-ledger violation vs
  `dcl-verbs.yaml`) and `qualifier-audit.md` (16-line retired stub pointing at `dcl-qualifiers.yaml`)
  both duplicate the compat register's job and are DELETE candidates — BUT both are referenced from
  `docs/index.md:45` (Seat C's file, out of my scope). Deleting them dangles Seat C's index.
  ACTION: coordinate with Seat C to drop the index.md refs in the same change, then `git rm` both.
- `design-dcl-fidelity.md` is NOT a duplicate (root-cause/architecture rationale) — keep, but its
  §1 counts (`~34/~13/~5/~2`) are stale; correct/strip and point at `dcl-verbs.yaml`.

### E. Historical-if-redundant (low priority; already self-bannered, not misleading)
- `design-executive-retrofit.md` — completed dispatch plan (EF/AST/privileges wiring all landed;
  vms.ko WIRED). Carries a superseded banner already. Archive-or-delete judgment; low value but not
  misleading. 0 urgency.
- `eval-revert-userspace-ods2.md` — **DONE this wave** (deleted; see DONE table above and open
  question 3's resolution below). The §3.4 codec-test follow-up it flagged is separately tracked,
  not a reason to keep a now-false eval doc alive.

### F. Corrections still owed (verified contradictions, positive-only fixes)
- `design-openssh-port-ovmx.md` — TL;DR server-path status (§0) says ABSENT/hard-prereq; `ssh.yaml`
  records bind/listen/accept landed (vms-698). Refresh the TL;DR table to what's present. (§7 was
  already corrected this wave — this is a separate, still-open §0 fix.)
- `design-tcpip-services-ovmx.md` §6 — names bundled SSH "VMSSSHD.EXE" citing vms-843; the real
  bundled server is the upstream OpenSSH sshd port (vms-9ef/vms-cb0). Repoint positively.
- `design-tcpip-config-persistence.md` — P0–P3 plan has shipped (`tcpip-services$config-plane`
  implemented, `run_tcpip_reboot_e2e.sh`). Mark shipped positively.
- `design-decnet-ovmx.md` — add a dated note citing the vms-cd3 FAL/DAP oracle capture (just landed).
- `api-rtl.md` — needs a fuller re-sweep vs `lib.yaml`/`ots.yaml`/`mth.yaml`/`str.yaml` (only the
  `lib$tparse` contradiction was fixed in Wave 1; doc was not touched by the 2026-09-14 Wave A pass).
- `design-dcl-fidelity.md` §1 counts (see D).
- ~~`design-self-host-mmk-spine.md`~~ — **DONE this wave** (see DONE table above).

---

## OPEN QUESTIONS FOR CONDUCTOR

1. **Compat register vs code disagreement:** `docs/compat/facilities/mms-mmk.yaml`
   (`mms-mmk$mmk-exe`, gap vms-ec70) says MMK's *execution* half is not done — but
   `design-mmk-exec-drive-ovmx.md` + `design-self-host-spine5-mmk-component.md` +
   `tests/qemu/test_syssvc_mmk_build.c` (wired into the `kernel-executive` CI barrier) show MMK
   drives compile→archive→LINK→activate in QEMU, and `design-self-host-mmk-spine.md`'s P1-P5
   verdict was corrected this wave against direct source citations (not the register), so that
   correction did not need to wait on this row. Recommend refreshing `mms-mmk.yaml` separately
   (register maintenance, not doc-lane scope).
2. **`sys-lock.yaml` label:** `sys$getlki` marked `absent` (no userspace wrapper in `sys_lock.c`/
   `starlet.h`) though the kernel ioctl `vms_ioctl_getlki` exists and is used by tests — the RMS
   lock docs' "$GETLKI proof" is proof-via-raw-ioctl, not the documented service. Minor label
   imprecision, flagging only.
3. **Seat C coordination** for D (index.md refs to the two DCL trackers) and for the
   `qualifier-audit.md` deletion this wave (index.md's "Compatibility & parity tracking" bullet
   needs that filename dropped).

## NEXT ACTIONS (successor, in priority order)
1. Cluster A: activation-cluster merge (verify `dcl_cmd_process.c` activation path, rewrite
   `design-in-process-activation.md` positively, fold rundown, `git rm` the two folded docs).
2. Cluster F: the remaining verified positive corrections (openssh-port §0 TL;DR, tcpip-services,
   tcpip-config-persistence, decnet-ovmx dated note, api-rtl full sweep, dcl-fidelity counts).
3. Cluster B: VAX-boot merge (7b1 → p4).
4. Clusters C/D/E: after conductor rulings on the open questions.
