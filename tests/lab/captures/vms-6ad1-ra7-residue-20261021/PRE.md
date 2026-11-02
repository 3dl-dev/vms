# RA7 pre-fire record. Written BEFORE the OVMX node booted.

Everything here was read from the live pod. Nothing in it is a prediction. The
predictions are in [`PLAN.md`](PLAN.md), committed 2026-10-21, and they are
not re-cut.

## The pod

`lab6ad1`, namespace `ovmx-lab`, image `192.168.2.43:30500/ovmx-vaxlab:7`,
`requests == limits` `1500m`/`2Gi`, node `k3s-worker`, created 2026-11-01
21:2x UTC. The lane created it for this arm and deletes it once the evidence is
committed. The five shared `vaxlab-*` replicas were not touched.

| what | value |
|---|---|
| in-pod `/usr/local/bin/entrypoint.sh` | md5 `3912fef8ad40e15465b74187406f4796`, byte-identical to `tests/lab/entrypoint.sh` |
| in-pod `/usr/local/bin/nodedrv.py` | md5 `4d3c455984f527e17348b38ae930d0a2`, byte-identical to `tests/lab/nodedrv.py` |
| `/etc/simh-version.txt` | `open-simh 2e0d51e9e97b2c3a28c55261108550d1f533916b (x86_64)` |
| `vax1/vax.ini` | carries `set clock nocatchup` (rd vms-549c) |
| boot trust (rd vms-d3a) | no `.bootfail` for either node; both boot commands were echo-verified |

## The cluster before anything in this arm ran

| node | `CLUSTER_NODES` | `LOCKDIRWT` |
|---|---|---|
| VAX1 | 2 | 1 |
| VAX2 | 2 | 1 |

OVMXE boots with `LOCKDIRWT 0`. That is the general configuration.

## The holder, built and smoke-tested on both VAXes

`../vms-50d-ra5-14cells-20261013/RMHOLD.MAR` went to each VAX by FTP with the
committed `vput.py` over the pod's own `br0` (`10.2.1.1`, `10.2.1.2`). Then:
`MACRO RMHOLD`, `LINK RMHOLD` (no diagnostics on either node), copied to
`SYS$COMMON:[SYSEXE]`, and `RMH :== $SYS$SYSTEM:RMHOLD`.

| node | smoke | result |
|---|---|---|
| VAX1 | `SPAWN/NOWAIT/PROCESS=SMOKE RMH 100-SMOKTST` | `SMOKE` in `HIB`; `STOP SMOKE` completed, process gone |
| VAX2 | `SPAWN/NOWAIT/PROCESS=SMOKE2 RMH 100-SMOKTS2` | `SMOKE2` in `HIB`; `STOP SMOKE2` completed, process gone |

## The build under test

Branch `lab/vms-6ad1-ra7` at `ef0b7e3fc`. It is the head of PR #1900
(`daa4e6eef`: vms-5cd, vms-a4d7 and the rest, on top of main `14716d4c7`) plus
the lab-only RMHOLD.EXE staging commit (the same diff as `f8f462511`, never
merged to main). The trail asked for #1900's fixes first, because a tree
without vms-5cd/vms-a4d7 re-hits known bugs. Building from #1900's own head
gives exactly the tree that merges. Artifacts: `build-boot-artifacts.yml` run
`41352991717`, `cluster_auth_group=1`.

## The gate, before the node booted

`tests/cluster/conformance/run_gate.sh` over a build of `ef0b7e3fc`
(`/tmp/ovmx-ra7-build`), for the frame types each fire declares:

| fire | types | result |
|---|---|---|
| A | `dlm_enq_req dlm_enq_ans_queued dlm_grant_notify dlm_deq dlm_rm_ack dlm_rm_ack_nm dlm_rm_lock_ans dlm_rm_14` | `every byte grounded`; negative control refused |
| B | the same with `dlm_rm_14_ra7` in place of `dlm_rm_14` | `every byte grounded`; negative control refused |

## The sequence, per PLAN.md's roles

Fire A: O = VAX1, N = OVMXE, H = VAX2. Fire B: O = VAX2, N = OVMXE, H = VAX1,
with `RM_RESIDUE_FILL=0x5A`. The pod is reset between the fires and both
consoles are crash-checked first.

1. O: `RMHOLD 123WRA7T`. O masters `RA7T`.
2. OVMXE: `RMHOLD 100-RA7T`. **P0 is checked here.** O's `SDA> SHOW
   RESOURCE/NAME=RA7T` must list an LKB held for OVMXE's CSID. If it does not,
   the arm stops.
3. O: `STOP HOLD1`. The tree should remaster onto OVMXE.
4. SDA on both VAXes, then H: `RMHOLD 100-RA7T`. This is P8: OVMXE must grant
   it from the adopted tree.
5. SDA again, stop the holders, collect consoles and capture.
