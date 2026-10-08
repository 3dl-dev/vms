%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-op08-req-coldform
class:     scs-msg
origin:    capture
capture:   coldform-ev2-formation.pcap
frame:     121
spec:      vms_cluster_codec_dlm.h VMS_DLM_WIREOP_DIR_LOOKUP_TR (rd vms-629)
wire-len:  204
sha256:    0386d2d7a9dbd15cc4cfcf0ecd72b64182c8140a948be0c4a2bfad4d0f2d6624
%bytes
; REAL captured record 121 of tests/lab/captures/vms-6d3d-coldform-ev2-20260924/coldform-ev2-formation.pcap:
; a real VAX's cat-0x02 op-0x08 'SYS$SYS_ID'+its own SCSSYSTEMID, sent to the other
; founding VAX at formation-barrier step ~7 (rd vms-629). body[36:44] zero (a ROOT), hash body[128:132].
@0    08 00 2b 62 02 09 08 00 2b 06 7d 8a 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 4b 13
@32   27 00 2d 00 01 00 12 00 27 00 00 00 2d 00 00 00
@48   27 00 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   08 00 2b 98 09 00 2b 98 12 00 0b 00 01 00 51 87
@80   02 08 00 00 01 00 01 00 00 00 00 00 03 00 02 00
@96   00 00 00 01 5c 00 05 05 02 00 01 00 00 00 00 00
@112  00 00 00 00 00 00 01 10 53 59 53 24 53 59 53 5f
@128  49 44 01 04 00 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@160  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 01 00 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff db 24 43 32
