# a1 contract — the DECnet `_NET:` `$QIO` broker (Option 1, NETACP-brokered)

**Status:** design/contract (rd vms-dda, the client-side `_NET:` `$QIO` seam; parent
vms-8ade/vms-30e). Option 1 (NETACP-brokered) already ruled by the conductor.
This pins the transport MECHANISM within Option 1, the `$QIO` function-code
contract, and the CI-testable sub-rung order. Grounded on a full seam map of
main@940031ff (file:line anchors inline). One architecture decision is teed up
for the conductor/Baron (§3).

## 0. What ships above this (the target)

A user process runs, VMS-native:
```
  $ASSIGN "_NET:", chan
  $QIO chan, IO$_ACCESS, iosb, p1=<NODE::"TASK=name" descriptor>    ! open the link
  $QIO chan, IO$_WRITEVBLK, iosb, p1=buf, p2=len                     ! send a message
  $QIO chan, IO$_READVBLK,  iosb, p1=buf, p2=len                     ! receive
  $QIO chan, IO$_DEACCESS,  iosb                                     ! disconnect
  $DASSGN chan
```
The connect descriptor is exactly what vms-dda rung 1 already builds/parses
(`dnet_cterm_sc_connect_build_task` / `_parse`, format-1 NAMED). The NSP link FSM
that carries the bytes is the proven `dnet_engine_link_*` engine, which lives in
the NETACP daemon (`decnetd.c`) — Option 1: the protocol FSM stays in NETACP; the
executive owns only the device face + the broker.

## 1. Seam map (grounded on main@940031ff)

- **`_NET:` device is real + generic-assignable.** `vms_devtab_probe_net`
  (`src/kernel-core/vms_devtab.c:1004`, called `:1172`) registers `NET:` as class
  `DC__SCOM`, `shareable=1`, gated on the primary NIC (`ETH0:`) — no NIC, no
  `_NET:` (`SS$_NOSUCHDEV`, INV-6). The executive `VMS_IOCTL_ASSIGN`
  (`vms_ioctl_assign`, `:1427`) already resolves `NET:` generically and hands back
  a `struct vms_channel`; `$GETDVI _NET:` works cross-process today
  (`docs/compatibility-surface.md`, the `decnet$netacp-device-face` row).
- **The `NET:` row carries NO per-link state.** `struct vms_device`
  (`src/kernel/vms_internal.h:936`) has class/shareable/`netif`/`link_up` only —
  no logical-link/NSP-handle table. New exec-resident state is required.
- **No `$QIO` `_NET:` path.** `sys_qio.c` classifies mailbox → BG → terminal →
  local fd (`src/libvms/syssvc/sys_qio.c:605-614`); a fd=-1 `_NET:` channel would
  hit `qio_validate_and_classify` and fail `SS$_IVCHAN` (`:434`). A `qio_net_op`
  classifier + handler must be added, mirroring `qio_bg_op` (`:311`).
- **No userspace `$ASSIGN _NET:`.** `resolve_vms_device` (`sys_assign.c:105`) has
  no `NET:` case, so `$ASSIGN _NET:` through libvms returns `SS$_NOSUCHDEV` even
  though the executive device is real. Needs an `is_net` branch mirroring `is_bg`
  (`:437-468`).
- **THE CORE GAP — no exec↔NETACP transport.** NETACP owns the raw datalink and
  polls it in its own `poll()` loop (`decnetd.c:3301`, `scs_datalink_recv`); the
  object-42 dispatch is NETACP-driven, never executive-woken. There is **no**
  primitive by which the executive hands NETACP a user `$QIO` request, and none by
  which NETACP wakes/completes one. This is the design's real work.

## 2. Reference model — NETACP is a network ACP

The faithful VMS framing: **`_NET:` is an ACP-served device and NETACP is its
ACP.** On real VMS, `$QIO` to an ACP-served device queues the request to the ACP
(via its AQB/mailbox); the ACP services it and posts the I/O completion. OVMX
already has this exact shape for files: the ODS-2 ACP path
(`vms_kif_acp_access/deaccess/readvb/writevb`, `vms_kif.h:1237-1252`). The
`_NET:` broker is the **DECnet ACP** analogue:

