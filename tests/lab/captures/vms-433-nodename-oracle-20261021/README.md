# One node name: what a real VAX answers (rd vms-433, 2026-10-21)

This is the demo's own real VAX, VAXC (OpenVMS VAX V7.3). Its pinned `vms73-nodeC-cluster.dsk.gz`
volume (sha256 `45355fd2…fe15`) was booted alone under open-simh and logged in as SYSTEM on the
console. The commands were then run in the order shown. The command echoes were de-duplicated (the
harness logged each command twice). No output line was touched.

```
NODENAME=[VAXC] SCSNODE=[VAXC  ]
```

- `F$GETSYI("NODENAME")` is the node's name, the same as SCSNODE, and is **not** padded.
- `F$GETSYI("SCSNODE")` returns the 6-character SCSNODE field, **blank-padded**.
- `SHOW USERS` and `SHOW PROCESS` name the node VAXC, the same name its cluster uses.

OVMX V0.7-13's Node B in the live demo named itself OVMX in SHOW USERS, SHOW PROCESS and SHOW
NETWORK, while SHOW CLUSTER called it OVMXB. The cause: `F$GETSYI("NODENAME")` was the substrate
hostname and every other surface re-read the parameter file. vms-433 makes the SCSNODE the executive
booted with the one name every surface shows.

`SHOW TERMINAL` on the SIMH console reports `Device_Type: LA36`, because SIMH's console answers no
terminal inquiry. That bears on rd vms-e097.
