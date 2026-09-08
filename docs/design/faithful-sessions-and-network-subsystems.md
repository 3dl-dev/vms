# Design — Faithful Sessions & Network Subsystems (LOGINOUT consolidation + DECnet NETACP)

**Status:** DRAFT for operator ratification (rd vms-515; roadmap epics vms-43b + vms-ec9).
**Origin:** an operator-caught LARP — a broken `vmsdcl --login` default in the DECnet CTERM
server — exposed that OVMX has no single VMS-authentic login subsystem, and that "decnetd"
is a Linux-daemon-shaped drift from the DECnet charter. Produced by an adversarial-design
forum (adversary / creative / systems-pragmatist / domain-purist, one-shot), grounded in code.

> **This is a design to ratify, not code to land.** All login/DECnet code and PR #1057 stay
> held until the operator ratifies. The gates in §7 are the done-bar; the tell in §7.5 is the
> anti-LARP instrument.

---

## 1. The two findings (both course-corrections, not new directions)

**Finding A — no single login authority.** OpenVMS has exactly one: every login process
(interactive, network, batch) runs **LOGINOUT.EXE** as its first image; LOGINOUT reads SYSUAF,
authenticates, establishes UIC/privileges/quotas, then chains into DCL. OVMX today:
- **Console** — faithful: `JOB_CONTROL → execl(LOGINOUT.EXE) → DCL --login`. LOGINOUT
  (`tools/vms_login.c`) is a real authenticator (SYSUAF + Purdy + DISUSER/DISACNT gate +
  `setident_quota` persona + `setprn` + credential-drop + accounting).
- **SSH** (`src/vmsssh/vmssshd.c`) — **re-implements LOGINOUT inline in C**, weaker (no DISUSER
  gate, plain `setident` without quotas, no process name, restores the `VMS_*` env facade
  LOGINOUT deleted, duplicate banner, root/euid-0 SYSUAF crutch), then execs `DCL --login`
  directly. Bypasses LOGINOUT. (A second, dormant OpenSSH-port stack multiplies the copies.)
- **DECnet SET HOST / CTERM** (`src/vmsdecnet/engine/decnetd.c --cterm-server`) — **no
  authentication at all**: forks + `execvp("vmsdcl","--login")`, never decodes the CTERM
  access-control username/password, stamps no identity, creates no terminal device. A remote
  node reaches `$` unauthenticated. This is a live login LARP (currently a manual dev flag,
  not deployed/auto-started — not a shipped vuln, but must never ship).

The executive already has **one** well-guarded persona primitive (`vms_ioctl_setident`,
"reserved for LOGINOUT", SETPRV-or-narrow-only) plus a SYSTEM-only sibling
(`vms_kif_establish_system`, privilege-gated). The fragmentation is entirely at the **caller**
layer — five independent callers, three of which reimplement or skip the login logic.

**Finding B — DECnet is Linux-daemon-shaped ("decnetd"), not the VMS network subsystem.** Real
VMS DECnet is **NETACP** (a privileged detached process — *not* kernel-resident) + the executive
**device face** (`_NET:`/`EWA0:` reached by `$ASSIGN`/`$QIO`/`$GETDVI`) + **NCP** + a **network-
object dispatch table**. `vms-30e`'s own charter already said "executive device face
(EWA0:/_NET:/QIO)… NOT a vendored Linux daemon" — decnetd drifted from the ratified design.

---

## 2. The faithful model (grounded; clean-room flags marked ⚑)

- **LOGINOUT.EXE is the sole authenticating image.** One SYSUAF-reading, one `setident`-calling
  code path, reached by every access route. `DCL --login` is faithful **iff its parent is
  LOGINOUT** — it is the login-job *handoff* (SYLOGIN/LGICMD/captive/`$`), never a substitute
  for the authenticator.
- **`$CREPRC` is the ONE process-creation primitive; JOB_CONTROL/NETACP are the detached
  creators.** No Linux mechanics above the VMS layer: `fork()`/`execl()`/`openpty()`/`dup2()` are
  hidden *inside* `$CREPRC`'s implementation and the terminal device's backing — never at the
  console/SSH/CTERM caller layer. Every path calls `$CREPRC` "create a process running
  LOGINOUT.EXE bound to terminal-device X". Today the console reaches LOGINOUT by raw
  `fork()+execl()` (a disclosed Linux leak, flagged in `ovmx_job_control.c`) and OVMX's `$CREPRC`
  copies the *creator's* identity — both must change: `$CREPRC` becomes the sole creator for all
  paths (console included), and it must create a process that **LOGINOUT re-personas post-auth**
  (see next bullet).
