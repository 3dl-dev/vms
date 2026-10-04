# vms-20c FC-P5.5 grounding capture — real-VAX rejoin rebuild burst (2026-09-15)

RAW EVIDENCE ONLY — no decode/interpretation here (the ⭐⭐ decode + SDA correlation
is the release-conductor's, fresh). Collected on own-lab pod `vaxlab-2` (ns ovmx-lab),
2-node real OpenVMS VAX 7.3 cluster (VAX1, VAX2).

## Scenario (both ends REAL VMS — cleanest oracle)
2-VAX rejoin: VAX2 crashed (RUN SYS$SYSTEM:OPCCRASH) → departed (VAX1 saw CN_1) →
rebooted via nodedrv.py --boot 'B/R5:10000000 DUA0' (o34ret construct) → rejoined
(CN_2, confirmed: VAX2 mounted cluster disk VAX1DATA + cross-node OPCOM). tcpdump on
br0 'ether proto 0x6007' spanned the whole window. Trigger-independent per the record
format; departure-survivor→new-master (3-node) reserved for the ci.6 proof phase
(OVMX boot artifacts + 3-VAX golden unavailable).

## Node ↔ MAC (from cn3-achieved same pod: 08:00:2b:1e:85:61 = VAX2)
- VAX1 (survivor)        = aa:00:04:00:01:04
- VAX2 (departed/rejoin) = 08:00:2b:1e:85:61

## Files
- rejoin-rebuild.pcap    — 178,596 frames; the departure→downtime→rejoin window.
- sda-before-vax1.txt    — SDA SHOW LOCKS on VAX1 (survivor), BEFORE departure (CN_2).
- sda-before-vax2.txt    — SDA SHOW LOCKS on VAX2, BEFORE departure.
- sda-after-vax2.txt     — SDA SHOW LOCKS on VAX2 (rejoiner/RECEIVER), AFTER rejoin (best-effort; VAX1 console wedged post-transition so no sda-after-vax1).
- frame-histogram.txt    — cat/op counts by src→dst.

## Raw frame counts (mechanical, NOT a decode) — rebuild window
- cat-0x02 op-0x0d (DLM REBUILD): 307 frames  aa:00:04:00:01:04(VAX1) → 08:00:2b:1e:85:61(VAX2)  + 307 cat-0x82 op-0x0d responses back.
- cat-0x01 op-0x05 (SCS LOCKRB):    3 frames  same direction  + 3 cat-0x81 op-0x05 responses.
- (context: cat-0x02 op-01 ENQ x3590, op-03 DEQ x2296, op-04 BLKAST x711, op-06 x477, op-07 CONVERT x342.)

## For the decode (conductor)
PRIMARY QUESTION (premise-3): 307 op-0d vs 3 op-05 => the lock RECORDS appear to travel
via the ALREADY-GROUNDED cat-0x02 op-0x0d DLM REBUILD (vms_dlm_xnode_args / consume
vms_lock.c:2442), with op-0x05 as the phase signal. CONFIRM by decoding the 307 op-0d
bodies as {resnam,req_csid,req_lkid,lkmode} records and correlating resnam/lkid/mode
against the SENDER's locks (sda-before-vax1.txt) and/or the RECEIVER's (sda-after-vax2.txt).
Do NOT infer from counts alone — verify the op-0d payloads parse as real records vs SDA.
