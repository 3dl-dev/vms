# vms-c56: the three consoles of the live cluster demo, V0.7-13 (2026-10-21)

Raw console transcripts of one run of the 3-node browser cluster demo
(https://openvmx.3dl.dev/demo/cluster/, release V0.7-13), captured by the
explore driver on 2026-10-21. Nothing was edited.

| File | Node | What it is |
|------|------|------------|
| `A.console.txt` | OVMXA | OpenVMX on the Linux substrate (x86_64, qemu-wasm) |
| `B.console.txt` | OVMXB | OpenVMX on the NetBSD substrate (VAX, pcjs), logged in, DCL commands typed |
| `C.console.txt` | VAXC  | the real OpenVMS VAX V7.3 in the same cluster (pcjs) |

## What they show

`C.console.txt` is the oracle: the console of a real VMS node joining the same
cluster. At boot it prints three short connection-manager lines, with **two**
spaces after the comma:

```
%SYSINIT, waiting to form or join a VMScluster system
%CNXMAN,  proposing formation of a VAXcluster
%CNXMAN,  now a VAXcluster member -- system VAXC
%CNXMAN,  completing VAXcluster state transition
```

and later cluster events as OPCOM messages.

`A.console.txt` and `B.console.txt` are the defect rd vms-c56 fixes: the OVMX
executive put every connection-manager, PEDRIVER, lock-manager and MSCP
diagnostic it composed on OPA0:, with Linux printk uptime stamps
(`[   37.218759] %CNXMAN, no coordinator CSID seen yet: ...`) on node A, onto
the `Username:` line, and at a logged-in DCL prompt on node B
(`%DLM, refusing a lock message (cat 0x02 op 0x01) from system 0x10001: ...`).

## Who reads them

- `tools/cluster-web-demo/test/visitor-gate.test.mjs`: the visitor gate's
  console-chatter assertion (`consoleChatter`, `tools/cluster-web-demo/gate-eval.mjs`)
  must FAIL on A and B and PASS on C, the real VAX, whose own console grounds
  the allowlist.
- `tests/cluster/host/test_cluster_opa0.c`: every console form the executive
  may print (`src/kernel-core/vms_cluster_opa0.h`) is found on a real VMS
  console here or in the VAX lab captures.
