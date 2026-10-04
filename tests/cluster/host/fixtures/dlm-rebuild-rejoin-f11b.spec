%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-rebuild-rejoin-f11b
class:     scs-msg
origin:    capture
capture:   rejoin-rebuild.pcap
wire-len:  204
sha256:    c49655fdc3a63675a6d95b220954481ab28e371d45c029beda2de37ca17c1329
%bytes
; REAL captured cat-0x02 op-0x0d REBUILD-record REQUEST, VAX1(survivor)->VAX2
; (vms-20c 2-VAX rejoin, 2026-09-15). Files-11 XQP $a sublock; PARENT "DIRECTORY" in the mid-region
; resource[b48]="F11B$aSYSDSK1     ...." (reslen[b47]=22).
; GROUNDED anchors: body[8:10]=02 0d (cat/op); body[12:14]=0x0001 (invariant);
; body[14:16]=0x0004 REBUILD-TYPE (0x0004 REJOIN vs 0x0003 JOIN -- NOT a constant).
; Full body CITED (real capture). Tail body[104:128] = frame-specific STALE BUFFER
; (real VMS residue), body[128:132] a per-frame trailing/checksum value -- both
; preserved VERBATIM by the round-trip, never named or fabricated.
@0    08 00 2b 1e 85 61 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 4b 13
@32   7c 00 2f 01 01 00 12 00 7c 00 00 00 2f 01 00 00
@48   7c 00 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   09 00 82 2c 0a 00 f7 70 14 01 61 00 05 00 8a 61
@80   02 0d 52 5f 01 00 04 00 20 02 00 00 04 00 00 00
@96   44 49 52 45 43 54 4f 52 59 20 20 20 00 00 00 00
@112  00 00 00 00 00 00 00 16 46 31 31 42 24 61 53 59
@128  53 44 53 4b 31 20 20 20 20 20 01 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@160  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 00 00 00 00 31 a2 e3 6b
