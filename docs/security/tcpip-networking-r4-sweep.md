# R4 Security Sweep — Inbound TCP/IP Networking Surface

**Gate:** R4 (V1.0 security). *"OVMX is a security boundary, or we state plainly it isn't. Shipping silently is the option that does not exist."* The node sits inside a customer production cluster's trust boundary.

**Scope:** the **inbound-facing TCP/IP attack surface** — untrusted bytes/connections from a remote TCP client. The **twin** of the DECnet networking sweep (`docs/security/decnet-networking-r4-sweep.md`); together they compose the R4 networking-inbound posture. Non-overlapping with the SCS/cluster sweep (vms-414). rd vms-c9ef.

**Grounded on:** `origin/main` @ `6059f491`. Every path traced by hand for bounds, launch blast-radius, authentication, resource exhaustion, and privilege.

**Deliverable shape:** findings + posture recommendation. **Report, not fix** — real gaps filed as rd items (§5).

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
| Config store (local, **not wire**) | `tcpip_inetd_parse_db` (`tcpip_inetd.h:124`), `tools/vms_tcpip_config.c` | `TCPIP$SERVICE.DAT` / mgmt `.DAT` parse |

---

## 2. What IS hardened (the boundary that holds — file:line proof)

**Memory safety of the inbound byte path — TRIPLE-bounded, allocation-free, verified at every layer:**
- Host recv size-capped (`exec_kbackend_linux.h:772-780`).
- Executive: `bufsz = (len > VMS_BG_IOCTL_MAXLEN) ? 4096 : len`, recv into fixed `data[4096]` (`vms_bg.c:488,496`; `MAXLEN=4096`, `vms_bg.h:71,123`); send guards `len > MAXLEN → SS$_BADPARAM` **before** the memcpy (`:438`).
- KIF **re-clamps against a lying/buggy executive**: `n = args.len; if (n > bufsz) n = bufsz; if (n > 4096) n = 4096;` before `vms_memcpy` (`vms_kif.c:2795-2799`).
- Veneer caps again (`vms_bgsock.c:323`). No heap in the per-connection path — a fixed 64-slot table (`:64-65`), `EMFILE` when full; handles range-checked (`:84,:105-109`).

**SERVICE.DAT parser — bounds-safe** (`tcpip_inetd.h:124-187`): line clamped before memcpy (`:144`); `strncpy(...,size-1)` into fixed `name[32]`/`image[256]`/`args[256]` (`:162,:174,:181`); port `strtol` range-checked 1..65535 (`:167`); malformed lines skipped, not faked; `MAX_SERVICES=16`.

**Launch path — a hostile client cannot influence *what* is launched:**
- Image path + args come from `TCPIP$SERVICE.DAT` (config, trusted), **not the wire**; argv re-split from `svc->args` at spawn (`tcpip_inetd.h:309-319`). The client controls **only the connection bytes** to the service (fd 0/1).
- **`execv`, no shell** (`tcpip_inetd.h:346`) — no shell-metachar injection even from config.
- **Pre-flight present (vms-f00):** INETD refuses to *advertise* a service whose image can't be staged/executed — no bound-but-unserviceable facade (`vms_tcpip_inetd.c:154-160`).
- Rule-9 honest-fail: no `/dev/vms` → `SS$_NOSUCHDEV` → INETD exits non-zero, never a fake per-process listener (`vms_tcpip_inetd.c:137-140`).

**The only ship-enableable service today is near-unreachable from the wire:** DAYTIME **reads nothing** (RFC 867 discards input), writes one bounded line via `strftime` into `line[64]` (`tcpip_daytime.h:69-93`) — unauthenticated **by design** and correct.

**Dormant-by-default + the CI posture guard** (§0) — a genuine posture control, not just documentation.

---

## 3. Residual GAPS — ranked by what a hostile client can ACTUALLY achieve

### G1 — INETD-launched services run as SYSTEM with ALL privileges; no privilege separation, no persona **(POSTURE-DEFINING)** → rd vms-8bd
INETD inits its PCB with `vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)` (`vms_tcpip_inetd.c:95`) → `cur_privs = perm_privs = 0xFFFF…FFFF` (**all privilege bits**), `uic = 0` (**SYSTEM [0,0]**) (`vms_pcb.c:41,61-62`), started `RUN/DETACHED` from `TCPIP$STARTUP.COM:55`. The spawn is raw `fork()+execv()` (`tcpip_inetd.h:334,346`) with **no setuid/setgid/persona/privilege-drop anywhere** — a grep of the surface returns only the header comment admitting per-service run-as identity is "deferred" (`tcpip_inetd.h:70`). **The launched service inherits INETD's full SYSTEM identity and every privilege.** **This is WORSE than the DECnet twin:** DECnet's inbound CTERM passes `PRC$M_LOGINOUT` so `LOGINOUT` authenticates the remote user and the session starts with **no identity**; INETD has **no equivalent gate** — the service *is* SYSTEM from its first instruction. **Consequence:** a memory-corruption bug in *any* enabled service image, or in INETD's own parse, executes **as SYSTEM with all privileges** — no defense-in-depth if the (strong) bounds controls fail. This is the ceiling on the boundary. Remediation: a per-service run-as identity + drop privileges before the service `execv`; optionally seccomp the child.

