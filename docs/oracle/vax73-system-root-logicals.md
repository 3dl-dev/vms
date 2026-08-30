# Oracle capture — per-node system-root logicals on OpenVMS VAX V7.3 (`vms-a01`)

**Question:** on a VMScluster with a shared system disk, each node boots from its
own `[SYSn]` root. How does the running system present `SYS$SYSROOT`,
`SYS$COMMON`, and `SYS$SYSDEVICE` on a node that is NOT `[SYS0]`? OVMX today
hardcodes the literal `SYS0` in `src/vmslnm/lnm_defaults.c`; this settles which
strings must carry the per-node token.

The cited `docs/oracle/vax73-system-root-logicals.md` did not exist — captured
fresh.

## Oracle

Live **OpenVMS VAX V7.3** (SIMH, lab-2 `vaxlab-2`, an isolated 2-node pod).
Per `src/vmsscs/include/scs_connect.h:102-104` the lab roots are
`[SYS0]=VAX1`, `[SYS1]=VAX2`, `[SYS11]=VAX3` (diskless), all in one shared
`d0.dsk` that vax1 + vax2 dual-port. So vax1 shows the `[SYS0]` case and vax2 the
`[SYS1]` case directly. Driven per `tests/lab/README.md` (login `SYSTEM`/`system`).

## Result — BOTH the node root and the common carry the per-node index n

### Node VAX1 = `[SYS0]`, verbatim

```
$ SHOW LOGICAL/FULL SYS$SYSROOT
   "SYS$SYSROOT" [exec] = "$2$DUA0:[SYS0.]" [concealed,terminal] (LNM$SYSTEM_TABLE)
        = "SYS$COMMON:"
1  "SYS$COMMON" [exec] = "$2$DUA0:[SYS0.SYSCOMMON.]" [concealed,terminal] (LNM$SYSTEM_TABLE)
$ SHOW LOGICAL/FULL SYS$COMMON
   "SYS$COMMON" [exec] = "$2$DUA0:[SYS0.SYSCOMMON.]" [concealed,terminal] (LNM$SYSTEM_TABLE)
$ SHOW LOGICAL/FULL SYS$SYSDEVICE
   "SYS$SYSDEVICE" [exec] = "$2$DUA0:" [concealed,terminal] (LNM$SYSTEM_TABLE)
```

### Node VAX2 = `[SYS1]`, verbatim

```
$ WRITE SYS$OUTPUT "NODE="+F$GETSYI("SCSNODE")
NODE=VAX2
$ SHOW LOGICAL/FULL SYS$SYSROOT
   "SYS$SYSROOT" [exec] = "$2$DUA0:[SYS1.]" [concealed,terminal] (LNM$SYSTEM_TABLE)
        = "SYS$COMMON:"
1  "SYS$COMMON" [exec] = "$2$DUA0:[SYS1.SYSCOMMON.]" [concealed,terminal] (LNM$SYSTEM_TABLE)
$ SHOW LOGICAL/FULL SYS$COMMON
   "SYS$COMMON" [exec] = "$2$DUA0:[SYS1.SYSCOMMON.]" [concealed,terminal] (LNM$SYSTEM_TABLE)
$ SHOW LOGICAL/FULL SYS$SYSDEVICE
   "SYS$SYSDEVICE" [exec] = "$2$DUA0:" [concealed,terminal] (LNM$SYSTEM_TABLE)
```

## What this pins

1. **`SYS$SYSROOT` is a 2-member concealed rooted search list.** Member 0 is the
   node-specific root `dev:[SYSn.]` — it carries the per-node index n
   (`[SYS0.]` on VAX1, `[SYS1.]` on VAX2). Member 1 is the **logical
   `SYS$COMMON:`** — a name reference, node-independent as a string.
2. **`SYS$COMMON` carries the per-node index too:** `dev:[SYSn.SYSCOMMON.]`
   (`[SYS0.SYSCOMMON.]` on VAX1, `[SYS1.SYSCOMMON.]` on VAX2). It is not globally
   `[SYS0.SYSCOMMON.]`. (Physically these are cluster-common files reached via a
   per-root `[SYSn.SYSCOMMON]` directory alias; the *logical* renders per node.)
3. **`SYS$SYSDEVICE`** is the bare device `$2$DUA0:` on both — node-independent.

So the OVMX per-node work must make **`[SYSn.]` (SYS$SYSROOT member 0) and
`[SYSn.SYSCOMMON.]` (SYS$COMMON, = SYS$SYSROOT member 1 as a literal)** carry the
node's boot-root token. `SYS$SYSDEVICE` stays as-is.

## Consequences for OVMX (`vms-a01`)

- The minimal LNM slice composes the node token into the three `SYS0` literals in
  `lnm_defaults.c` (member 0 `[SYSn.]`, and `SYS$COMMON` `[SYSn.SYSCOMMON.]`,
  which is also SYS$SYSROOT member 1). Default token `0` reproduces this capture's
  `[SYS0]` rows exactly.
- **Fidelity gap (separate follow-on):** authentic `SYS$SYSROOT` member 1 is the
  *logical* `"SYS$COMMON:"`, not the literal `"dev:[SYSn.SYSCOMMON.]"` OVMX seeds.
  Adopting the logical member would let a `SYS$COMMON` redefinition move
  `SYS$SYSROOT` with it (as on real VMS) and remove one composed literal. Left out
  of the minimal slice so the shipped concealed-rooted composition machinery is
  not perturbed. Filed as a `vms-a01` follow-on.

## Reproduce

`kubectl -n ovmx-lab scale sts/vaxlab --replicas=3`; drive `vaxlab-2` `vax1`
(=[SYS0]) and `vax2` (=[SYS1]) per `tests/lab/README.md` (login `SYSTEM`/`system`),
run the three `SHOW LOGICAL/FULL` commands above. Scale back to 2 when done.