- **LOGINOUT.EXE is INSTALLED with privilege** (as real VMS installs it) — that is how a process
  running it gains the authority to read SYSUAF and set the persona *regardless of who created
  it*. This is the keystone: the creator (`JOB_CONTROL`/`NETACP`) needs **no** identity-forging
  privilege of its own; LOGINOUT's authority is its own installed image. It dissolves the "network
  daemon must hold `CAP_SYS_ADMIN` to forge identity" attack (§4 A2) and makes "`$CREPRC` a
  process LOGINOUT then re-personas" (real VMS) the faithful, buildable model.
- **The terminal is a real executive device.** `OPA0:` (console), and — absent today —
  **`RTAn:`** (DECnet inbound SET HOST, RTTDRIVER), `TNAn:` (TELNET), `FTAn:` (pty). A network
  login is LOGINOUT bound to one of these devices, not a nameless PTY session.
- **NETACP dispatches by OBJECT NUMBER ⚑** — and *not every object runs LOGINOUT*: object 42
  (CTERM/SET HOST) → LOGINOUT on RTAn:; object 17 (FAL) → FAL.EXE; object 0 (task) → the target
  command file. "Consolidate SET HOST onto LOGINOUT" is correct **only for the interactive-
  terminal objects**. Asserting all objects front LOGINOUT would be an invented internal.
- **The DECnet wire engine may stay userspace** (ruling vms-a1c): Linux removed `AF_DECnet` in
  6.1, so unlike `AF_INET` there is no in-kernel stack to ride — the one case where a userspace
  Phase IV engine is faithful. The AF_PACKET socket is hidden behind the device face, exactly as
  the cluster's `scsd` socket is hidden behind the SCS surface.

⚑ **Clean-room prerequisite (Rule 8, BLOCKING):** the exact inbound-SET-HOST sequence
(NETACP ↔ RTTDRIVER ↔ object-table ↔ LOGINOUT-on-RTAn:) and the CTERM access-control credential
semantics (does LOGINOUT consume a connect-carried username and still prompt for password, or
not?) are VMS internals we do **not** currently possess in evidence. They must be pinned against
a real VAX↔VAX `SET HOST` capture (packet trace + LOGINOUT transcript) **before** the network-
terminal-creation path is coded. CTERM is already entirely spec-derived; that honesty extends here.

---

## 3. Architecture

