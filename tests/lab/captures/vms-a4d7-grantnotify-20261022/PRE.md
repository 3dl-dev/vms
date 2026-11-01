# Pre-fire checks for fire `a4d1` — recorded BEFORE the OVMX node booted

Everything here was read out of the live pod, not predicted, and is committed
ahead of the fire so the fire's result rests on a bed whose state is on record.

## The pod

`laba4d7`, namespace `ovmx-lab`, image `192.168.2.43:30500/ovmx-vaxlab:7`,
`requests == limits` `1500m`/`2Gi`, node `k3s-worker`. Created by this lane for
this fire and **deleted when this evidence is committed**. The five shared
`vaxlab-*` replicas were left alone.

## Substrate, attributed by CONTENT (rd vms-386)

| what | value |
|---|---|
| in-pod `/usr/local/bin/entrypoint.sh` | md5 `3912fef8ad40e15465b74187406f4796` — byte-identical to this tree's `tests/lab/entrypoint.sh` |
| in-pod `/usr/local/bin/nodedrv.py` | md5 `4d3c455984f527e17348b38ae930d0a2` — byte-identical to this tree's `tests/lab/nodedrv.py` |
| `/etc/simh-version.txt` | `open-simh 2e0d51e9e97b2c3a28c55261108550d1f533916b (x86_64)` — the `SIMH_REF` `tests/lab/Dockerfile` pins |
| `vax1/vax.ini` | carries `set clock nocatchup` (the rd vms-549c fix, whose absence shows up as a hang) |
| boot trust (rd vms-d3a) | no `.bootfail` for either node: both boot commands were echo-verified, so neither VAX is a duplicate-identity clone on a shared root |

## The cluster, before anything of this fire ran

| node | reading |
|---|---|
| VAX1 | `F$GETSYI("CLUSTER_NODES")` = **2**, `F$GETSYI("LOCKDIRWT")` = **1** |
| VAX2 | `F$GETSYI("CLUSTER_NODES")` = **2** |

Two real OpenVMS VAX V7.3 members, both with `LOCKDIRWT 1`. OVMXE boots with
`LOCKDIRWT 0`, which is the GENERAL configuration rd vms-ci.6 must close in.

## The arm, built and smoke-tested on BOTH VAXes

`RMHOLD.MAR` (committed, `../vms-50d-ra5-14cells-20261013/RMHOLD.MAR`, 105
lines) was transferred to each VAX over the pod's own private LAN with the
committed `vput.py` (FTP, `10.2.1.1` / `10.2.1.2` on `br0`), then on each node:

```
$ MACRO RMHOLD          ! no diagnostics
$ LINK RMHOLD           ! no diagnostics
$ COPY RMHOLD.EXE SYS$COMMON:[SYSEXE]RMHOLD.EXE
$ RMH :== $SYS$SYSTEM:RMHOLD
$ DIRECTORY RMHOLD.*    ! RMHOLD.EXE;1 RMHOLD.MAR;1 RMHOLD.OBJ;1
```

Smoke test, each node, on a throwaway name:

| node | arm | result |
|---|---|---|
| VAX1 | `SPAWN/NOWAIT/PROCESS=SMOKE RMH 100-SMOKTST` | `SMOKE` in `HIB` holding; `STOP SMOKE` **completed** (the process is gone) |
| VAX2 | `SPAWN/NOWAIT/PROCESS=SMOKE2 RMH 100-SMOKTS2` | `SMOKE2` in `HIB` holding; `STOP SMOKE2` **completed** |

That `STOP` completes on a *locally*-held lock is the control for prediction P4:
a `STOP` that cannot complete later is then about the cross-node wait and not
about the arm.

## The gate, before the node booted

`tests/cluster/conformance/run_gate.sh` over the gated build tree (a separate
checkout pinned at `f8f462511`, the commit the artifacts were built from) for
the eight frame types this fire declares — `dlm_enq_req dlm_enq_ans_queued
dlm_grant_notify dlm_deq dlm_rm_ack dlm_rm_ack_nm dlm_rm_lock_ans dlm_rm_14`:
**`frame_conformance: every byte grounded`**, and its negative control refused
the corrupted frame, as it must. `dlm_grant_notify` is the new type and the
reason the gate exists.
