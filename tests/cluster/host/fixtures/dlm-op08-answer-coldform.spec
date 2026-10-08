%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-op08-answer-coldform
class:     scs-msg
origin:    capture
capture:   coldform-ev2-formation.pcap
frame:     123
spec:      vms_cluster_codec_dlm.h VMS_DLM_WIREOP_DIR_LOOKUP_TR (rd vms-629)
wire-len:  204
sha256:    5bbb852b6cc39a16429ac10a91a61a2bd4da7b0902677b1e578e1018bfb37095
%bytes
; REAL captured record 123 of tests/lab/captures/vms-6d3d-coldform-ev2-20260924/coldform-ev2-formation.pcap:
; the other VAX's answer, 1.5 ms later: cat 0x82, OPCODE 0x01, body[34]=0xf9 'you master it',
; body[28:39] otherwise stale buffer. The barrier proceeds right after it.
@0    08 00 2b 06 7d 8a 08 00 2b 62 02 09 60 07 bc 00
@16   aa 00 04 00 01 04 01 00 aa 00 04 00 02 04 4b 13
@32   2e 00 28 00 01 00 12 00 2e 00 00 00 28 00 00 00
@48   2e 00 00 00 01 00 00 02 92 00 04 00 0a 00 00 00
@64   09 00 2b 98 08 00 2b 98 0d 00 12 00 01 00 51 87
@80   82 01 00 00 01 00 01 00 00 00 00 00 03 00 02 00
@96   00 00 00 01 81 07 00 00 03 00 f9 00 00 00 00 00
@112  00 00 00 00 00 00 01 10 53 59 53 24 53 59 53 5f
@128  49 44 01 04 00 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@160  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 01 00 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff db 24 43 32
