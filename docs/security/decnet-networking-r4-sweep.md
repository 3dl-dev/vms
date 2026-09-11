# R4 Security Sweep — Inbound DECnet Networking Surface

**Gate:** R4 (V1.0 security). *"OVMX is a security boundary, or we state plainly it isn't. Shipping silently is the option that does not exist."* The node now sits inside a customer production cluster's trust boundary.

**Scope:** the **inbound-facing DECnet networking attack surface** — untrusted bytes arriving from a remote DECnet peer over the wire. Non-overlapping with the SCS/cluster sweep (vms-414). rd vms-2947.

**Grounded on:** `origin/main` @ `d7824ca3`. Every decoder in the target files was traced by hand for bounds, fuzz coverage, authentication, isolation, resource exhaustion, and privilege.

**Deliverable shape:** findings + posture recommendation. **Report, not fix** — real gaps are filed as rd items (listed below), not patched in place.

---

## 0. A scope fact that changes the reading (measure-first)

On **current origin/main**, the inbound CTERM server is **opt-in and not auto-started**:
- `cterm_server` defaults to `0` (`src/vmsdecnet/engine/decnetd.c:2487`); enabled only by an explicit `--cterm-server` flag.
- No `SYS$STARTUP`/`.COM` launches `DECNETD.EXE` on origin/main.

The **inbound enabler (PR #1162, `work/vms-a70-inbound-sethost`, in flight — not yet merged)** changes this: it makes the persistent NETACP daemon **serve object 42 by default** and auto-start at boot **when a DECnet executor address is configured** (`SYS$MANAGER:STARTNET.COM`, gated on NCP `SET EXECUTOR ADDRESS`). **This sweep assesses the surface as #1162 activates it** — that live inbound listen surface is the R4-relevant exposure — while recording that on current main the surface is dormant. The posture below applies to the 1.0 target (with #1162); it must not be read as "already exposed on main."

**Also latent (honest scoping):** the FAL object-17 server is wired only into the `--fal-*` socketpair self-tests, **never into the live `--cterm-server` datalink loop** (`decnetd.c:2133,2164`). On the live wire the **only** inbound object served is **CTERM 42**. FAL's gaps (G3) are latent until FAL is live-wired.

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

### G4 — Minor fuzz coverage gaps **(LOW)** → rd vms-8b36
The **host-role** `dnet_cterm_rx` FSM arm (the actual inbound-server role) and `found_terminal_rx` / `found_client_termchar_parse` are not mutation-fuzzed — memory safety is covered by the shared codec + terminal-role fuzz, but host-role state transitions and those outbound-client parsers rest on oracle round-trip only. Cheap to close (extend the existing mutation harness to the host role).

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
| vms-8b36 | G4 host-role/outbound-parser fuzz gaps | low | hardening backlog |
