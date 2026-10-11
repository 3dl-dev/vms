# vms-1f64 pre-registered lab prediction (committed before the run)

Build: this branch (port opcodes at abs 55, kind-6 pull / write-back / answer).
Topology: the ci6-evac config: VAX1 + VAX2 (OpenVMS VAX V7.3, $2$DUA1 shared)
plus OVMXE (no device named $2$DUA1). Fresh vaxlab-3 pod; consoles checked for
BUGCHECK before the fire.

Steps (VAX1): DISMOUNT/CLUSTER $2$DUA1:, then MOUNT/CLUSTER $2$DUA1: VAX1DATA.

Predictions, each falsifiable from the pcap and the VAX1 console:

1. For each kind-6 cat-0x06 VAX1 sends OVMXE, OVMXE sends exactly one 0x08
   (content 68) naming body[16:20]/[20:24]/[28:32], VAX1 answers 0x0d, OVMXE
   sends one 0x05 into the same buffer, VAX1 sends 0x0f, and only then OVMXE's
   cat-0x86. No OVMXE frame with an abs-55 opcode outside {02,05,08,0c,0d,0f}.
2. DISMOUNT/CLUSTER prints, for OVMXE, `-SYSTEM-W-NOSUCHDEV` (the secondary
   real VAX3 produced in dlmlab F1b), and no DEVOFFLINE/GPTFULL/ACCVIO.
3. MOUNT/CLUSTER completes with no %MOUNT-W-RMTMNTFAIL for OVMXE (VAX3 in F1b
   was skipped silently).
4. The written-back record differs from VAX1's request only at rec[86:90].
   rec[0:8] stays zero (ungrounded; real answers carry an undecoded quadword);
   the prediction is that VAX1 neither displays nor depends on it -- watched,
   not assumed.
5. Zero bugchecks and zero hangs on every console; every DCL step returns.

A miss on any line is recorded here as a miss, not re-explained.
