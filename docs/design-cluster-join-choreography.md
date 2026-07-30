# OVMX cluster-join dir-CLIENT choreography (vms-760, NEW→MEMBER)

**Status:** grounded + byte-verified 2026-07-29; implementation in progress.
**Provenance:** clean-room — `formation-clean-2node.pcap` wire observation + public
OpenVMS docs only (CLAUDE.md Rule 8). Every offset/value below was decoded by
Con.ID pair from the clean 2-node formation (joiner MAC `08:00:2b:94:ca:47`),
**not** trusted from any agent's frame ordinals (those drifted ~1 and mislabeled
frames — the orchestrator re-derived from the wire).

## Why OVMX stalls at NEW

OVMX reaches `SHOW CLUSTER` status NEW by opening only the `VMS$VAXcluster` VC and
sending the add-member burst; the member credits it but never reciprocates. The
grounded reason: a real joiner first acts as a **directory + disk CLIENT** — it
opens its own `SCS$DIRECTORY` connection, **resolves each SYSAP via a lookup before
connecting to it**, and connects `MSCP$DISK` before the VC. The member reciprocates
the add-member config on the joiner VC (→ MEMBER) only once that full connection-set
is present. OVMX presents 0 `VMS$DISK_CL_DRVR` frames vs the clean joiner's 41.

Two OVMX attempts regressed and pinned the mechanism:
- **760b** (own dir connect, then jumped straight to the VC connect): the VC connect
  was presented before its SYSAP was resolved → member only *echoed* it (op=1, lc=0),
  never sent the op=2 response that supplies its handle → VC never bound, retransmit
  forever, never even NEW.
- **760mscp** (MSCP connect with no prior lookup): the unprocessable connect on the
  **shared** per-channel `send_seq` created an in-order **hole** → member's cumulative
  `recv_ack` froze at 2 → the next VC connect was never accepted → below NEW to blank.

**Invariant:** the per-channel `send_seq` is shared across all Con.ID pairs and must
stay **contiguous**; exactly one join-drive frame outstanding at a time; a connect is
issued **only after** its directory lookup returns a HIT; retransmits **reuse** the
frame's `send_seq` (never advance). Any frame the member cannot process at that point
poisons the sequence.

## The dir-CLIENT dialogue (byte-verified, joiner handle `0x4e630007` ↔ member `0xe2dc0008`)

All frames ethertype 0x6007. Absolute offsets (frame byte 0 = Ethernet dst):
`msgtype@30`, `recv_ack@32` (le16), `send_seq@34` (le16), `op@60` (le16),
`remote_conid@64` (le32), `local_conid@68` (le32), `marker@72` (le32),
`name@76` (16B SYSAP), `result@92` (16B).

| step | dir | mt | SCA len | op | seq | rc | lc | name@76 | result@92 | marker@72 |
|------|-----|----|---------|----|----|-----|----|---------|-----------|-----------|
| connect-req | J→M | 0x5b | 110 | 0 | 1 | 0 | `0x4e630007` | `SCS$DIRECTORY` | `SCS$DIR_LOOKUP` | `0x00010000` |
| echo | M→J | 0x5b | 66 | 1 | — | `0x4e630007` | 0 | — | — | `0x00010000` |
| response | M→J | 0x5b | 110 | 2 | — | `0x4e630007` | `0xe2dc0008` | `SCS$DIR_LOOKUP` | `SCS$DIRECTORY` | 0 |
| **confirm** | J→M | 0x5b | **62** | **3** | 2 | `0xe2dc0008` | `0x4e630007` | *(none)* | *(none)* | `0x00010000` |
| lookup TAPE | J→M | 0x5b | 94 | 10 | 3 | `0xe2dc0008` | `0x4e630007` | `MSCP$TAPE` | zeros | 0 |
| resp MISS | M→J | 0x5b | 94 | 10 | — | `0x4e630007` | `0xe2dc0008` | `MSCP$TAPE` | `NOT PRESENT HERE` | 1 |
| lookup DISK | J→M | 0x5b | 94 | 10 | 4 | `0xe2dc0008` | `0x4e630007` | `MSCP$DISK` | zeros | 0 |
| resp HIT | M→J | 0x4b | 94 | 10 | — | `0x4e630007` | `0xe2dc0008` | `MSCP$DISK` | `MSCP$DISK` (echo) | 1 |
| lookup VC | J→M | 0x4b | 94 | 10 | 7 | `0xe2dc0008` | `0x4e630007` | `VMS$VAXcluster` | zeros | 0 |
| resp HIT | M→J | 0x4b | 94 | 10 | — | `0x4e630007` | `0xe2dc0008` | `VMS$VAXcluster` | `01 1b 01 03 …06 00` | 1 |

