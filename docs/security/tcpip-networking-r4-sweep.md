# R4 Security Sweep — Inbound TCP/IP Networking Surface

**Gate:** R4 (V1.0 security). *"OVMX is a security boundary, or we state plainly it isn't. Shipping silently is the option that does not exist."* The node sits inside a customer production cluster's trust boundary.

**Scope:** the **inbound-facing TCP/IP attack surface** — untrusted bytes/connections from a remote TCP client. The **twin** of the DECnet networking sweep (`docs/security/decnet-networking-r4-sweep.md`); together they compose the R4 networking-inbound posture. Non-overlapping with the SCS/cluster sweep (vms-414). rd vms-c9ef.

**Grounded on:** `origin/main` @ `ed426269` (re-grounded vms-c9ef; original sweep @ `6059f491`). Every path traced by hand for bounds, launch blast-radius, authentication, resource exhaustion, and privilege.

**Deliverable shape:** findings + posture recommendation. **Report, not fix** — real gaps filed as rd items (§5).

> **STATUS UPDATE (re-grounded @ `ed426269`).** Since the original sweep the two posture-defining gaps have **LANDED** and the third is **in flight**: **G1 (per-service run-as persona + privilege drop before `execv`, fail-closed) is CLOSED** (vms-8bd, V0.6-15); **G2 (INETD `MAXCHILD` concurrency cap + accept back-pressure) is CLOSED** (vms-bb4, V0.6-15); **G3 (decoder fuzz) is IN FLIGHT** (vms-deb3, on the DECnet fuzz loan, both SERVICE.DAT parsers). The **HARD GATE** the posture placed before enabling a reading/privileged service (notably the SSH rung) — *G1 + G2 must land first* — is therefore **MET on the technical axis**; what remains before an SSH-enable is G3 (in flight) and the **enable-decision itself, which is a Baron/conductor-reserved posture call, not enacted here.** §§3–5 below are updated in place to reflect this.

---

## 0. Scope fact that changes the reading (measure-first — mirrors the DECnet §0)

**On current origin/main the inbound TCP/IP surface is DORMANT by default and CI-guarded:**
- The shipped `SYSEXE/TCPIP$SERVICE.DAT` has **every service line commented out** — DAYTIME (:13) and SSH (:22) both disabled.
- The shipped `SYSTARTUP_VMS.COM` does **not** invoke `@SYS$STARTUP:TCPIP$STARTUP`, so `TCPIP$INETD.EXE` never runs — **nothing binds a port out of the box**.
- The bootable image carries **no aux-server image at all**; `TCPIP$INETD.EXE`/`TCPIP$DAYTIME.EXE` are staged only under `--build-arg OVMX_TEST_ENABLE_TCPIP=1` (test overlay `distro/rootfs-test-tcpip/`).
- `tests/integration/test_tcpip_posture_guard.sh:41-55` **reds** if a change ships an enabled service line or wires `TCPIP$STARTUP` — **enabling a listener is an explicit, CI-enforced, Baron-reserved posture decision.**

The 1.0-relevant exposure is the **operator-enabled** posture. **This sweep assesses the surface as an operator enabling a service activates it**, while recording that on current main it is dormant behind a CI guard — do not read this as "already exposed on main." (This *dormant-by-default* control is itself a real security strength, and the strongest one the surface has today.)

---

## 1. Attack-surface inventory (path of an untrusted inbound TCP byte)