**3.1 One VMS primitive, no Linux above the line.** Every access path — console, SSH, SET
HOST/CTERM, future (TELNET) — calls the SAME VMS service: `$CREPRC` "create a process running
**LOGINOUT.EXE** bound to **terminal-device X**". Nothing above the VMS layer forks, execs, opens
a pty, or dup2s — those are `$CREPRC`'s and the RTAn: device's *internal* Linux implementation,
below the abstraction. LOGINOUT (an installed-privileged image, §2) authenticates against SYSUAF
and re-personas the process *post-creation*, so the creator forges no identity. The caller's only
job is transport: obtain the terminal device (console OPA0:, or an RTAn: bound to an SSH channel /
CTERM link, §3.2) and hand its name to `$CREPRC`. This is the same relationship a booted node's
JOB_CONTROL has to the console today — generalized into one primitive every path shares, with the
raw `fork/execl/pty` that JOB_CONTROL and vmssshd/decnetd expose today pushed down inside
`$CREPRC`, deleted from the callers. No path ever execs `DCL --login` directly (that flag is
faithful only as LOGINOUT's own handoff).

**3.2 RTAn: as a real-but-cheap executive device (the reconciliation).** The systems/purist
tension resolves cleanly: `RTAn:` is a real executive device-table entry
(`vms_devtab_add_terminal`, a ~150-LOC fork of the existing runtime `add_served_disk` pattern) —
`$GETDVI`-visible, `$ASSIGN`-able, owned by the session job, cross-process visible, with the
`VMS_TTC_*` characteristics OPA0: carries. It is **not** a new 800-1500-LOC terminal-QIO driver:
there is no executive byte-QIO surface for *any* device today (even OPA0:'s bytes flow over the
inherited Linux fd; the executive holds only identity/ownership/characteristics). RTAn: is backed
by a PTY (SSH) or a CTERM link (SET HOST) for byte transport — the OPA0: shape exactly. Faithful
= the **device** is executive-real and cross-process-visible; it does **not** require bytes to
traverse the executive. The LARP tell (§7.5) is precisely "a session with no executive device."

**3.3 NETACP: privileged detached process + executive device/dispatch face + userspace datalink.**
NETACP is a privileged `RUN/DETACHED` process (JOB_CONTROL's category — **not** moved into
`vms.ko`; the cluster's kernel-residency is for DLM/membership survival-across-death + cluster
timing, which DECnet has no analogue for — moving decnetd into vms.ko would be over-building).
NETACP owns: the **network-object table** (name/number/dispatch, in the executive device/table
layer so it is cross-process real), the **`_NET:`/`EWA0:` device face** (a `vms_devtab` entry
like ETH0:/PEA0:), and **NCP**'s view. The existing decnetd **wire engine** (HELLO/router-hello/
adjacency/NSP/CTERM codecs — the real assets in PR #1057) is **demoted to NETACP's datalink**,
the AF_PACKET socket hidden behind the device face. Inbound object-42 dispatch = *mint an RTAn:
backed by the CTERM link, then create a LOGINOUT session on it* — the same three primitives as
§3.1/§3.2, no bespoke authenticator.

**3.4 Executive-boundary / security posture (answers adversary A2/A8).** The attacker-controlled
wire-parsing (NSP/CTERM codecs, adjacency SM) must **not** live in a `CAP_SYS_ADMIN`-privileged
process. NETACP's *privileged* control path (which creates LOGINOUT sessions) is thin and does
**not** parse attacker bytes at privilege; the wire engine parses at low privilege and hands
NETACP a validated, typed connection descriptor. This is the same isolation the executive-resident
cluster enforces. Where a piece is unbuilt, fail `SS$_NOSUCHDEV` (INV-6), never fake.

---

## 4. Adversary attacks → disposition

| ID | Attack | Disposition |
|----|--------|-------------|
| A1 | `$CREPRC` stamps identity from creator; LOGINOUT self-stamps post-auth — conflation breaks arbitrary-user login; console is fork+execl not $CREPRC | **Resolved by §2/§3.1**: `$CREPRC` is the primitive, and LOGINOUT — **installed with privilege** — re-personas the process *post-auth* (real VMS). OVMX's current creator-identity-copy `$CREPRC` must be extended to this installed-privileged-LOGINOUT model. No fork+execl at the caller (that was the Linux leak the operator rejected). |
| A2 | NETACP-creates-LOGINOUT puts a network-facing, byte-parsing process in the root-equivalent identity set — worse than the hole it fixes | **Resolved by §3.4**: wire-parsing isolated from the privileged control path; auth concentrated in LOGINOUT, not reimplemented. Residual (a privileged network daemon exists) is JOB_CONTROL's existing, accepted posture — **permanent constraint**, mitigated by isolation. |
| A3 | CTERM accept path never decodes the connect username/password — "route through LOGINOUT" doesn't touch the gap; the decoder doesn't exist | **Real work, scoped** (§6 P3): the CTERM SC-connect access-control decoder is a named deliverable, gated on the oracle capture (A9). |
| A4/A9 | CTERM credential semantics (re-prompt vs consume) asserted, not sourced | **Blocking prerequisite** (§2 ⚑, §6 P0): oracle capture of real VAX SET HOST before coding the path. |
| A5 | RTAn: is a whole new kernel subsystem, sized as a footnote | **Resolved by §3.2**: built as the OPA0:-shape device (~150 LOC), not a QIO driver. |
| A6 | vmssshd's dead `VMS_*` env facade is a regression-in-waiting if refactored in place | **Explicit delete** (§6 P2): delete the file's inline block, not refactor-in-place. |
| A7 | Wall-6 asymmetric-arch gate (establish_system before SYSUAF read) must be carried per caller or the VAX leg breaks invisibly | **Hard requirement** on the shared caller sequence (§6 P2/P3); acceptance must run on the VAX rail. |
| A8 | userspace→executive `$CREPRC` boundary unresolved | **Resolved by §3.3**: the crossing IS the VMS-authentic path — NETACP is a real privileged VMS *process* (its own executive PCB, JOB_CONTROL-category) legitimately calling the `$CREPRC` service, exactly as VMS's NETACP $CREPRCs a login process. No raw fork/execl at the caller; the Linux mechanics live inside `$CREPRC`. |

---

## 5. What survives / what is deleted

**Survives (reused, not rewritten):** LOGINOUT.EXE (`tools/vms_login.c`) unchanged in its
authenticated body (~0-15 defensive LOC to be terminal-device-agnostic); `vms_ioctl_setident`/
`_quota`/`setprn` (the single persona primitive); `vms_devtab` runtime device-creation pattern;
the DECnet wire codecs/NSP/CTERM in **PR #1057** (become NETACP datalink internals — held, not
lost); `$CREPRC` (extended into the core installed-privileged-LOGINOUT session primitive, §3.1).
**Deleted:** the raw `fork/execl/openpty/dup2` login mechanics from *every* caller (console
included) — they move inside `$CREPRC`; vmssshd's inline LOGINOUT
reimplementation + its `VMS_*` env facade + `cred_drop.c`/`ssh_ident.c`/`term_map.c` + the second
SSH stack; decnetd's no-auth `fork+execvp`. **Net effect: a code *reduction*** (~+300 added,
~−400 deleted) — consolidation, not a big-bang rewrite.

