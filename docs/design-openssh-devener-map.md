# OpenSSH as a DE-VENEER map of the OVMX VMS-compat surface

> **LADDER RECONCILE (operator 2026-08-22, Rule 1 / `vms-ports-build-ladder`):**
> this map is exactly the right shape. The deliverable is *"the OpenVMS OpenSSH
> port builds on OVMX over a VMS-faithful DECC\$SOCKET / \$QIO(BGn:) surface +
> TCP/IP services (`vms-67f`)"* (`vms-9ef`) — build the compat ladder **up** until
> the real VMS OpenSSH port links unchanged, never hand-roll a native SSH server
> or minimally-adapt a Linux daemon down to an ad-hoc surface. The Tier A
> de-veneer (`vms-4bf`, full DECC\$SOCKET surface so **unmodified** OpenSSH
> dispatches every fd call to \$QIO) **is** the correct rung — keep and extend it
> (`vms-99f`, `vms-9ac`, server path `vms-698`). Do not discard.

> **STATUS (2026-08-15):** **Tier A LANDED** — the real DECC$SOCKET surface
> (`getpeername`/`getsockname`/`setsockopt`/`getsockopt`/`fcntl O_NONBLOCK`)
> resolves against the executive-resident kernel socket via `$QIO`
> (`VMS_IOCTL_BG_GETNAME`/`BG_SOCKOPT`), proven 14/14 against a real `/dev/vms`
> (`tests/qemu/test_syssvc_bgsock_peername.c`, vms-4bf / PR #601). The
> anti-veneer bar is met: `getpeername` returns the true remote IP; `setsockopt`
> is honored, not swallowed. **Tier A.2 (vms-9ac) remains:** flip the KEX build
> to `OVMX_WRAP=1` default, re-prove end-to-end key exchange over the wrapped
> `$QIO` path (assert no AF_UNIX fd), then delete the socketpair+pump adapter
> and the 2 `OVMX_VENEER` patches — the veneer is not gone until that lands.

> **STATUS (2026-08-28): Tier A.2 LANDED (`vms-9ac`).** The veneer is GONE. The
> OpenSSH source is now UNMODIFIED — `socket()`/`connect()`/`read()`/`write()`/…
> reach the executive purely via the linker's `--wrap` (`ovmx_ssh_wrap.c`,
> `socket`+`connect` added so no `sshconnect` source patch is needed;
> `timeout_connect` treats `connect()==0` as immediate success, so a blocking
> `__wrap_connect` suffices). **DELETED:** `ovmx_ssh_glue.c/.h` (the AF_UNIX
> socketpair+pump) and both `OVMX_VENEER` patches. The `--wrap` set is applied via
> `SSHLIBS` to the `ssh` client ONLY, so `sshd` stays a stock real-socket server.
> Re-proven end-to-end in CI: `test_syssvc_ssh_kex.c` runs the wrapped KEX AND
> asserts (hard-fail) NO AF_UNIX socket fd in the ssh process (`/proc/<pid>/fd`
> vs `/proc/net/unix`); `run_ssh_build.sh` asserts the `__wrap_*` dispatch is
> linked and no retired `ovmx_ssh_*` glue shim survives. The sshd path now carries
> real bytes over the executive socket seam with **zero fabrication**.

> **Task:** COMPAT-SURFACE MAPPING (read/analyze only; the conductor files rungs).
> **Operator directive (2026-08-15):** "Use SSH as a compat goal. Use the VMS SSH
> port to map and build the surface we're missing — it's a DE-VENEER exercise,
> not a veneer celebration."
> **Analysis basis:** `origin/main` (SSH arc tip `f50a0284`, #579 — the real-KEX
> proof). Every line cite is against `origin/main` unless noted.
> **Clean-room posture (operator 2026-08-15, authoritative):** the VSI/HPE
> OpenVMS OpenSSH port is **GPL/OSS** and is now an **in-bounds reference** to
> READ for surface-mapping, exactly like the OpenSSH-portable tree. Rule 8 still
> bans PROPRIETARY VSI/HPE internals (cluster wire, executive, closed binaries).
> **Downstream nuance (flagged, not a mapping blocker):** reading the GPL port to
> *map* is fine; when OVMX later *builds* the surface we write OVMX's own
> implementation informed by the map — see §7 for the copyleft-tempting spots the
> conductor must handle at build time.

---

## 0. The reframe, stated precisely

