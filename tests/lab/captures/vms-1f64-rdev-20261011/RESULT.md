# vms-1f64 held-out lab run rdev1 -- result against PREDICTION.md

Run 2026-10-11 01:57-02:03 UTC, fresh vaxlab-3 pod (k3s-worker rebooted 01:34),
VAX1 1025 + VAX2 1026 (OpenVMS VAX V7.3) + OVMXE 1030, build 6d56dc4a9
(evac/int-12 = main + #1615 + this branch + lab-only EVACWL). Capture:
heldout-run-rdev1.pcap (every frame to/from OVMXE).

| # | prediction | observed | verdict |
|---|---|---|---|
| 1 | per kind-6 close: one 0x08, VAX 0x0d, one 0x05, VAX 0x0f, THEN 0x86; no other opcode from OVMXE | 3 kind-6 closes (two 136-byte DISMOUNT, one 200-byte MOUNT), each exactly 0x08 -> 0x0d -> 0x05 -> 0x0f -> cat-0x86 within 2 ms; OVMXE emitted opcodes {02: 592, 08: 3, 05: 3} only; VAX1 acknowledged every 0x05 under OVMX's own minted tags | HIT |
| 2 | DISMOUNT/CLUSTER prints -SYSTEM-W-NOSUCHDEV for OVMXE, no DEVOFFLINE/GPTFULL/ACCVIO | `%DISM-W-RMTDSMFAIL, _$2$DUA1: failed to dismount on node OVMXE` / `-SYSTEM-W-NOSUCHDEV, no such device available` (x2), then `%DISM-W-DSMCLUFAIL` | HIT |
| 3 | MOUNT/CLUSTER: no RMTMNTFAIL for OVMXE | `%MOUNT-I-MOUNTED, VAX1DATA mounted on _$2$DUA1: (VAX1)` and nothing else | HIT |
| 4 | written-back record differs from the request only at rec[86:90]; rec[0:8] zero | diffs at [86,87] (DISMOUNT, 08090000) and [86,88] (MOUNT, 01000000); rec[0:8] all zero; VAX1 displayed nothing from it | HIT |
| 5 | zero bugchecks / hangs | 0 BUGCHECK on vax1/vax2/OVMXE consoles, no oops/rcu stall; every DCL step returned | HIT |

Not covered by this run: the kind-5 closes (VAX2 at join 01:58:09, VAX1
02:01:45) are still withheld by OVMXE ("transaction close not answered"), as
before this change; no hang followed in this run.