| Layer | File | Untrusted input |
|---|---|---|
| Host kernel socket | `exec_kbackend_linux.h:772` `exec_socket_recv` | raw TCP bytes from `kernel_recvmsg` |
| Executive BGn: driver | `src/kernel-core/vms_bg.c` (recv `:471`, accept `:357`) | client bytes into fixed `data[4096]` |
| KIF veneer | `src/libvmssys/vms_kif.c` (recv `:2778`, accept `:2947`) | length re-clamp seam |
| BSD-sockets veneer | `src/vmstcpip/sockets/vms_bgsock.c` (recv `:307`, accept `:363`) | recv/accept over BGn: |
| Aux-server accept + launch | `tools/vms_tcpip_inetd.c:174-191`, `tcpip_inetd.h` (spawn `:291`, dispatch `:363`) | connect → `fork`+`execv` service image |
| Service image | `vms_tcpip_daytime.c` (only ship-enableable service today) | connection on fd 0/1 |
| Config store (local, **not wire**) | `tcpip_inetd_parse_db` (`tcpip_inetd.h:124`), `tcpip_svcdb_parse_rec` (`tcpip_service_db.h`, #878), `tools/vms_tcpip_config.c` | `TCPIP$SERVICE.DAT` / mgmt `.DAT` parse |

---

## 2. What IS hardened (the boundary that holds — file:line proof)

**Memory safety of the inbound byte path — TRIPLE-bounded, allocation-free, verified at every layer:**
- Host recv size-capped (`exec_kbackend_linux.h:772-780`).
- Executive: `bufsz = (len > VMS_BG_IOCTL_MAXLEN) ? 4096 : len`, recv into fixed `data[4096]` (`vms_bg.c:488,496`; `MAXLEN=4096`, `vms_bg.h:71,123`); send guards `len > MAXLEN → SS$_BADPARAM` **before** the memcpy (`:438`).
- KIF **re-clamps against a lying/buggy executive**: `n = args.len; if (n > bufsz) n = bufsz; if (n > 4096) n = 4096;` before `vms_memcpy` (`vms_kif.c:2795-2799`).
- Veneer caps again (`vms_bgsock.c:323`). No heap in the per-connection path — a fixed 64-slot table (`:64-65`), `EMFILE` when full; handles range-checked (`:84,:105-109`).

**SERVICE.DAT parser — bounds-safe** (`tcpip_inetd.h:124-187`): line clamped before memcpy (`:144`); `strncpy(...,size-1)` into fixed `name[32]`/`image[256]`/`args[256]` (`:162,:174,:181`); port `strtol` range-checked 1..65535 (`:167`); malformed lines skipped, not faked; `MAX_SERVICES=16`.

**SERVICE.DAT *management* parser — bounds-safe (#878, `tcpip_service_db.h`):** the DCL `TCPIP {SET,SHOW,ENABLE,DISABLE,DELETE} SERVICE` engine parses the **same** grammar (`tcpip_svcdb_parse_rec`) with the same discipline — pre-clamped `line[512]`, `strncpy(...,size-1)` into the same fixed buffers, `strtol` range-check, `continue` (clean-skip) on any malformed field, `TCPIP_SVCDB_MAX=16` — plus a leading-`!` disabled-record convention and a `':'`/`'/'`-disambiguated run-as-username-vs-image token. The DCL write side supersedes over the Files-11 ACP (`tcpip_svcdb_store`), fail-honest (`%TCPIP-W-NOEXEC` with no executive). Still **config/operator input, not wire** (A2) — but it now round-trips with the aux-server reader, so both parsers are covered by the G3 fuzz (below).

**Launch path — a hostile client cannot influence *what* is launched:**
- Image path + args come from `TCPIP$SERVICE.DAT` (config, trusted), **not the wire**; argv re-split from `svc->args` at spawn (`tcpip_inetd.h:309-319`). The client controls **only the connection bytes** to the service (fd 0/1).
- **`execv`, no shell** (`tcpip_inetd.h:346`) — no shell-metachar injection even from config.
- **Pre-flight present (vms-f00):** INETD refuses to *advertise* a service whose image can't be staged/executed — no bound-but-unserviceable facade (`vms_tcpip_inetd.c:154-160`).
- Rule-9 honest-fail: no `/dev/vms` → `SS$_NOSUCHDEV` → INETD exits non-zero, never a fake per-process listener (`vms_tcpip_inetd.c:137-140`).

**The only ship-enableable service today is near-unreachable from the wire:** DAYTIME **reads nothing** (RFC 867 discards input), writes one bounded line via `strftime` into `line[64]` (`tcpip_daytime.h:69-93`) — unauthenticated **by design** and correct.

**Dormant-by-default + the CI posture guard** (§0) — a genuine posture control, not just documentation.

---

## 3. Residual GAPS — ranked by what a hostile client can ACTUALLY achieve

### G1 — per-service run-as identity + privilege drop before `execv` — **CLOSED (vms-8bd, V0.6-15)** → rd vms-8bd
**RESOLVED.** INETD now drops each spawned service to its own SYSUAF run-as account (the `SERVICE.DAT` username field → `vms_kif_setident` + Linux credential drop) **before** `execv`, **fail-closed**: an empty/unknown/refused account `_exit(125)`s and the service never launches, **never as SYSTEM** (`tcpip_inetd_ident.c`, `tcpip_inetd.h`). Proven on main by the fail-closed CHECKs in `test_syssvc_tcpip_inetd.c` — assertions "a service with NO configured run-as account fails CLOSED (never SYSTEM)", "on a refused identity the Linux credential drop is NOT attempted (fail-closed short-circuit)", "an accepted identity + verified cred drop succeeds" all PASS (verified green on the x86_64 and Alpha rails). The posture-defining ceiling below is lifted: a service no longer inherits INETD's SYSTEM/all-privs identity. *(Original gap analysis retained for context:)*
INETD inits its PCB with `vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)` (`vms_tcpip_inetd.c:95`) → `cur_privs = perm_privs = 0xFFFF…FFFF` (**all privilege bits**), `uic = 0` (**SYSTEM [0,0]**) (`vms_pcb.c:41,61-62`), started `RUN/DETACHED` from `TCPIP$STARTUP.COM:55`. The spawn is raw `fork()+execv()` (`tcpip_inetd.h:334,346`) with **no setuid/setgid/persona/privilege-drop anywhere** — a grep of the surface returns only the header comment admitting per-service run-as identity is "deferred" (`tcpip_inetd.h:70`). **The launched service inherits INETD's full SYSTEM identity and every privilege.** **This is WORSE than the DECnet twin:** DECnet's inbound CTERM passes `PRC$M_LOGINOUT` so `LOGINOUT` authenticates the remote user and the session starts with **no identity**; INETD has **no equivalent gate** — the service *is* SYSTEM from its first instruction. **Consequence:** a memory-corruption bug in *any* enabled service image, or in INETD's own parse, executes **as SYSTEM with all privileges** — no defense-in-depth if the (strong) bounds controls fail. This is the ceiling on the boundary. Remediation: a per-service run-as identity + drop privileges before the service `execv`; optionally seccomp the child.

### G2 — INETD concurrency cap + accept back-pressure — **CLOSED (vms-bb4, V0.6-15)** → rd vms-bb4
**RESOLVED.** The aux server now caps concurrent service children (`TCPIP_INETD_MAXCHILD`) with **accept back-pressure**: the control loop counts live children and stops selecting the listeners for `POLLIN` at the cap (new SYNs queue in the listen backlog) until a child exits — so a network fork-flood can no longer exhaust PIDs/memory. Proven on main by `test_syssvc_tcpip_inetd.c` — "fork-flood gate: accept when idle / accept just below the child cap / back-pressure AT the child cap (no unbounded fork) / back-pressure above the child cap" all PASS. The slowloris fork-bomb-via-network below is bounded. *(Original gap analysis retained for context:)*
The accept loop dispatches one `fork()+execv()` per inbound connection (`vms_tcpip_inetd.c:174-191` → `tcpip_inetd.h:363-380`). Reaping is `waitpid(-1,…,WNOHANG)` (`:189`) — **non-blocking, with NO live-child counter and NO MAXCHILD ceiling**; `listen` backlog is 5. **What a hostile client achieves:** for a write-and-exit service (DAYTIME) it is spawn-rate / PID churn (children exit promptly); for **any reading/slow service under slowloris** (the future SSH rung), children **accumulate unbounded → PID/memory exhaustion — a fork-bomb-via-network.** INETD imposes **no concurrency ceiling of its own** — it relies entirely on each service self-limiting (OpenSSH's `MaxStartups`). **INETD itself does not crash** (fixed buffers, no retained per-connection listener state). **Denial is spawn-storm/resource, not an INETD crash.** Remediation: a MAXCHILD concurrency cap + per-source connect rate-limit + accept back-pressure.

### G3 — Fuzz coverage of the decoders — **IN FLIGHT (vms-deb3)** → rd vms-deb3
**IN PROGRESS on the DECnet fuzz loan.** Exact-sized-buffer ASan/UBSan mutation fuzzing of **both** SERVICE.DAT parsers — `tcpip_inetd_parse_db` (`tcpip_inetd.h`) and the #878 `tcpip_svcdb_parse_rec` (`tcpip_service_db.h`) — with a shared corpus and a `parse_rec → format_rec → inetd_parse_db` round-trip over-read assertion; the over-read bug class (missing/embedded NUL, over-length token, no-newline line, all-whitespace, bare `!`) is contract-agnostic. The `bgsock` recv path is a bounded 4096-byte stream copy (not a structured decoder), memory-safe by inspection (§2) — low fuzz value. **Original state (superseded):** no coverage-guided/mutation fuzzing existed (only `test_syssvc_tcpip_daytime.c`, a functional proof); DECnet shipped 5 sanitizer-fuzzed decoders. **Mitigant:** the TCP/IP byte path is far simpler (fixed 4096 buffers, no nested TLV), memory-safe by inspection at every layer (§2). Close (deb3 lands) before enabling any service that *reads* client bytes. **Higher-untrusted follow-on (flagged):** the client-tool *response* parsers that eat network bytes — `tcpip_client.h` (TELNET option negotiation, FTP reply/length) and `tcpip_ping.h` (ICMP echo-reply) — are a separate, arguably higher-value fuzz rung than the admin-config SERVICE.DAT parse.

---

## Areas to state in posture (honest limitations, not bugs)

- **A1 — INETD is a dispatcher, not an authentication boundary.** Each launched service authenticates itself or not (DAYTIME public-by-design; a wrapped sshd self-authenticates). INETD grants the connection with **no auth of its own** — safe only while every enabled service is public-by-design or self-authenticating.
- **A2 — The config store is local, not wire.** `TCPIP$SERVICE.DAT` / mgmt `.DAT` parsing is operator/config input, not attacker-reachable; the parser is bounds-safe (§2). `tools/vms_tcpip_config.c` is an interactive operator wizard (bounded `fgets`/`sscanf`, `execv` of `vmsdcl`) — operator-local, out of the inbound-wire scope.
- **A3 — Dormant-by-default is the current de-facto boundary** (§0) — the strongest control on main today is that the surface is off and CI-guarded.
- **A4 — Cleartext transport.** DAYTIME and Phase-IV-era inetd services provide no wire confidentiality/integrity by design (SSH excepted). A protocol property, not an OVMX defect, but it bounds any claim.

---

## 4. Posture recommendation (input to Baron's R4 ruling — his reserved call)

The inbound TCP/IP **byte path is genuinely hardened at the memory-safety layer** (triple-bounded, allocation-free, honest-failing), with a real config/wire separation (the client cannot influence *what* is launched) and a real pre-flight. **What OVMX's TCP/IP surface holds today is a memory-safety + config-integrity boundary that ships DORMANT-by-default.**

The honest limits that define the edge (updated for G1/G2 landing):
- **(a) Per-service run-as identity + privilege drop — NOW PRESENT (G1 CLOSED, vms-8bd).** A launched service drops to its own SYSUAF account before `execv`, fail-closed (never SYSTEM). This lands the isolation the DECnet twin gets from its LOGINOUT identity-reset — the two inbound surfaces now match on identity discipline. Residual: OS-level sandboxing (seccomp) is still absent by design (faithfulness note below).
- **(b) DoS resistance in the aux server — NOW PRESENT (G2 CLOSED, vms-bb4).** A `MAXCHILD` concurrency cap + accept back-pressure bounds the fork-flood; the slowloris network-fork-bomb is closed even for a reading service.
- **(c) INETD is a dispatcher, not an auth boundary (A1)** — unchanged; safe only while every enabled service is public-by-design or self-authenticating (a wrapped sshd self-authenticates in-protocol).
- **(d) Decoder fuzz — IN FLIGHT (G3, vms-deb3)** — lower residual risk than DECnet given the simpler path; landing now on the fuzz loan.

**Recommended framing (composes with the DECnet ruling; Baron chooses):**
1. **"OVMX's TCP/IP surface is a memory-safety + config-integrity boundary, with per-service identity isolation and DoS back-pressure on the aux server, that ships DORMANT-by-default; it is NOT a seccomp-sandboxed or transport-authenticating boundary (each service authenticates itself). Enabling any inbound service is an explicit operator posture decision."** — precise, defensible, matches the code + the CI guard. **Recommended for 1.0.** The original **HARD GATE — G1 (per-service run-as + privilege drop) and G2 (concurrency cap / back-pressure) MUST land before any *reading* or privileged service (notably the SSH rung) is enabled — is now MET (both landed, V0.6-15).** What remains before an SSH-enable is **G3** (decoder fuzz, in flight — vms-deb3) and the **enable-decision itself**, which is the reserved posture call (conductor per the R4 Option-1 precedent, or Baron if it is a confidentiality call) — **not enacted by this sweep.**
2. **"OVMX's TCP/IP surface is a hardened security boundary"** — G1 + G2 are now remediated; only G3 (fuzz, in flight) stands before a reading service, plus the enable-decision.

**Faithfulness note (as for DECnet):** real VMS `TCPIP$INETD` also spawns services and does not seccomp-sandbox them; option 1 matches what real VMS provides rather than gold-plating past it. The *dormant-by-default* posture is, if anything, *stronger* than a stock TCP/IP kit.

**Composition with the DECnet posture (updated):** both inbound surfaces are now **memory-safe AND identity-isolated**. DECnet resets identity via a **fresh-LOGINOUT** on inbound CTERM; TCP/IP drops each service to its **own SYSUAF run-as account** before `execv` (G1) and bounds the aux server with **`MAXCHILD` back-pressure** (G2), and additionally ships **dormant-by-default**. If Baron takes option 1 for both, the whole R4 networking ruling reads: *"OVMX's inbound networking is a memory-safety boundary with per-service/LOGINOUT identity isolation and (TCP/IP) aux-server DoS back-pressure — NOT a seccomp-sandboxed or transport-authenticating one; TCP/IP additionally ships off by default, and enabling a reading/privileged service is gated only on G3 (fuzz, in flight) plus the reserved enable-decision."*

---

## 5. Filed findings (report-not-fix)

| rd | Gap | Severity | Gate | Status |
|---|---|---|---|---|
| vms-8bd | G1 services run as SYSTEM/all-privs, no persona/privilege-drop | posture-defining | before SSH / any reading or privileged service | **CLOSED** (per-service run-as + drop, fail-closed; V0.6-15) |
| vms-bb4 | G2 INETD fork-flood DoS (no MAXCHILD/rate-limit) | medium (spawn-storm) | before SSH / any reading service | **CLOSED** (MAXCHILD cap + accept back-pressure; V0.6-15) |
| vms-deb3 | G3 no fuzz coverage (SERVICE.DAT parsers: `tcpip_inetd_parse_db` + #878 `tcpip_svcdb_parse_rec`) | low-medium | before a client-reading service | **IN FLIGHT** (DECnet fuzz loan) |
| *(follow-on)* | Higher-untrusted network-byte parsers: `tcpip_client.h` TELNET/FTP replies, `tcpip_ping.h` ICMP reply | low-medium | before relying on the client tools against hostile peers | proposed (my surface; coordinate w/ fuzz loan) |