### G2 — Connection-flooding DoS: fork-per-connection, no MAXCHILD cap, no rate-limit, no idle timeout **(CONFIRMED — structural)** → rd vms-bb4
The accept loop dispatches one `fork()+execv()` per inbound connection (`vms_tcpip_inetd.c:174-191` → `tcpip_inetd.h:363-380`). Reaping is `waitpid(-1,…,WNOHANG)` (`:189`) — **non-blocking, with NO live-child counter and NO MAXCHILD ceiling**; `listen` backlog is 5. **What a hostile client achieves:** for a write-and-exit service (DAYTIME) it is spawn-rate / PID churn (children exit promptly); for **any reading/slow service under slowloris** (the future SSH rung), children **accumulate unbounded → PID/memory exhaustion — a fork-bomb-via-network.** INETD imposes **no concurrency ceiling of its own** — it relies entirely on each service self-limiting (OpenSSH's `MaxStartups`). **INETD itself does not crash** (fixed buffers, no retained per-connection listener state). **Denial is spawn-storm/resource, not an INETD crash.** Remediation: a MAXCHILD concurrency cap + per-source connect rate-limit + accept back-pressure.

### G3 — No fuzz coverage of the wire-reachable decoders **(MEDIUM — hardening)** → rd vms-deb3
No coverage-guided/mutation fuzzing of the bgsock recv path or the SERVICE.DAT parser (only `test_syssvc_tcpip_daytime.c`, a functional proof). **Contrast:** DECnet shipped 5 sanitizer-fuzzed decoders (60k–200k iters). **Mitigant:** the TCP/IP byte path is far simpler (fixed 4096 buffers, no nested TLV), memory-safe by inspection at every layer (§2), and the enabled-today service reads nothing — residual risk materially lower than DECnet's. Still a real, cheap gap; close before enabling any service that *reads* client bytes.

---

## Areas to state in posture (honest limitations, not bugs)

- **A1 — INETD is a dispatcher, not an authentication boundary.** Each launched service authenticates itself or not (DAYTIME public-by-design; a wrapped sshd self-authenticates). INETD grants the connection with **no auth of its own** — safe only while every enabled service is public-by-design or self-authenticating.
- **A2 — The config store is local, not wire.** `TCPIP$SERVICE.DAT` / mgmt `.DAT` parsing is operator/config input, not attacker-reachable; the parser is bounds-safe (§2). `tools/vms_tcpip_config.c` is an interactive operator wizard (bounded `fgets`/`sscanf`, `execv` of `vmsdcl`) — operator-local, out of the inbound-wire scope.
- **A3 — Dormant-by-default is the current de-facto boundary** (§0) — the strongest control on main today is that the surface is off and CI-guarded.
- **A4 — Cleartext transport.** DAYTIME and Phase-IV-era inetd services provide no wire confidentiality/integrity by design (SSH excepted). A protocol property, not an OVMX defect, but it bounds any claim.

---

## 4. Posture recommendation (input to Baron's R4 ruling — his reserved call)

The inbound TCP/IP **byte path is genuinely hardened at the memory-safety layer** (triple-bounded, allocation-free, honest-failing), with a real config/wire separation (the client cannot influence *what* is launched) and a real pre-flight. **What OVMX's TCP/IP surface holds today is a memory-safety + config-integrity boundary that ships DORMANT-by-default.**

The honest limits that define the edge:
- **(a) No OS privilege isolation, and services run as SYSTEM/all-privs (G1)** — *worse* than the DECnet twin, which resets identity via LOGINOUT.
- **(b) No DoS resistance in the aux server (G2)** — spawn-storm; a network fork-bomb the moment a *reading* service is enabled.
- **(c) INETD is a dispatcher, not an auth boundary (A1).**
- **(d) No fuzz coverage (G3)** — lower residual risk than DECnet given the simpler path.

**Recommended framing (composes with the DECnet ruling; Baron chooses):**
1. **"OVMX's TCP/IP surface is a memory-safety + config-integrity boundary that ships DORMANT-by-default; it is NOT a privilege-isolated, DoS-resistant, or authenticating boundary. Enabling any inbound service is an explicit operator posture decision."** — precise, defensible, matches the code + the CI guard. **Recommended for 1.0**, with G1/G2/G3 as the raise-the-boundary backlog and a **HARD GATE: G1 (per-service run-as + privilege drop) and G2 (concurrency cap / rate-limit) MUST land before any *reading* or privileged service — notably the SSH rung — is enabled.** DAYTIME-only exposure is tolerable under option 1 because it reads nothing and needs no auth.
2. **"OVMX's TCP/IP surface is a hardened security boundary"** — then G1 + G2 must be remediated first, G3 before any reading service.

**Faithfulness note (as for DECnet):** real VMS `TCPIP$INETD` also spawns services and does not seccomp-sandbox them; option 1 matches what real VMS provides rather than gold-plating past it. The *dormant-by-default* posture is, if anything, *stronger* than a stock TCP/IP kit.

**Composition with the DECnet posture:** both inbound surfaces are **memory-safe but not privilege-isolated**. DECnet adds a **fresh-LOGINOUT identity reset**; TCP/IP adds **dormant-by-default** but a **runs-as-SYSTEM launcher**. If Baron takes option 1 for both, the whole R4 networking ruling reads: *"OVMX's inbound networking is a memory-safety boundary with real authentication (DECnet) / config-integrity (TCP-IP), NOT a privilege-isolated or DoS-resistant one; TCP/IP additionally ships off by default, and enabling a reading/privileged service is gated on G1+G2."*

---

## 5. Filed findings (report-not-fix)

| rd | Gap | Severity | Gate |
|---|---|---|---|
| vms-8bd | G1 services run as SYSTEM/all-privs, no persona/privilege-drop | posture-defining | **before SSH / any reading or privileged service** |
| vms-bb4 | G2 INETD fork-flood DoS (no MAXCHILD/rate-limit) | medium (spawn-storm) | **before SSH / any reading service** |
| vms-deb3 | G3 no fuzz coverage (bgsock recv, SERVICE.DAT parse) | low-medium | before a client-reading service |