Discriminators (byte-verified):
- **request vs response** on op=10: a REQUEST has `marker@72==0` and `result@92`=zeros
  (and `rc`=peer handle, `lc`=own handle); a member RESPONSE has `marker@72==1`,
  `result@92` filled, `rc`=**our** handle (`0x4e630007` on the wire; OVMX's own-dir
  handle `0x4F580008` in OVMX's run).
- **HIT vs MISS**: `result@92 != "NOT PRESENT HERE"` ⇒ HIT. Marker is 1 for **both**
  hit and miss — do **not** use marker to classify, and never test `result==name`
  (the `VMS$VAXcluster` HIT returns a binary blob, not the name).

## Full join order (each connect gated on its lookup HIT)

On one contiguous shared `send_seq`, exactly one outstanding, stop-and-wait:

1. own `SCS$DIRECTORY` connect-req (op=0) → wait member echo(op=1)+response(op=2), learn member dir handle
2. dir **confirm** (op=3, new builder) — fire-and-forget (member bare-ACKs 0x48)
3. lookup `MSCP$TAPE` → wait MISS
4. lookup `MSCP$DISK` → wait HIT
5. `MSCP$DISK` connect (`scs_connect_build_mscp_request`, done) → wait member echo+response, bind
6. lookup `VMS$VAXcluster` → wait HIT
7. `VMS$VAXcluster` VC connect (`send_joiner_connect_request`, done) → wait member echo+response, bind
8. add-member burst (`cm_send_config_burst`, done) → **member reciprocates 204-byte config on the joiner VC ⇒ MEMBER**

Coexistence: throughout, OVMX also keeps answering the member's **separate** dir
probe as a dir-SERVER (member opens its own `SCS$DIRECTORY` connect later; distinct
Con.ID pair — OVMX server handle `0x4F580007` vs client handle `0x4F580008`). The
two directory connections never collide; the Con.ID pair alone disambiguates. This
refutes the earlier "own-dir suppresses the member probe" idea — the clean joiner is
a dir-client AND the member still opens its own probe.

## Implementation notes (corrections applied)

- **Seq oracle is RELATIVE, not the clean joiner's absolute values.** OVMX advances
  contiguously via `scs_seq_advance`; it does not emit the clean joiner's intermediate
  MSCP frames, so its VC connect lands at whatever contiguous seq comes next — **not**
  the clean joiner's `seq=10`. Success criterion = contiguous seq, no gap, one frame
  outstanding, member `recv_ack` never freezes. Hard-forcing absolute values would
  reopen the 760mscp hole.
- **Per-lookup seq storage-and-reuse** must be added: `send_own_dir_lookup` currently
  advances the seq every call and stores nothing. The stop-and-wait retransmit of the
  three lookups must reuse a stored seq, mirroring `own_dir_req_seq`/`mscp_req_seq`/
  `joiner_req_seq` — never re-call the sender (which would advance and open a hole).
- New builder `scs_dir_build_connect_confirm` (op=3, 62-byte SCA / 76-byte frame,
  Con.IDs bound, marker `0x00010000`, no names).
- Sequencer state in `peer_state`: `join_step` enum + outstanding-step seq + retx
  timer; response handler keys on `rc==own-dir handle && op@60==10 && marker@72==1`,
  affirmative = `result != "NOT PRESENT HERE"`.
- Keep the dir-SERVER responder (branch b2) and member-opened VC path unchanged.