---

## 6. Phased build plan (post-ratification; decomposed via /swarm-plan)

- **P0 (BLOCKING prereq):** oracle-capture real VAX↔VAX `SET HOST` (NETACP/RTTDRIVER/object/
  LOGINOUT sequence + CTERM credential semantics); update `docs/design-decnet-ovmx.md` to the
  vms-a1c engine ruling and add the NETACP/RTAn:/object-dispatch architecture (the doc still
  carries the superseded AF_DECnet-restore ruling).
- **P1 — the core primitive (no Linux above the line):** `$CREPRC` "create a process running
  LOGINOUT.EXE bound to terminal-device X", with LOGINOUT **installed-privileged** so it
  re-personas post-auth (extends OVMX's creator-identity-copy `$CREPRC`); `fork/execl/pty` hidden
  inside `$CREPRC`. Prove it first on the **console** (retire JOB_CONTROL's raw fork+execl into
  this call) — the reference path, so every later caller shares a primitive that already works.
- **P2:** `vms_devtab_add_terminal`/`_remove_terminal` — RTAn: real executive device, `$GETDVI`-
  visible, ~150 LOC + test (OPA0:-shape, PTY/CTERM-backed). Pure addition.
- **P3 (highest leverage):** vmssshd → delete the inline LOGINOUT reimpl + `VMS_*` env facades;
  the SSH daemon becomes transport-only — obtain an RTAn: (P2) bound to the SSH channel, then
  **call `$CREPRC` (P1)** to run LOGINOUT on it. No fork/execl/auth in the daemon. Carry Wall-6
  (`establish_system` before SYSUAF). SSH banner/`SHOW PROCESS` becomes provably identical to
  console.
- **P4 (with P3 — the no-auth one):** decnetd CTERM → the SC-connect username/password decoder +
  route object-42 dispatch through the SAME `$CREPRC`-LOGINOUT-on-RTAn: path. Closes the no-auth
  hole; deletes decnetd's `fork+execvp`.
- **P5:** NETACP executive device/dispatch face (`_NET:`/`EWA0:`/object-table) + wire-engine
  isolation (§3.4); demote the decnetd wire engine to the datalink; retire the dead SSH code +
  second stack.

---

## 7. Ratification gates (each is executive-substance, not naming)

1. **LOGINOUT.EXE is the only image that authenticates** — exactly one SYSUAF-reading, one
   `setident`-calling path; every access path reaches LOGINOUT via **`$CREPRC`**, never inline
   auth nor `DCL --login` directly. **No Linux above the VMS layer:** no `fork/execl/openpty/dup2`
   in any console/SSH/CTERM caller — those are `$CREPRC`/device-backing internals.
2. **RTAn:/TNAn:/FTAn: are real executive devices** — `$GETDVI`-visible, `$ASSIGN`-able, session-
   owned; no bare-pty-as-terminal, no bind-string literals.
3. **The `$CREPRC`-bound-to-terminal shape is actually built** (incl. console) so all paths share
   one creation primitive (P5).
4. **Clean-room-verify** the NETACP↔RTTDRIVER↔object↔LOGINOUT sequence against a lab VAX before
   coding it; every unsourced field flagged.
5. **The tell (from the scsd precedent):** if a "SET HOST works" green can be produced **without
   the executive device table changing**, it is a LARP. Bind the acceptance gate to `$GETDVI
   RTA0:` returning a real device **from a different process than the session** — exactly as
   cluster membership was bound to `$GETSYI` reading `vms.ko`.

**Compliance judgment (domain purist):** *faithful in intent, not yet in substance — conditionally
approve.* The plan identifies the one true model and correctly diagnoses the fragmentation; the
risk is declaring victory by renaming. The five gates are the guard against that.

<!-- rd-item: vms-515 -->
