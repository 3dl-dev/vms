# OpenVMS VAX V7.3 — a device's full name (`DVI$_FULLDEVNAM`)

**Captured 1-NOV-2026, node VAX1, for `vms-1f7`.** Raw console:
`tests/lab/captures/fulldevnam-standalone-20261101/vax1-standalone-fulldevnam.txt`.
Taken on an isolated single-node lab (pod `labdv1`, a clone of the vaxlab disks) that was booted
conversationally with `SYSBOOT> SET VAXCLUSTER 0`. The node was **not** a cluster member:

```
VC=0 CM=FALSE CN=0 NN=VAX1
```

## 1. The question

The clustered VAX1 oracle prints `Devices allocated:  VAX1$OPA0:` and `%DCL-I-ALLOC, _VAX1$OPA0:
allocated` (`vax73-show-system-process.md` §2.3, `vax73-terminal-device.md` §4). OVMX printed `OPA0:`.
The open question was whether the `node$` prefix comes from cluster membership. If it did, a
standalone OVMX node would be right to omit it.

## 2. It does not depend on membership

The same commands on the standalone node:

```
$ SHOW PROCESS
...
Devices allocated:  VAX1$OPA0:
$ WRITE SYS$OUTPUT F$GETDVI("OPA0:","FULLDEVNAM")," ",F$GETDVI("OPA0:","DEVNAM")," ",F$GETDVI("OPA0:","ALLDEVNAM")
_VAX1$OPA0: _OPA0: _VAX1$OPA0:
$ ALLOCATE OPA0:
%DCL-I-ALLOC, _VAX1$OPA0: allocated
$ SHOW DEVICE OPA0:
...
OPA0:                   Online               0
```

So `FULLDEVNAM` and `ALLDEVNAM` carry the prefix, and so do `$ALLOC`'s physical name and SHOW PROCESS.
`DEVNAM` (`_OPA0:`) and the SHOW DEVICE brief row (`OPA0:`) do not. The full name keeps its leading
underscore; SHOW PROCESS prints it without one.

## 3. What decides it: `DEV$M_NNM`, per unit

A walk over every unit (`F$DEVICE()`, printing `DEVNAM`, `FULLDEVNAM` and `DEVCHAR2`; table `R2|` in
the capture) separates the units into exactly two groups. The split follows `DEVCHAR2` bit 9,
`DEV$M_NNM` = 512 (`vax73-starlet-defs/DEVDEF.txt`). Of the 78 units, 53 have the bit set and 25 have
it clear. The prefix follows the bit with no exceptions:

| `DEV$M_NNM` | Units | Full name |
|---|---|---|
| set | `OPAn` `TTxn` `RTAn` `FTA0` `MBAn` `LPA0` `MSA0` `MUAn` `MPA0` and the disks | `_VAX1$TTA0:`, `_VAX1$MBA1:`, ... |
| clear | `NLA0` `PUA0` `PTA0` `XQAn` `NETn` `NDA0` `BGn` `TNA0` `WSA0` | `_NLA0:`, `_XQA0:`, `_NET0:`, ... |

Note that the mailboxes `MBAn` have the bit and `NLA0:` does not, although both are class
`DC$_MAILBOX` (160).

Disks also have `DEV$M_NNM`. On this system (allocation class 2) they are named by the class instead:

```
_$2$DUA0:   (DEVNAM _DUA0:)
```

## 4. The rule OVMX implements (`src/kernel-core/vms_devtab_fullnam.h`)

1. A disk on a system with a non-zero ALLOCLASS: `$<alloclass>$<unit>` (e.g. `$2$DUA0:`).
2. Otherwise, a unit with `DEV$M_NNM`: `<SCSNODE>$<unit>` (e.g. `VAX1$OPA0:`).
3. Otherwise, the physical name alone (e.g. `NLA0:`).

The executive composes the name in `devinfo_fill()` and every reader receives it: `$GETDVI`
`DVI$_FULLDEVNAM`/`DVI$_ALLDEVNAM`, `F$GETDVI`, `$ALLOC` and SHOW PROCESS. OVMX's terminal and disk
rows carry `DEV$M_NNM`. `NLA0:`, the NIC, `_NET:` and `PEA0:` do not. `PEA0:` was not loaded on the
standalone capture because VAXCLUSTER was 0, so its bit is unmeasured and OVMX leaves it clear.

Two limitations remain:

- **No SCSNODE loaded, no prefix.** Before STARTUP's SYSGEN load, the executive has no node name to
  add, so the full name is the physical name. OVMX does not invent a node name for this case.
- **ALLOCLASS is applied to disks only.** Tapes have their own `TAPE_ALLOCLASS`, and OVMX has no tape
  units.
