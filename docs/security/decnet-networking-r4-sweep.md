# R4 Security Sweep — Inbound DECnet Networking Surface

**Gate:** R4 (V1.0 security). *"OVMX is a security boundary, or we state plainly it isn't. Shipping silently is the option that does not exist."* The node now sits inside a customer production cluster's trust boundary.

**Scope:** the **inbound-facing DECnet networking attack surface** — untrusted bytes arriving from a remote DECnet peer over the wire. Non-overlapping with the SCS/cluster sweep (vms-414). rd vms-2947.

**Grounded on:** originally `origin/main` @ `d7824ca3` (2026-09-11); **re-grounded on `origin/main` @ `723472af` (2026-09-12)** — see the dated **§6 Update** for what changed since the inbound enabler and the FAL COPY command layer merged. Every decoder in the target files was traced by hand for bounds, fuzz coverage, authentication, isolation, resource exhaustion, and privilege.

**Deliverable shape:** findings + posture recommendation. **Report, not fix** — real gaps are filed as rd items (listed below), not patched in place.

> **Status (2026-09-12):** posture **RULED — Option 1** (memory-safety + authentication boundary; not privilege-isolated / DoS-hardened / confidential), and the R4 hard-gate is **MET** — the two blocking gaps landed in V0.6-15 (**G1** per-service persona #1177, **G2** MAXCHILD #1171). This doc is now the standing record of the surface; **§6** re-grounds it on current main.

---

## 0. A scope fact that changes the reading (measure-first)

**Updated 2026-09-12 — the inbound enabler merged (#1162).** On current `origin/main` the persistent NETACP daemon **serves inbound object 42 by default** (`cterm_server = 1`, `src/vmsdecnet/engine/decnetd.c`) and is auto-started at boot by `SYS$MANAGER:STARTNET.COM` (registered at the LPBETA phase). **The activation is config-gated, and that gate is the key posture fact:** STARTNET is a **silent no-op unless the node has been configured for DECnet** — it runs nothing and prints nothing until NCP `SET/DEFINE EXECUTOR` has written `SYS$SYSTEM:NETNODE_LOCAL.DAT` (`STARTNET.COM`, a pure-DCL `F$SEARCH` gate). So:

- **A default/fresh install is DORMANT** — no inbound DECnet listener exists until an operator explicitly configures the executor address. The surface is **not exposed out of the box**.
- **Once configured, the live inbound listen surface is active** — that is the R4-relevant exposure this sweep assesses. NETACP additionally self-checks (defense in depth): launched with no executor address it logs a not-configured note and serves nothing.

This *strengthens* the earlier framing (the pre-#1162 draft treated the enabler as in-flight): the exposure is opt-in by configuration, not automatic on every boot.

**Also latent (unchanged, honest scoping):** the FAL object-17 **server** is wired only into the `--fal-*` socketpair self-tests, **never into the live inbound datalink loop**. On the live wire the **only** inbound object served is **CTERM 42**. FAL's gaps (G3) are latent until the FAL server is live-wired. (The FAL *client* — the outbound `$ COPY` command layer added since this draft — is covered in §6; it is outbound/local-initiated, not inbound wire surface.)

---

## 1. Attack surface inventory

An untrusted frame from a remote peer transits, in order:

| Layer | File | Untrusted input |
|---|---|---|
| Datalink / routing / HELLO | `dnet_engine.c`, `routing/` | raw Ethernet 0x6003 frames |
| NSP transport | `nsp/dnet_nsp.c` | Connect Initiate/Confirm, Data, Ack, Link Service, Disconnect |
| Session Control connect | `cterm/dnet_cterm.c` | object id + access-control descriptors (user/password/account) |
| CTERM foundation + terminal I/O | `cterm/dnet_cterm.c` | foundation TLV/envelope, Read/Write/OOB |
| **Isolation seam (A2/A8)** | `cterm/dnet_cterm.c` → `cterm/dnet_cterm_host.c` | validated typed descriptor (no wire bytes cross) |
| Privileged session creation | `cterm/dnet_cterm_host.c` | mints `RTAn:`, `$CREPRC` LOGINOUT |
| FAL (latent) | `fal/dnet_fal.c`, `dap/dnet_dap.c` | connect-carried creds + DAP file ops |
| Accept loop | `engine/decnetd.c:2694-3020` | dispatches the above on the live datalink |

---

## 2. What IS hardened (the boundary that holds)

**Memory safety of every inbound decoder — strong, evidenced:**

- **NSP** (`dnet_nsp.c:80-219`): header length floor (`:86`); every per-type arm re-checks `remain` before use; every `memcpy` into the fixed `out->data[1024]` is guarded `remain > DNET_NSP_MAX_DATA → EBADLEN` **before** the copy (`:114,:132,:155,:166,:203`). Byte-wise endian-neutral reads; `remain` only decreases from `len` (no integer-overflow path). No pointer arithmetic on unchecked lengths.
- **CTERM / SC-connect** (`dnet_cterm.c`): counted-string helpers **refuse rather than clip** (`sc_get_string :668-669`); descriptor parse refuses unknown format and object 0 (`:697,:719`); on any error the whole output is re-zeroed. `dnet_conn_descriptor_from_wire` is **memset-first**, sets `validated=1` **last** (`:1033,:1053`), so a caller that ignores the return value still gets an unusable descriptor. Terminal-I/O decode double-checks `datalen > MAX_DATA` then `off+datalen > len` before copy (`:196,:211`).
- **Injection defense:** printable-only filters strip non-`0x20-0x7e` from anything reaching the console or the accounting surface (`desc_copy_printable :1008`, `remote_port_info :993`); a peer cannot inject control/escape sequences. **The accounting address is the routing-header address, not peer-claimed** (`:975`) — a peer cannot spoof its identity on the accounting surface.
- **Password hygiene:** the SC-connect password is **measured but never retained** (`sc_connect_parse :849-860`); FAL wipes the password/account/user buffers after the auth check (`dnet_fal.c:253-264`).

**Fuzzed under sanitizers:** NSP decode (60k iters), `sc_connect_parse`, `from_wire`, foundation parse, `dnet_cterm_rx`/decode (60k iters), DAP decode (200k) — all built `-fsanitize=address,undefined`, with `OVMX_FUZZ_REQUIRE_SANITIZERS` teeth on the routing/emit fuzzers (`tests/vmsdecnet/CMakeLists.txt:108-163`). **Zero heap allocation** in all five surface files — every buffer is a fixed stack/struct member, so there is no allocator to exhaust or corrupt.

**Authentication — real, no unauthenticated admission:**

- **CTERM (42):** the privileged path (`dnet_cterm_host.c:83-168`) **parses no wire bytes**, refuses `!validated` and any non-object-42 descriptor **before** any device/process exists (`:100-104`); `$CREPRC` is passed `uic=0, prvadr=NULL, PRC$M_LOGINOUT` (`:151-153`) so the session starts with **no identity** — the real `LOGINOUT.EXE` challenges the remote user fresh; the connect-carried username reaches no decision. No `fork/exec/openpty/dup2` anywhere above the VMS layer (gated by `test_creprc_session_primitive.sh` check 5).
- **FAL (17, latent):** `dnet_fal_authenticate` runs the **one faithful authenticator** — `sysuaf_lookup` + Purdy `sysuaf_authenticate` + disabled-account gate; no-user and bad-password both return `SS$_INVLOGIN` (**no user enumeration**); no file served on a bad connect.

**Isolation seam (A2/A8):** faithfully reproduced at the live call site (`decnetd.c:2907-2925`) — parse at the low-privilege codec, hand **only** the validated typed descriptor to the privileged path; a malformed frame → `SS$_BADPARAM` → wire disconnect, admits nobody. No fallback shell on any refusal.

---

## 3. Residual GAPS — ranked by what a hostile peer can ACTUALLY achieve

### G1 — No OS privilege separation / no privilege drop **(POSTURE-DEFINING)** → rd vms-aef
`grep setuid|setgid|capset|prctl|seccomp|no_new_privs|chroot|unshare` across `src/vmsdecnet/**` returns **nothing**. The daemon acquires `CAP_NET_RAW` and **never drops it** or any launch privilege. The "low-privilege" wire parser and the privileged `$CREPRC` session-creator run **in the same address space at the same OS privilege** — the A2/A8 split is a **code-structure** boundary (the validated-descriptor seam), **not a process/sandbox boundary**. **Consequence:** a single memory-corruption bug in *any* inbound decoder executes in the process that holds `CAP_NET_RAW` and creates processes; there is **no defense-in-depth** if the (strong) bounds/fuzz controls ever fail. This is not a bug — it is the **ceiling** on how strong this boundary can be, and it must be stated plainly. Remediation option: drop `CAP_NET_RAW` + apply a `seccomp` filter after the socket is open, or split the wire-parser into a separate low-privilege process behind the descriptor seam.

### G2 — DoS: no rate-limit, no idle timeout, single session/link **(CONFIRMED — service denial)** → rd vms-6af1
One-session-at-a-time (`host_active`, `decnetd.c:2883`) over a **single** engine logical link. There is **no idle/session watchdog, no rate-limit, no half-open cap** — only the routing T3/listen sweep, which does not bound a bound CTERM session. An unauthenticated peer that connects and **stalls at LOGINOUT's `Username:`**, then reconnects, can hold the single SET HOST slot **continuously**, denying all legitimate inbound SET HOST. **Impact is bounded:** the daemon does not crash, and there is no memory/fd exhaustion (fixed buffers, serialized sessions, `dnet_cterm_host_close` balanced on every teardown path `:2825,:2977,:2982`). **Denial is of the SET HOST object, not the node.** Remediation: an inbound session idle timeout + a per-source connect rate-limit; optionally a small bounded session pool instead of one-at-a-time.

### G3 — FAL post-auth authorization gap **(CONFIRMED — latent)** → rd vms-d85
Authentication is real, but after it `dnet_fal_server_run` serves files via `rms_textfile_open(spec)` **with the daemon's own identity — no persona to the authenticated UIC, and no filespec confinement** (`dnet_fal.c:283-285`). An authenticated user could GET/PUT any path the daemon (system) can reach. **Latent** — FAL is not live-wired (§0) — but this **must be closed before FAL object-17 is exposed on the live datalink** (persona to the authenticated UIC + `$CHKPRO`/filespec confinement).

### G4 — Minor fuzz coverage gaps **(LOW) — CLOSED 2026-09-12** → rd vms-8b36
*Originally:* the **host-role** `dnet_cterm_rx` FSM arm (the actual inbound-server role) and `found_terminal_rx` / `found_client_termchar_parse` were not mutation-fuzzed — memory safety was covered by the shared codec + terminal-role fuzz, but host-role state transitions and those parsers rested on oracle round-trip only.
**Closed:** #1175 added host-role `dnet_cterm_rx` mutation fuzz plus the `found_terminal_rx` / `found_client_termchar_parse` fuzz (`test_dnet_cterm.c`: `host_accepts` / `ft_undef` / `fc_undef`); #1197 added the node-filespec splitter / COPY-plan ASan/UBSan fuzz (`test_dnet_nodespec.c`, 400k inputs), which additionally **surfaced and fixed** a clean-on-reject credential-leak in `dnet_nodespec_parse`. The inbound host-role FSM and the new outbound-COPY parser are now both mutation-fuzzed under sanitizers.

---

## 4. Posture recommendation (input to Baron's R4 ruling — his reserved call)

The inbound DECnet codecs are **genuinely hardened at the memory-safety layer** (exhaustively bounded, allocation-free, sanitizer-fuzzed, injection-filtered), the A2/A8 descriptor seam is real, and inbound authentication is real (fresh LOGINOUT for CTERM; SYSUAF/Purdy for FAL) with no unauthenticated admission path. **What OVMX's DECnet surface holds today is a memory-safety + authentication boundary.**

The honest limits that **define the edge** of that boundary:
- **(a) No OS-level privilege isolation (G1)** — one process, one privilege, a code-structure seam. Strong compensating controls, no defense-in-depth.
- **(b) No availability/DoS resistance on the inbound session (G2)** — a hostile peer can deny the SET HOST service (not crash the node).
- **(c) FAL authorization is authentication-only and not yet confined (G3)** — latent until FAL is live-wired.
- Underlying all of it: **DECnet Phase IV is a cleartext LAN protocol** — it provides no wire confidentiality or integrity by design. That is a protocol property, not an OVMX defect, but it bounds any claim.

**Recommended ruling framing (two honest options — Baron chooses):**
1. **"OVMX's DECnet surface is a memory-safety + authentication boundary, but NOT a DoS-resistant, privilege-isolated, or confidential-transport boundary"** — a precise, defensible carve-out that ships honestly and matches the code. Given the node sits inside a *production cluster's trust boundary*, this is coherent (the cluster LAN is assumed semi-trusted; DECnet Phase IV assumes the same).
2. **"OVMX's DECnet surface is a hardened security boundary"** — then **G1 and G2 must be remediated first** (privilege drop + seccomp after socket open; inbound session idle-timeout + connect rate-limit), and G3 before FAL exposure.

Recommendation: **option 1** is the honest, shippable posture for 1.0, with G1/G2/G3 tracked as the "raise the boundary" backlog. Either way, the "on by default when configured" activation arrives with PR #1162 — the posture must be published alongside it (INV-0).

---

## 5. Filed findings (report-not-fix)

| rd | Gap | Severity | Gate |
|---|---|---|---|
| vms-aef | G1 no OS privilege separation | posture-defining | before "security boundary" ruling |
| vms-6af1 | G2 inbound DoS (no rate-limit/timeout) | medium (service denial) | before "security boundary" ruling |
| vms-d85 | G3 FAL post-auth authorization/confinement | high-when-exposed | before FAL live-wire |
| vms-8b36 | G4 host-role/outbound-parser fuzz gaps | low | **CLOSED** (#1175 host-role fuzz + #1197 splitter/COPY-plan fuzz) |

---

## 6. Update — re-grounded on current main (2026-09-12)

This sweep was first written on `d7824ca3`, before the inbound enabler and the FAL COPY command layer merged. Re-grounded on `723472af`; nothing below changes the **posture**, which stands as ruled (**Option 1**, gate **MET**). The corrections are factual accuracy on a live gate doc.

**1. Inbound activation merged (#1162) — dormant-by-default (see the rewritten §0).** The persistent NETACP daemon now serves object 42 by default (`cterm_server = 1`) and auto-starts via `SYS$MANAGER:STARTNET.COM` at LPBETA — but **config-gated**: STARTNET is a silent no-op until NCP `SET/DEFINE EXECUTOR` writes `SYS$SYSTEM:NETNODE_LOCAL.DAT` (pure-DCL `F$SEARCH` gate). A fresh install is **dormant** (no listener until an operator configures DECnet). This **strengthens** the posture — the inbound surface is opt-in by configuration, not automatic on every boot — and the silent gate keeps the boot console clean (the vms-1fb oracle enforces it).

**2. G4 closed (#1175 + #1197).** The host-role `dnet_cterm_rx` FSM arm and the foundation/terminal parsers are now mutation-fuzzed (#1175), and the new node-filespec parser is ASan/UBSan-fuzzed (#1197). See §3 G4. The fuzz **earned its keep**: #1197's fuzz surfaced a clean-on-reject **credential-leak** in `dnet_nodespec_parse` (a mid-parse rejection could leave a half-parsed username in the output) and it was fixed to re-zero the output on every non-OK return — matching `dnet_fal_access_decode`'s existing guarantee.

**3. New surface: the outbound `$ COPY` command layer (#1193 / #1196 / #1197) — outbound/local, not inbound wire.** A DCL `COPY NODE"user pw"::file local` is decomposed by `dnet_nodespec_parse` + `dnet_copy_plan` (fal lib) and would drive the object-17 connect + `dnet_fal_client_get/put`. Classification for this sweep:
   - **Not inbound attack surface.** The parser eats a **local** command argument (from a DCL process), not untrusted bytes off the wire; direction/creds are decided locally. It is in-scope here only because it is new credential-handling code.
   - **Credential handling is hardened.** The FAL access password is a real credential (unlike CTERM, where LOGINOUT re-authenticates and the connect password is empty), so the `--copy` activation **refuses a password on argv** (`%DECNETD-E-COPYPW` — it would be world-readable in `/proc/<pid>/cmdline`) and reads it only from an inherited `--password-fd`; the splitter is clean-on-reject (item 2) and fuzzed. This is a **decided credential posture** (no password on argv; fd/pipe only; refuse the fork-fallback), not an ad-hoc choice.
   - **The live transfer is not wired** — `run_copy_loop` validates the plan + enforces the posture, then reports `%DECNETD-I-COPYNOTWIRED` honestly (a DCL process does not yet own a live DECnet circuit to carry DAP). The **inbound** FAL object-17 server likewise remains latent (§0). So **G3 (FAL post-auth authorization/confinement) is still latent** and still gates FAL live-wire, unchanged.

**Net:** the boundary described in §2–§4 holds and is now **more** accurate — dormant-by-default inbound activation, one previously-open fuzz gap (G4) closed, and a new credential-handling path added with its posture enforced. **Posture unchanged: Option 1, gate met.** The remaining "raise the boundary" backlog is still G1 (vms-aef) / G2 (vms-6af1, hard-gate piece landed) / G3 (vms-d85, before FAL live-wire); the DECnet **live legs** that would exercise more of this surface (inbound SET HOST bracket, live outbound COPY transport, FAL server live-wire, task-to-task) are all gated on the **vms-101** build-host/lab.
