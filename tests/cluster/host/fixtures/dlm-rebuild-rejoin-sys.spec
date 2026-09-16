%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-rebuild-rejoin-sys
class:     scs-msg
origin:    capture
capture:   rejoin-rebuild.pcap
wire-len:  204
sha256:    47a0c8592fdb2ae690bb1c2ee68126e32beb4bb306097fa3b41cb5bf34d6d6b6
%bytes
; REAL captured cat-0x02 op-0x0d REBUILD-record REQUEST, VAX1(survivor)->VAX2
; (vms-20c 2-VAX rejoin, 2026-09-15). system-id lock; class disc body[16:18]=40 02
; resource[b48]="SYS$SYS_ID......" (reslen[b47]=16).
; GROUNDED anchors: body[8:10]=02 0d (cat/op); body[12:14]=0x0001 (invariant);
; body[14:16]=0x0004 REBUILD-TYPE (0x0004 REJOIN vs 0x0003 JOIN -- NOT a constant).
; Full body CITED (real capture). Tail body[104:128] = frame-specific STALE BUFFER
; (real VMS residue), body[128:132] a per-frame trailing/checksum value -- both
; preserved VERBATIM by the round-trip, never named or fabricated.
@0    08 00 2b 1e 85 61 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 4b 13
@32   7c 00 2d 01 01 00 12 00 7c 00 00 00 2d 01 00 00
@48   7c 00 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   09 00 82 2c 0a 00 f7 70 12 01 61 00 03 00 96 61
@80   02 0d 9a 00 01 00 04 00 40 02 01 00 04 00 01 00
@96   03 25 01 00 01 01 00 00 60 5d bd f3 00 00 00 00
@112  00 00 00 00 00 00 01 10 53 59 53 24 53 59 53 5f
@128  49 44 01 04 00 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 04
@160  00 00 00 64 03 00 00 00 01 00 01 00 03 00 03 00
@176  03 00 00 60 ee 78 de ff ff ff 20 72 65 63 65 69
@192  76 65 64 20 56 41 58 63 db 24 43 32