```
 user process            executive (vms.ko)                 NETACP (decnetd.c)
 ------------            ------------------                 ------------------
 $QIO IO$_ACCESS  --->   qio_net_op / vms_kif_net_access
                          enqueue request on the NET link  --- broker transport -->
                          request queue; mark IOSB pending      dequeue; run
                                                                 dnet_engine_link_open
                                                                 over the datalink
                          complete the $QIO (AST/IOSB)     <---  post completion
 $QIO IO$_WRITEVBLK --> vms_kif_net_send  --> enqueue     --->  dnet_engine_link_send
 $QIO IO$_READVBLK  --> vms_kif_net_recv  --> (park)      <---  deliver rx segment
 $QIO IO$_DEACCESS  --> vms_kif_net_deaccess              --->  dnet_engine_link_close
```

Per link there is one exec-resident **link handle** (a small record in new
`NET:`-device state: state, owner, the exec↔NETACP correlation id, rx/tx queues)
— the DC$_SCOM analogue of the mailbox message queue / BG socket endpoint.

## 3. ARCHITECTURE DECISION (teed up for the conductor/Baron)

The transport mechanism carrying requests exec→NETACP and completions back:

- **(T1) A dedicated executive mailbox (`SYS$NET`-shaped), reusing `vms_mbx.c`.**
  NETACP adds the mailbox fd to its existing `poll()` loop; the executive `_NET:`
  `$QIO` writes a request record to it and parks the IOSB; NETACP replies on a
  companion mailbox and the executive completes the I/O. **Pros:** VMS-authentic
  (real DECnet task-to-task literally uses `SYS$NET` mailbox association); reuses
  shipped exec-resident mailbox infra (quota, blocking read, write-attention AST,
  `vms_kif_mbx_*`); NETACP's poll loop already multiplexes fds. **Cons:** two
  message framings (mailbox record ⇄ link op); correlation ids to match
  completions to waiters.
- **(T2) A new exec-resident link table (new `vms_net.c`) + `$WAKE`/AST, mirroring
  `vms_bg.c`.** **Pros:** `$QIO`→`vms_kif_net_*` maps 1:1 like BG. **Cons:** the
  NSP endpoint is NOT in the executive (it is in NETACP), so this STILL needs an
  exec↔NETACP hop — it does not remove the core gap, it just renames it, and it
  invents a bespoke queue where the mailbox already exists.

**Recommendation: T1 (mailbox-backed network ACP).** It is the faithful shape,
reuses shipped infrastructure, and needs no new kernel queue primitive — only the
`_NET:` device front-end (`qio_net_op` + a link handle) plus wiring the request
records onto a `vms_mbx` pair NETACP already knows how to poll. Ratify T1 (or
direct otherwise) before the transport sub-rung (a1b) starts. The device
front-end sub-rungs (a1-0, a1-1) do not depend on this choice and can proceed.

## 4. `_NET:` `$QIO` function-code contract

All function codes already exist in `iodef.h`; no new codes. Modifiers TBD per
op. `p1..p6` and IOSB semantics:

| `$QIO` func | direction | p1 | p2 | IOSB / completion | broker op |
|---|---|---|---|---|---|
| `IO$_ACCESS` | outbound connect | connect descriptor buffer (NAMED task / object) | descriptor length | `SS$_NORMAL` when the link reaches RUN (CC in), or `SS$_REJECT`/`SS$_LINKDISCON` | `dnet_engine_link_open` + pump to RUN |
| `IO$_ACCESS \| IO$M_ACCEPT` | inbound accept | (out) peer connect descriptor | buf len | `SS$_NORMAL` on accept | `dnet_engine_link_accept` |
| `IO$_WRITEVBLK` | send | data buffer | length | bytes sent | `dnet_engine_link_send` (one NSP data segment) |
| `IO$_READVBLK` | receive | (out) data buffer | max length | bytes received; `SS$_LINKDISCON` if peer closed | `dnet_engine_link_rx` → `rx_data` |
| `IO$_DEACCESS` | disconnect | reason (opt) | — | `SS$_NORMAL` | `dnet_engine_link_close` |

