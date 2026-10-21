# The demo's own real VAX: `SHOW SYSTEM` (rd vms-6512, 2026-10-21)

The target for cluster-demo review finding 3 ("SHOW SYSTEM shows only 3 processes; a real VMS system
shows its system processes"). This is what **VAXC**, the demo's Node C, prints.

**Source:** the pinned Node C volume `vms73-nodeC-cluster.dsk.gz` from baron-3dl/pcjs master
(sha256 `45355fd2631c59666b0fe2839d17ed743e7d045646d2c179a8b3402915b4fe15`), a fresh copy booted alone
under open-simh (`ovmx-vax-lab:latest`, MicroVAX 3900, 64 MB, `set idle=vms`). It formed its own
one-member VAXcluster, as it does in the demo before the OVMX nodes join. Login was SYSTEM with the
build script's default password, typed at the console. Commands ran 30 s after login.

`vaxc-console.txt` holds the console from `SHOW SYSTEM` onward, verbatim apart from one thing. The
harness's console log recorded each typed command twice, once as sent and once as echoed. Those lines
were de-duplicated, and no output line was touched.

## What it shows

OpenVMS VAX V7.3 with no DECnet installed (`SHOW NETWORK` says `%NETWRK-E-NOAVAILABLE`). It runs 13
system processes:

`SWAPPER CLUSTER_SERVER CONFIGURE LANACP IPCACP ERRFMT CACHE_SERVER OPCOM AUDIT_SERVER JOB_CONTROL
SECURITY_SERVER SMISERVER TP_SERVER`

The lab's DECnet node VAX1 (`docs/oracle/vax73-show-system-process.md`) adds `NETACP`, `REMACP` and
`EVL`, plus `QUEUE_MANAGER` and the TCP/IP processes it has started.

## How it is used

`tools/cluster-web-demo/gate-eval.mjs`:
- `SHOW_SYSTEM_ORACLE` is the list above plus the three DECnet processes.
- The visitor gate requires every OVMX node to show `SHOW_SYSTEM_REQUIRED` (`JOB_CONTROL`, `NETACP`).
- Every other oracle name must be in `SHOW_SYSTEM_GAPS`, which names the rd item that would build that
  facility for OVMX.
- A ledgered name that an OVMX node *does* show fails the run, so a placeholder process cannot slip in.

For comparison, OVMX V0.7-13 on x86 with DECnet configured (local KVM, same day) shows:

```
  Pid    Process Name          CPU         Page flts   Pages
10000004 JOB_CONTROL        0 00:00:00.02         89     363
10000001                    0 00:00:01.06         81     357
10000003 NETACP             0 00:00:00.15       1311    1683
10000005 SYSTEM             0 00:00:00.47       1098     876
```

Two of its divergences have items of their own: the unnamed PID 1 row (vms-adf, vms-d0e) and the
missing State/Pri/I/O columns (vms-466).
