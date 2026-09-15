# a1-2 mailbox-seam design — the exec↔NETACP T1 transport wiring

**Status:** design (rd vms-22c, a1-2 integration). Grounds the /dev/vms mailbox
integration on the surface map + the two landed host-tested slices (the broker
record codec #1239, the service dispatch #1246). Written to de-risk the
error-prone executive-integration slice before code. CLEAN-ROOM (Rule 8).

## What is already landed (host-tested, on main)

- **The record** (`dnet_broker_req/rsp`, #1239): bounds-validated decode + the
  correlation-id anti-cross-talk guard. Reuses the executive mailbox message
  (payload ≤1024 ≪ 4096) — **no new ioctl**.
- **The service dispatch** (`dnet_broker_serve`, #1246): one bounds-validated
  request → the NSP link engine (OPEN/SEND/RECV/CLOSE) → a correlation-matched
  response + the wire frame to transmit; never-crash refusals. Proven over a
  socketpair.

The remaining work is the **mailbox plumbing** that carries records between a
user process's `qio_net_op` and NETACP, and the NETACP serve loop that calls
`dnet_broker_serve`. This is inherently /dev/vms-coupled (proof via run-on-rail).

## The routing crux — per-client reply mailbox

A mailbox read is **destructive** (VMS_IOCTL_MBX_READ dequeues), so a single
shared response mailbox that all clients read cannot work: client A, reading the
queue head, would consume client B's response and be unable to put it back.
Filtering-by-correlation on a shared mailbox is therefore impossible.

**Decision: each `$ASSIGN _NET:` channel gets its OWN reply mailbox.** NETACP
owns ONE shared **request** mailbox; each client `$CREMBX`s a temporary **reply**
mailbox and tells NETACP where to answer.

### Topology

```
  user process (qio_net_op)                 NETACP (decnetd)
  -------------------------                 ----------------
  per-channel reply mbx  <--- response ---  writes response to req->reply_unit
        (its own MBAr:)                            ^
                                                   | dnet_broker_serve
  request  ---- vms_kif_mbx_write --->  shared request mbx (well-known name)
                                             read IO$M_NOW on the serve cadence
```

- **Shared request mailbox**: NETACP `vms_kif_mbx_create`s it at startup
  (decnetd.c ≈:3100, after `dnet_engine_init`, before the serve loop) and
  publishes it under an LNM$SYSTEM logical (e.g. `DNET$NETACP_REQ` → `MBAn:`),
  the SYS$NET/ACP-mailbox shape. `maxmsg` raised to hold `DNET_BROKER_REQ_MAX`.
- **Per-client reply mailbox**: `qio_net_op`, on the first op for a channel,
  `vms_kif_mbx_create`s a temporary reply mailbox, records its **unit** on the
  PCB_CHAN_NET channel, and carries that unit in every request.

### Record extension — `reply_unit`

Add `uint32_t reply_unit` to `struct dnet_broker_req` (the mailbox unit NETACP
answers to). Header grows 20→24; update the codec + the #1239 selftest byte
offsets. Response record is unchanged (it is delivered to `reply_unit`, so it
needs no routing field — only `corr_id` + `status` + data). This is additive and
backward-safe: nothing in production consumes the record yet.

## `qio_net_op` flow (the /dev/vms client side)

`qio_net_op` (currently returns `SS$_DEVOFFLINE`) is widened to receive `p1/p2`
(the map flagged both call sites drop them today) and:

1. lazily `$CREMBX` the channel's reply mailbox (once), assign the shared request
   mailbox (`vms_kif_mbx_assign("DNET$NETACP_REQ")`), both cached on the channel;
2. build a `dnet_broker_req` (`corr = dnet_broker_corr_next(per-channel state)`,
   `owner_pid`, `link_handle = exec_chan`, `op` from the IO$ func, `reply_unit`,
   `data` from p1/p2 — for OPEN the `remote_area/node` + the SC descriptor);
3. `dnet_broker_req_encode` → `vms_kif_mbx_write` to the request mailbox;
4. **park** on `vms_kif_mbx_read` (blocking) of the reply mailbox → decode →
   `dnet_broker_corr_match(req.corr, rsp.corr)` (drop + re-read on a mismatch —
   the anti-cross-talk gate) → fill the caller's IOSB from `rsp.status`/data.

`$QIOW` blocks on step 4; `$QIO`'s async form is a later refinement (park via an
AST on the reply mailbox's write-attention). No local fd is ever used.

## NETACP serve-loop flow (the /dev/vms server side)

The ruled transport is **IO$M_NOW mailbox reads on the existing serve cadence**
(decnetd.c serve loop ≈:3172), NOT a `.poll` fop (vms.ko has none) and NOT a
reader thread. Each serve iteration, in addition to the datalink recv:

1. `vms_kif_mbx_read(request_mbx, …, nowait=1)` — drain 0..N pending requests;
2. for each: `dnet_broker_req_decode` (bounds-validated) → `dnet_broker_serve`
   → transmit the returned frame on the datalink → `dnet_broker_rsp_encode` →
   `vms_kif_mbx_write` to `MBA<reply_unit>:`;
3. the datalink recv continues to drive async completions (a CC that brings a
   link to RUN, an inbound data segment) — these are matched to the originating
   channel by the engine's link state; the completion the client parked on
   (an OPEN's RUN, a RECV's data) is answered by a **later** response write once
   the wire event arrives (the serve loop re-checks and replies).

**Refinement (optional, more faithful):** the request mailbox's
write-attention AST can wake the serve loop the instant a request lands, instead
of waiting up to one cadence tick — investigate whether AST delivery interrupts
the loop's `poll`/recv cheaply (`vms_kif_mbx_set_wrtattn` exists); ship the
IO$M_NOW baseline either way.

## Correlation + never-crash invariants (carried from #1239/#1246)

- Every request→response is matched by `corr_id`; a mismatched/stale reply is
  dropped by the client (destructive-read-safe because it is the client's OWN
  reply mailbox — only its own responses arrive, `corr` guards against a
  duplicate).
- Every mailbox record is bounds-validated at decode on BOTH sides before use;
  a malformed record is refused, never faulting the executive or NETACP.
- No fabricated transfer: absent link/engine/mailbox → an honest `SS$_` status.

## Sub-slice order (proof)

- **2a — record `reply_unit` extension** (host-tested: round-trip + the #1239
  selftest offsets updated). Small, host-testable.
- **2b — NETACP $CREMBX + IO$M_NOW serve loop** calling `dnet_broker_serve`.
  Proof: a `--net-mbx-accept-test` on /dev/vms (NETACP reads a request from its
  mailbox, services it, writes the response) via run-on-rail.
- **2c — `qio_net_op` marshalling + reply mailbox**; the e2e proof ($QIO _NET:
  IO$_ACCESS → the chain → a link) on /dev/vms via run-on-rail (a2 test).
- **a1-3 (vms-da1a)** — the data-plane $QIO ops end-to-end; **a1-4 (vms-5b5c)** —
  real-VAX lab interop.