`ssh` completes a real SSH-2 KEX on OVMX today (`test_syssvc_ssh_kex.c`, proof
#579). It does so the **wrong** way for a VMS distribution: it is a **bare Linux
static-musl ELF** (`third-party/openssh/build-openssh.sh` links `LDFLAGS=-static`,
no `PT_INTERP`, no `.vms$sv`), and inside it an **AF_UNIX socketpair + two byte-pump
threads** (`third-party/openssh/ovmx/ovmx_ssh_glue.c`) bridge unmodified OpenSSH
to the executive so **no OpenSSH code ever touches a VMS facility directly**. That
is a veneer: a facade that lets Unix software bypass VMS. The goal is the inverse
— make `ssh` a genuine OVMX image whose every OS touch is a real VMS-compat
facility, and read the list of those touches as the compat surface OVMX must build.

### 0.1 The three cheats, cited

| # | Cheat | Where | Why it exists |
|---|-------|-------|---------------|
| **1** | **AF_UNIX socketpair + 2 pump threads** shuttle every wire byte between OpenSSH's fd and the executive BGn: socket | `ovmx_ssh_glue.c:ovmx_ssh_connect` (`socketpair(AF_UNIX,…)` → `pump_out`/`pump_in` calling `ovmx_send`/`ovmx_recv`) | OpenSSH treats the connection fd as a **full BSD socket**: it does raw `read()/write()` on it *outside* the packet layer (the SSH banner exchange, `kex.c:1222/1262/1314` `atomicio(vwrite/read, connection_out/in, …)`), plus `getpeername()` (`packet.c:451/456/558`) and `setsockopt()` (`sshconnect.c:534` SO_KEEPALIVE, `misc.c:206/307` TCP_NODELAY/IP_TOS). A veneer **handle** is not an fd, and the executive **readiness fd** (`ovmx_pollfd`) has **no read/write file-ops** — so the banner atomicio fails EBADF and getpeername/setsockopt fail ENOTSOCK. The socketpair is a real, read/write-able fd, so all of that "works." |
| **2** | **Bare Linux static ELF**, not a VMS image | `build-openssh.sh` (`-static`, musl); `build-ssh-kex-harness.sh` | The port proved the transport substitution first; IMGACT packaging is listed as a *later* continuation (`design-openssh-port-ovmx.md §7.3` item 2). The binary is not `PT_INTERP`-activated by `IMGACT.EXE`, carries no `.vms$sv`/`.vms$imp` symbol vector, is not linked against `DECC$SHR`, and is not a DCL-activatable `SSH.EXE`. |
| **3** | **Host `open()`/`fopen()`** for config/known_hosts/keys, not RMS | OpenSSH `hostfile.c:291/523/936`, `readconf.c:2576`, `authfile.c:126/181/217/388/500`; paths `pathnames.h` (`SSHDIR "/ssh_config"`, `"~/.ssh/known_hosts"`) | Unmodified OpenSSH does POSIX file I/O on Unix paths. On OVMX those calls go to the host FS (musl syscalls direct, or the C-RTL host-FS passthrough), never RMS-over-ODS-2 with VMS filespecs. |

Note the socketpair is **already degraded**, not merely inelegant: on an AF_UNIX
fd `getpeername()` returns `AF_UNIX` (so `ssh_remote_ipaddr`, `packet.c:548`,
falls to its fallback — the remote IP used for known_hosts matching and logging is
wrong) and `setsockopt(TCP_NODELAY/IP_TOS/SO_KEEPALIVE)` returns `ENOPROTOOPT`
(swallowed as non-fatal). OpenSSH's `ssh_packet_connection_is_on_socket()` only
passes because of its `connection_in == connection_out → return 1` shortcut
(`packet.c` ~L515), never reaching the getpeername probe.

---

## 1. The gap list — VMS-compat facilities `ssh` requires that OVMX does not genuinely provide

Ranked per the operator: **(A) what kills the socketpair/pump**, then **(B) IMGACT
packaging**, then **(C) RMS config I/O**, then **(D) the rest**. "Current OVMX
state": *absent* / *partial* / *veneered* (a fake/bypass exists). Effort is
relative (S ≤ ~1 session, M a few, L a lane).

### Tier A — kill the socketpair/pump (the headline de-veneer)

The socketpair exists because the connection fd must be, simultaneously:
read/write-able (banner), `getpeername`-able as AF_INET, `setsockopt`-able for
TCP options, and poll/select-able. The veneer today provides only
send/recv/connect/close + a readiness fd. The missing socket surface:

| Gap | OpenSSH code that needs it | Public VMS API contract | OVMX state | Effort |
|-----|---------------------------|-------------------------|-----------|--------|
| **A1. `getpeername` / `getsockname` over BGn:** | `packet.c:451/456/558` (`ssh_remote_ipaddr`, `is_on_socket`); `misc.c:284` `getsockname` | VSI TCP/IP Services: `$QIO IO$_SENSEMODE` returns the socket's local/peer `sockaddr` (Sockets-API / QIO interface, Programming Concepts manual) | **absent** — `vms_bgsock.h` has no `ovmx_getpeername`/`ovmx_getsockname`; no `vms_kif_bg_getpeername`; the socketpair returns AF_UNIX | **S–M** (add a kif op → `kernel_getname`/`kernel_getpeername` on `bs->sock`; veneer wrapper) |
| **A2. Raw read/write of the connection fd routed to $QIO** (the **banner exchange**, which is *outside* the patched packet layer) | `kex.c:1222/1262/1314` `atomicio(vwrite/read, connection_out/in, …)` | The socket the C RTL hands the app **is** a channel; `read()/write()` on it issue `IO$_READVBLK/IO$_WRITEVBLK` under DECC$SOCKET | **veneered** — data moves through the socketpair + pump, i.e. through libc `read/write` on an AF_UNIX pipe, *then* `ovmx_send/ovmx_recv`. The wire byte does reach $QIO, but only after a Unix hop | **M** (see §2.1 — the fd-dispatch layer or a full-socket fd) |
| **A3. `setsockopt`/`getsockopt` (SO_KEEPALIVE, TCP_NODELAY, IP_TOS, SO_ERROR) over BGn:** | `sshconnect.c:534`; `misc.c:196/206/307/427`; `channels.c:2047` | `$QIO IO$_SETMODE`/`IO$_SENSEMODE` with the socket-option subfunction | **absent** — no `ovmx_setsockopt`/`getsockopt`; degraded to ENOPROTOOPT on the socketpair | **S–M** (kif op → `sock_setsockopt`/`sock_getsockopt` on `bs->sock`) |
| **A4. The fd-dispatch mechanism** — the thing that lets OpenSSH's *unmodified* fd calls hit the veneer instead of libc, so no socketpair is needed | all of the above + `clientloop.c` poll set | On VMS this is DECC$SHR: the C RTL **owns socket fds** and routes each fd call to $QIO. There is no host socket to bypass to | **veneered** — today the map is per-connection fd→handle (`ovmx_ssh_glue`) and only 4 sites are routed (`sshconnect`+`packet` patches); everything else uses the socketpair's real fd | **M** (linker `--wrap`, §2.1) |
| **A5. Non-blocking mode + poll/select composition over veneer fds** | `clientloop.c`/`ssh.c` set the connection non-blocking and `poll()` a mixed fd set (connection + stdin/stdout) | C RTL `fcntl(O_NONBLOCK)` + `select()/poll()` over socket channels (async `$QIO`/AST underneath) | **partial** — the readiness fd (`ovmx_pollfd` / `VMS_IOCTL_BG_POLLFD`, `vms_bg.c:99`) makes a BGn: socket select()-able, but the veneer send/recv are **blocking only** and there is no `ovmx_fcntl`/non-blocking send/recv | **M** |

### Tier B — package `ssh` as a genuine VMS image (IMGACT), not a bare ELF

| Gap | What a VMS image needs | Public VMS contract | OVMX state | Effort |
|-----|------------------------|---------------------|-----------|--------|
| **B1. Link against `DECC$SHR`, not static musl** | `ssh`'s C-RTL calls (stdio, string, `socket/connect/read/write/poll/getpeername/setsockopt`, `open/fopen/stat/getpwnam/…`) resolve through the shareable image's `.vms$sv` symbol vector | VMS images bind DECC$SHR at activation via the symbol vector | **partial** — `DECC$SHR` exists (`src/vmslink/mk_decc_shr.sh`, ~102+ universals incl. an `inet_*/socket` set), but (a) it does **not** export every C-RTL universal OpenSSH uses, and (b) its `socket/connect` map to **raw musl/Linux sockets, not the BGn: veneer** — a genuine VMS `ssh` needs `socket()` to be DECC$SOCKET-over-BGn: | **M–L** (extend the vector — heed the "new RTL call → DECC$SHR symbol vector" + append-only GSMATCH rules; wire `socket/connect/...` to the veneer) |
| **B2. Build as static-PIE `PT_INTERP` image activated by `IMGACT.EXE`** | ELF with `PT_INTERP=…/SYSEXE/IMGACT.EXE`, `.vms$imp` import vector, DT_NEEDED on DECC$SHR (+libcrypto) | `SYS$IMGACT` maps the image + shareables, resolves the GST/symbol vector, checks GSMATCH (`design-image-activation.md`) | **absent** — current `ssh` is `-static`, no interpreter, no import vector | **M** (apply the native-link toolchain — `src/vmslink`/`LINK.EXE` — to the OpenSSH object set) |
| **B3. `LINK.EXE` resolves the OpenSSH object set + emits the image** | LINK.EXE consumes the `ssh` objects, binds DECC$SHR's vector, statically pulls libcrypto | VSI Linker Utility (public) | **absent for a program this size** — LINK.EXE proven on OVMX's own images + tcc, not yet a large third-party C program | **M–L** |
| **B4. DCL activation as `SSH.EXE`** | `$ SSH host` activates the image; foreign-command / DCLTABLES entry | DCL image activation | **absent** — no DCL path invokes the binary; note the "new cross-image symbol → THREE DCL enumerations" gotcha | **S–M** |
| **B5. libcrypto as a bindable dependency** | LibreSSL/`libcrypto` linked into the image | n/a (OSS) | **partial** — vendored **static** cross-build exists (`third-party/libcrypto/build-libcrypto.sh`); static-into-the-image is acceptable, a DECC$-style shareable is optional later | **S** (reuse) / L (if made a shareable) |

### Tier C — config / known_hosts / keys through RMS

| Gap | OpenSSH code | Public VMS contract | OVMX state | Effort |
|-----|-------------|---------------------|-----------|--------|
| **C1. File I/O via the C RTL → RMS sequential record files** | `hostfile.c` (known_hosts), `readconf.c` (ssh_config), `authfile.c` (keys/authorized_keys) — all `open()/fopen()` | DECC$SHR `open/fopen` map to RMS `$OPEN/$CONNECT/$GET/$PUT` on sequential files; C RTL does the record I/O | **veneered / inherited-gap** — becomes RMS-genuine only once (a) `ssh` links DECC$SHR (B1, so its I/O uses the C RTL not musl syscalls) **and** (b) the C-RTL file path is real RMS-over-ODS-2, which is the standing **ODS-2 passthrough gap** (`vms-5eb`; live `SYS$DISK` is host-FS passthrough, MEMORY [[ods2-runtime-passthrough-gap]]) | **inherited** — the SSH-specific part is **S** (VMS filespecs + logicals, C2); the RMS-genuineness is the ODS-2 lane, **not** an SSH problem |
| **C2. VMS filespecs + logical names for the SSH file tree** | `pathnames.h` (`SSHDIR`, `~/.ssh`) | VSI: `SSH2$DIR`/`SYS$LOGIN:[.SSH2]…` (or `TCPIP$SSH_*`) VMS filespecs + `TCPIP$*`/`SSH*` system logicals | **absent** — config lives as Linux files under `distro/rootfs/etc/ssh` | **S** (config placement + logical-name defs; RMS filespec ↔ path mapping) |

### Tier D — the rest (breadth for a real service, most already tracked)

| Gap | Need | OVMX state | Effort / owner |
|-----|------|-----------|----------------|
| **D1. DNS resolver — real `getaddrinfo`/`gethostbyname`** | `ssh host` (non-numeric) | **partial** — veneer has numeric-IPv4 only (`ovmx_getaddrinfo_numeric`); DNS deferred | M (TCP/IP Services BIND resolver, `design-tcpip-services-ovmx.md §5`) |
| **D2. BGn: SERVER path — bind/listen/accept (for `sshd`)** | inbound `sshd` | **absent** — `ovmx_bind/listen/accept` = honest `ENOSYS`; `vms_bg.c` client-only | L — item `vms-698` |
| **D3. Executive remote-terminal device (TNA0:/RTA0:)** | `SHOW USERS`/`$GETDVI`/TT: fidelity; `term_map`'s production home | **absent** — `vms_devtab.c` creates only `OPA0:`; `term_map.c` has no caller | M (fidelity, parallel — `design-openssh-port-ovmx.md §2.2 Prereq B`) |
| **D4. `getpwnam/getpwuid` → SYSUAF** (for `sshd`'s account lookup) | `sshd` session launch | **partial/real** — `sysuaf_lookup` exists and the current libssh daemon uses it; must re-home into OpenSSH's `openbsd-compat` auth seam | M |
| **D5. Credential drop to UIC (LOGINOUT)** | `sshd` becomes the user | **DONE** — `cred_drop.c`, fail-closed, unit-tested (closes `vms-49e`) | — (keep verbatim) |
| **D6. `getentropy`/`arc4random` / `/dev/urandom`** | crypto RNG | **partial** — guest `/dev/urandom` works; maps cleanly in the image | S |
| **D7. `TCPIP$*` system logical-name tables + `SHOW DEVICE BG`** cross-process | management + honest device face | **absent/partial** — system-table honesty is the executive bridge (`vms-a7e`/`vms-ln0`) | inherited (networking lane) |

---

## 2. The de-veneer plan for the three cheats

### 2.1 Cheat 1 — socketpair/pump → the veneer hands OpenSSH a genuine VMS channel fd

The connection fd must answer, on one object: raw `read/write` (banner),
`getpeername` (AF_INET peer), `setsockopt` (TCP options), and `poll/select`. Two
ways to get there; they trade fidelity against effort.

**Option B (RECOMMENDED — keeps data on $QIO, faithful DECC$SOCKET model).**
Make the veneer a **complete DECC$SOCKET-equivalent C-RTL socket layer** that owns
socket fds and routes *every* fd call to $QIO, and link OpenSSH so its unmodified
calls dispatch there — no socketpair, no pump, and the `sshconnect`/`packet`
patches collapse into the dispatch layer:

- **Grow `vms_bgsock` to the full surface** (all new kif ops mirror the existing
  `vms_kif_bg_*` and register in `libvmssys_shr.vec` **+ all 6 harness copies = 7
  total**, per the MEMORY gotcha):
  - `ovmx_getpeername`/`ovmx_getsockname` → new `vms_kif_bg_getpeername/getsockname`
    → `kernel_getpeername`/`kernel_getname` on `bs->sock` → `IO$_SENSEMODE`.
  - `ovmx_setsockopt`/`ovmx_getsockopt` → `vms_kif_bg_{set,get}sockopt` →
    `sock_setsockopt`/`sock_getsockopt` (SO_KEEPALIVE, TCP_NODELAY, IP_TOS,
    SO_ERROR) → `IO$_SETMODE`/`IO$_SENSEMODE`.
  - `ovmx_read`/`ovmx_write` = thin aliases of `ovmx_recv`/`ovmx_send`
    (`IO$_READVBLK`/`IO$_WRITEVBLK`) — these carry the **banner** bytes.
  - `ovmx_fcntl(O_NONBLOCK)` + non-blocking recv/send; keep `ovmx_pollfd` for the
    readiness side.
- **Dispatch by fd via linker `--wrap`** (the musl-static analogue of DECC$SHR
  owning the fd namespace): `--wrap=read,write,poll,select,ppoll,getpeername,`
  `getsockname,setsockopt,getsockopt,close,fcntl,shutdown`. Each `__wrap_*`:
  veneer fd (≥ `OVMX_BGSOCK_BASE`) → `ovmx_*` ($QIO); real fd → `__real_*`. For
  `poll/select`, swap veneer fds for their `ovmx_pollfd` readiness fds in the set,
  call `__real_poll`, swap back — composes with OpenSSH's mixed fd set. `socket()`
  returns the veneer handle directly; the `sshconnect`/`packet` patches are
  **removed** (unmodified OpenSSH now works through the wrap layer).
- **Result:** every OpenSSH socket op — including the kex.c banner atomicio and
  getpeername/setsockopt — dispatches to a real `$QIO` on BGn:. No socketpair, no
  pump threads, and the fd OpenSSH sees is genuinely the VMS INET channel.

**Prereqs to build first for Option B:** A1 (getpeername/getsockname), A3
(setsockopt/getsockopt), A5 (non-blocking + poll compose), and the `--wrap`
dispatch layer (A4). All are S–M kernel/veneer additions on the existing
`vms_kif_bg_*` pattern.

**Option A (lower effort, lower fidelity — FLAG).** Have the executive install the
**real host socket** as the userspace fd (`sock_alloc_file`/`sock_map_fd` on
`bs->sock` instead of the readiness anon-inode). Then read/write/getpeername/
setsockopt/poll all work natively because the fd **is** the INET socket, and the
socketpair AND all OpenSSH patches vanish. **But** the app's I/O then bypasses
`IO$_READVBLK`/`IO$_WRITEVBLK` entirely — data no longer transits the executive's
$QIO interface, only the socket object. See §3 FLAG-1: this is the cheaper path but
it surrenders per-I/O executive mediation ($QIO accounting, `TCPIP SHOW
DEVICE_SOCKET` byte counts, any future QIO-layer audit/ACL) — a fidelity
regression from the DECC$SOCKET model, even though the socket stays vms.ko-owned.

### 2.2 Cheat 2 — bare ELF → IMGACT image

Build first: **B1** (DECC$SHR exports the full C-RTL surface OpenSSH uses, with
`socket/connect/...` wired to the veneer), then **B2/B3** (LINK.EXE emits a
static-PIE `PT_INTERP` image with `.vms$imp` binding DECC$SHR + static libcrypto),
then **B4** (DCL `SSH.EXE`). This is "apply the OVMX native-link/self-host
toolchain to a large third-party program"; libcrypto (B5) is reused static.

### 2.3 Cheat 3 — host file I/O → RMS

Build first: **B1** (so `ssh`'s `open/fopen` are C-RTL calls, not musl syscalls),
then **C2** (config/keys under VMS filespecs + `SSH2$`/`TCPIP$` logicals). RMS
*genuineness* (C1) then rides the **ODS-2 passthrough** lane (`vms-5eb`) — see
§3 FLAG-2; it is not an SSH-specific build.

---

## 3. Sequencing — minimal set for a genuine VMS `ssh` doing a real KEX with NO socketpair and NO bare-ELF

```
                    ┌──────────────────────────────────────────────┐
  (independent)     │ vendored static libcrypto  (DONE, reuse)     │
                    └──────────────────────────────────────────────┘

  KILL THE SOCKETPAIR (Tier A, the headline):
    A1 getpeername/getsockname ┐
    A3 setsockopt/getsockopt   ├─►  A4 --wrap fd-dispatch ─►  socketpair+pump DELETED
    A5 nonblock + poll compose ┘        (ovmx_ssh_glue pumps gone; patches collapse)
    A2 read/write→$QIO (aliases of send/recv) rides A4

  PACKAGE AS A VMS IMAGE (Tier B):
    B1 DECC$SHR full C-RTL surface + socket()→veneer
          └► B2 IMGACT PT_INTERP image ─► B3 LINK.EXE emits SSH.EXE ─► B4 DCL $ SSH

  RMS CONFIG (Tier C):
    B1 ─► C2 VMS filespecs/logicals ─► C1 RMS-genuine  (gated on ODS-2 vms-5eb)
```

- **Minimal "genuine VMS ssh, real KEX, no socketpair, no bare ELF" =**
  **A1 + A3 + A5 + A4 (+ A2 aliases)** to kill the socketpair, **then B1 + B2 + B3 +
  B4** to make it an IMGACT-activated `SSH.EXE`. RMS (C) and the rest (D) raise
  fidelity but are not on that minimal path.
- **A-tier is independent of B-tier** and should land first — a genuine-channel-fd
  `ssh` is provable as a bare ELF (upgrade the existing `test_syssvc_ssh_kex.c`
  to assert **no socketpair fd is created** — e.g. no AF_UNIX fd in the process —
  and that the connection fd answers `getpeername` with the AF_INET loopback peer).
- **B-tier depends on B1**, which is the same DECC$SHR-surface work the self-host
  lane already advances; the OpenSSH object set is just a large consumer of it.
- **Clients before server:** everything above is the **client** (`ssh`/`scp`/
  `sftp`), which only connects and rides the DONE BGn: client path. `sshd` adds the
  BGn: **server** path (D2 / `vms-698`) and is a separate, later arc.

---

## 4. FLAG — what cannot be de-veneered without a larger lift

**FLAG-1 — the $QIO-vs-native-fd fidelity fork (Cheat 1).** You can have a genuine
socket *fd* OpenSSH uses directly **or** keep every byte on `IO$_READVBLK/
IO$_WRITEVBLK`, cheaply — not both cheaply. Option A (install the host socket fd)
is the low-effort kill but bypasses $QIO for I/O (loses executive per-I/O
mediation). Option B (full DECC$SOCKET veneer + `--wrap`) keeps $QIO faithfulness
but is the larger build (A1+A3+A4+A5 + the wrap layer). **Recommendation:** Option
B — it is the faithful DECC$SOCKET model and preserves INV-6's "$QIO into the
executive" story that the whole BGn: design rests on. Conductor call if effort must
be cut.

**FLAG-2 — RMS genuineness is not an SSH problem (Cheat 3).** SSH config/known_hosts/
authorized_keys become *truly* RMS-backed only when the C-RTL file path is real
RMS-over-ODS-2. Today live `SYS$DISK` is **host-FS passthrough, not ODS-2**
(`vms-5eb`, operator-gated; MEMORY [[ods2-runtime-passthrough-gap]]). So C1 is
**inherited** from that lane — SSH can place files under VMS filespecs/logicals
(C2) and route I/O through DECC$SHR (B1), but "the file is a genuine ODS-2/RMS
sequential file" is delivered by the ODS-2 lane, not by the SSH port. Do not scope
full RMS genuineness *into* the SSH work.

**FLAG-3 — the multi-threaded channel truth is a real, deferred VMS-faithfulness
debt (bears on A4/A5).** The veneer speaks `vms_kif_bg_*` **directly** (not the
public `sys$assign`/`sys$qiow` path) precisely because the userspace PCB's channel
table is **per-thread** (`vms_pcb.c`, `__thread current_pcb`), so a channel
assigned on one thread is invisible to another — while a socket is a *process*
resource OpenSSH reads and writes from different threads
(`design-bgsockets-veneer-ovmx.md §4`). Making the PCB channel table **process-wide**
(so the *public* `sys$assign`/`sys$qiow` works across threads, as VMS channels do)
is the VMS-faithful long-term fix but has broad blast radius across every `sys$`
service reading `vms_pcb_get()`. Until then, "the app uses the *public* VMS socket
API" is itself partly veneered; the kif-direct path is the correct localized
answer for now. This does not block the socketpair kill, but it caps how
"VMS-native" the socket calls can honestly be claimed to be.

**FLAG-4 — `sshd` (server) is a genuinely larger lift, not a de-veneer of the
current arc.** The BGn: **server path** (bind/listen/accept, `vms-698`, D2) and the
executive remote-terminal device (TNA0:/RTA0:, D3) are net-new executive
facilities, not shims to remove. They are correctly out of the client de-veneer's
minimal path.

**FLAG-5 (copyleft, build-time only — per the operator's downstream nuance).**
Reading the GPL OpenVMS OpenSSH port to *map* the VMS-API surface is in-bounds.
When OVMX *builds* the DECC$SOCKET `$QIO` mapping (A1/A3), the RMS filespec/logical
layout (C2), or the LOGINOUT/terminal glue (D3/D4), a genuine VMS-compat behavior
may tempt a near-verbatim copy of the GPL port's VMS glue. Those spots should be
flagged to the conductor so the license posture (clean reimplementation informed
by the map vs. GPL-inheriting) is a deliberate call, not an accident. This is a
build-phase concern; it does not affect this mapping.

---

## 5. Clean-room provenance (Rule 8)

- **OpenSSH-portable** (BSD/ISC): read and adapted directly — legitimate.
- **VSI/HPE OpenVMS OpenSSH port** (GPL/OSS, per the operator's 2026-08-15 call):
  now in-bounds to READ as the precise VMS-API map (DECC$ usage, TCP/IP Services
  `$QIO`/socket calls, RMS file access, image-activation/DCL, terminal handling).
  When it can be obtained, mine it to confirm the A1/A3/C1/D3 contracts below the
  granularity public docs publish; heed FLAG-5 at build time.
- **Public VSI docs:** TCP/IP Services Programming Concepts (Sockets API + `$QIO`
  interface), the Linker Utility manual, RMS docs — the byte/verb contracts for
  A1/A3, B3, C1.
- **Still forbidden:** proprietary VSI/HPE internals (cluster wire, executive,
  disassembly of closed binaries). None are touched here.
- OVMX design choices stay labeled (the BGn: unit numbering, the `term_map`
  `TERM`→device map) — no VMS-authentic byte-layout claim is made for them.