Bounds: every buffer length is validated against the segment cap
(`DNET_NSP_MAX_DATA`) before the broker enqueue; the descriptor is parsed by the
already-fuzz-clean `dnet_cterm_sc_connect_parse` at low privilege (the A2/A8
isolation discipline, `decnet$wire-isolation`) — the privileged completion path
takes only a validated typed record. INV-6: no `/dev/vms` ⇒ honest `SS$_NOSUCHDEV`;
no NETACP ⇒ honest `SS$_DEVOFFLINE`, never a fabricated link.

## 5. Decomposed sub-rungs (CI-testable order)

- **a1-0 — userspace `$ASSIGN _NET:` (the non-blocker; do FIRST).** Add the
  `is_net` case to `resolve_vms_device` (`sys_assign.c`) routing to the existing
  generic executive assign; a user process `$ASSIGN _NET:` returns a channel and
  `$GETDVI _NET:` resolves the real `DC$_SCOM` device cross-process. **Test:** the
  booted-battery / kmod cross-process tell (the RTAn: pattern) — no NSP brokering
  needed, exercises against `/dev/vms` today. Bounded; no new ioctl (reuses
  `VMS_IOCTL_ASSIGN`). *This is the immediate next PR.*
- **a1-1 — the `NET:` link handle + `qio_net_op` skeleton.** New exec-resident
  per-link state on the `NET:` device; `vms$$chan_is_net` classifier + `qio_net_op`
  in `sys_qio.c` returning honest `SS$_DEVOFFLINE` until the transport lands.
  **Test:** host unit on the classifier + a kmod ctest that `$QIO _NET:` reaches
  `qio_net_op` and fails honestly (INV-6), not `SS$_IVCHAN`.
- **a1-2 — the exec↔NETACP transport (needs §3 ratified).** The mailbox-backed
  request/response (T1); NETACP adds it to its poll loop and services one op
  (start with `IO$_ACCESS` outbound). **Test:** a `--net-broker-selftest` host
  floor (a mock exec side + NETACP servicing a queued connect over a socketpair
  datalink, byte-verified — the `--copy-transport-selftest` pattern), then a
  booted `$QIO IO$_ACCESS _NET:` reaching a real `dnet_engine_link_open`.
- **a1-3 — the data plane + disconnect.** `IO$_WRITEVBLK`/`IO$_READVBLK`/
  `IO$_DEACCESS` over the transport, with the full 4-enum kif wiring
  (`VMS_IOCTL_NET_*`: Linux `vms_ioctl.h` + `vms_module.c` ×2, NetBSD
  `vms_acp_nb.h` + `vms_netbsd.c` ×2; client `vms_kif.h`/`vms_kif.c`; shr.vec
  `libvmssys_shr.vec`) — the checklist the memory warns about. **Test:** a booted
  two-process task-to-task round-trip (`$QIO` writer ⇄ `$QIO` reader over
  `_NET:`), the cross-process byte-verified tell.
- **a1-4 — lab (vms-101-gated):** interop with a real VAX task-to-task pair.

## 6. Wiring checklist (for a1-3, from the seam map)

New `VMS_IOCTL_NET_*` ioctls: number + `_Static_assert` in **Linux**
`src/kernel/vms_ioctl.h` AND **NetBSD** `src/kernel-netbsd/vms_acp_nb.h`; dispatch
in **Linux** `src/kernel/vms_module.c` (outer case-list + inner switch = ×2) AND
**NetBSD** `src/kernel-netbsd/vms_netbsd.c` (outer + inner = ×2) — FOUR enum-case
listings total; client prototype `src/libvmssys/vms_kif.h` + `KIF_CALL` wrapper
`src/libvmssys/vms_kif.c`; symbol vector `src/vmslink/libvmssys_shr.vec`
(append-only). All-`_IOWR` to dodge the Alpha `_IOR/_IOW` divergence trap
(`vms_ioctl.h:31-52`).
