# Scoping — real VAX MOUNTs an OVMX-MSCP-served unit (the last cluster-1.0 gap)

> Design/scoping record for the V0.6-9-worthy increment. Grounded on origin/main
> (worktree HEAD d61423c2), 2026-08-31. Clean-room (Rule 8): every field cited
> below is from a lab capture we ran (vaxlab-9, real-VAX↔real-VAX, 2026-08-06,
> `docs/design-mscp-direction.md`) or public MSCP/Cluster-Systems docs. No VSI
> bytes.

## Why this exists

The compatibility register and several code comments UNDERSTATE the MSCP server.
`docs/compat/facilities/mscp-serve.yaml` calls `online-end` a **stub** ("book-only
… the captured joiner never mounts"); `scs_mscp_srv.c:12-16` says the data path
"is not implemented at all." Both are **stale confounds**. The "captured joiner
never mounts" corpus is the vms-2f3 *rejoin* stall (joiner side), NOT the serving
path. This doc records what is actually built so the register/comments can be
corrected and the true remaining gap driven.

## What is already built + grounded (origin/main `src/vmsscs/scs_mscp_srv.c`)

Dispatch `scs_mscp_srv_handle()` (`:765`). All handlers for the documented mount
handshake are present:

- **SET_CTLR_CHAR 0x04** → `build_scc_end` — byte-exact golden test (954 frames).
- **GET_UNIT_STATUS 0x03** → `build_gus_end` (`:395`) — MD.NXU enumeration walk +
  Unit-Offline terminator; byte-exact (52-byte, `[48:50]=0x006e` from capture).
- **ONLINE 0x09** → `build_online_end` (`:456`) — NOT a no-op: sets `u->online`,
  echoes host P.UNFL bit-15 (0x8000, grounded by vaxlab-9 `:496`), OR-s UF.WPS,
  writes P.UNSZ (volume size the mounting VAX reads) + P.VSER; length 44 (measured).
- **READ 0x21** → `handle_read` (`:533`) → real `pread` (`:233`) → streams via the
  `srv->xfer` hook (`scsd_mscp_srv_xfer`, `scsd.c:7854`).
- **WRITE 0x22** → `handle_write` (`:625`) — read-only opt-out (0x1006) or `pwrite`.
- Anything before Controller-Online, or unknown → Invalid Command (`:316`).

Mount ordering (Fig 4-38, confirmed from a cold real node, `design-mscp-direction.md:437-440`):
`SCC→SCC-END ×2 → GUS walk (term 0x0003) → ~20s GUS polls → at MOUNT: ONLINE→ONLINE-END
→ GUS→GUS-END → READ LBN1 (home) → READ 0x40a → READ/WRITE INDEXF/BITMAP`. Every
step has a handler. **No opcode is missing at the command level.**

Served volume: filesystem-agnostic raw blocks. The daemon (`scsd.c:2719`) `open()`s
`OVMX_MSCP_SRV_UNIT_FILE` read-only. Point it at a **genuine ODS-2** image
(`tests/ods2/real_vax_ods2.dsk`, real-VAX-produced, in-tree; or OVMX's ODS-2 writer
output, real-VAX-MOUNT-verified vms-0f3) — NOT the `vmsfs` "VMFS/VFH2" ODS-2-*inspired*
image (`src/vmsfs/include/vmsfs_ondisk.h:52`). Default (env unset) = honest zero-unit.

## The true remaining gap (3 parts)

1. **End-to-end run — a real VAX mounts OVMX** (lab/harness, heaviest, no new code).
   Only VAX↔VAX was ever captured; no run has a real VAX MOUNT the OVMX daemon.
   Acceptance: `%MOUNT-I-MOUNTED` + `TYPE` of a marker file on the VAX console, +
   a pcap of ONLINE→ONLINE-END→GUS→READ→block-transfer originating from OVMX.
2. **Block-transfer header constants `+4`/`+6`** (`conn_const`/`xfer_const`,
   `scsd.c:7854`) — the ONE genuine RE unknown (`design-mscp-direction.md:455`:
   "ungrounded — do not build on", incl. whether credit/flow-control interacts with
   block transfers). If the VAX rejects OVMX's READ data, this is why. Grounds only
   from a real capture of OVMX serving.
3. **Byte-exact ONLINE-END oracle** — SCC + GUS have goldens; ONLINE-END does not.
   Add one once a capture of OVMX's ONLINE-END *accepted by a real VAX* exists.

Not needed for read-only mount: the WRITE live hook (`scs_mscp_srv_set_wxfer` exists
but is never called in `scsd.c` — read path only, `:2795`). Add only if write is in
scope.

## Lab recipe (tests/lab/README.md §MSCP serving, vms-291/vms-3d3)

Stock lab-2 can't serve (both nodes attach the same disk). Asymmetric recipe: repoint
vax1 `rq2`→`ra81` on a new `d2.dsk` vax1 alone owns; disable vax2 `rq2/rq3`;
`MSCP_LOAD=1`/`MSCP_SERVE_ALL=1` (golden); boot; from vax2 MOUNT vax1's served unit.
For THIS increment, run `scsd` (OVMX) as the server (`OVMX_MSCP_SRV_UNIT_FILE=…/real_vax_ods2.dsk`,
`OVMX_MSCP_SRV_UNIT=n`) joined to a real VAX, and MOUNT from the VAX. **Traps:**
`entrypoint.sh` regenerates `vax.ini` every boot (hand-edits lost — edit entrypoint +
rebuild, or treat pod as disposable); verify `CLUSTER_NODES=2`; mint identities via
`mk_sysgen.py` (avoid REMOTE NODE conflicts). **HEAVY — hold until disk <~88% and
coordinate with the conductor (single heavy lane).**

## rd items that are DONE-in-code but likely open in the tracker (for the rd purge)

vms-600 (READ block-transfer sender — `:15-16` comment stale), vms-34b (MSCP$DISK in
LISTEN set, `scsd.c:2888`), vms-941 (SCA block transfer — design prose "deferred"
stale), vms-6e1 (rw opt-in, server side), vms-4e31 (pwrite mirror), vms-61b2 (LISTEN
registration). Confirm each against the cited file:line, then close/reframe.

## What is HONEST vs a real confound (checked 2026-08-31)

Not everything that "looks understated" is a stale confound — several conservative
labels are correct and must NOT be over-claimed away (INV-6):

- **`mscp-serve$online-end` = `stub` (register) / "BOOK-ONLY" (`scs_mscp_srv.c`
  header): HONEST, keep.** The handler is built and its length + UNFL bit-15 echo
  are grounded, but there is no byte-exact ONLINE-END oracle (its body past the
  header is book-derived). "stub/book-only" is the right INV-6 posture. The
  re-census (#992) deliberately kept it `stub`.
- **`scsd.c` "OVMX LISTENS … AND NOT MSCP\$DISK": HONEST, keep.** That header line
  is explicitly self-superseded a few lines down ("SUPERSEDED (vms-34b) … not a
  currently-accurate description, left as record"), and the code below registers
  `MSCP\$DISK` when `ovmx_mscp_server_enabled()` (default on). Self-annotated
  history, not a confound.

Genuinely stale doc prose (fixed in the docs-destale PR): `docs/design-mscp-direction.md`
"block data transfer unimplemented / READ returns Controller Error / vms-941 deferred"
and the "vmsfs not byte-compatible with ODS-2" aside — both contradict the landed
register (`mscp-serve$disk-read-write` implemented/real; `ods2$reader` verified/real).
