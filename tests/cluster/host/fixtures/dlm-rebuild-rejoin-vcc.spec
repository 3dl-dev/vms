%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-rebuild-rejoin-vcc
class:     scs-msg
origin:    capture
capture:   rejoin-rebuild.pcap
wire-len:  204
sha256:    08ec1ba1351e806de8bec6160948dd415923e745f5d8e566773b60159b45239a
%bytes
; REAL captured cat-0x02 op-0x0d REBUILD-record REQUEST, VAX1(survivor)->VAX2
; (vms-20c 2-VAX rejoin, 2026-09-15). volume cache; class 20 02, binary mid-region
; resource[b48]="VCC$vSYSDSK1     " (reslen[b47]=17).
; GROUNDED anchors: body[8:10]=02 0d (cat/op); body[12:14]=0x0001 (invariant);
; body[14:16]=0x0004 REBUILD-TYPE (0x0004 REJOIN vs 0x0003 JOIN -- NOT a constant).
; Full body CITED (real capture). Tail body[104:128] = frame-specific STALE BUFFER
; (real VMS residue), body[128:132] a per-frame trailing/checksum value -- both
; preserved VERBATIM by the round-trip, never named or fabricated.
@0    08 00 2b 1e 85 61 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 4b 13
@32   7c 00 2e 01 01 00 12 00 7c 00 00 00 2e 01 00 00
@48   7c 00 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   09 00 82 2c 0a 00 f7 70 13 01 61 00 01 00 f4 65
@80   02 0d 49 53 01 00 04 00 20 02 00 00 05 00 00 00
@96   78 da 94 87 78 da 94 87 80 da 94 87 00 00 00 00
@112  00 00 00 00 00 00 00 11 56 43 43 24 76 53 59 53
@128  44 53 4b 31 20 20 20 20 20 2b 26 94 af 24 bc 00
@144  02 02 00 07 00 00 00 00 a3 6c 01 00 00 00 00 00
@160  00 5e d0 b2 00 00 00 00 c0 da 94 87 c0 da 94 87
@176  c0 64 8d 87 00 cc 94 87 00 00 00 00 00 00 00 00
@192  00 00 00 00 00 00 00 00 91 1e 47 e4
