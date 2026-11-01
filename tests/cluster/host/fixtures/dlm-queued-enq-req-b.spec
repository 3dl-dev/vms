%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-queued-enq-req-b
class:     scs-msg
origin:    capture
capture:   r3-ovmx-first.pcap
frame:     8245
spec:      vms_cluster_codec_dlm.h "THE SAME RECORD WHEN THE MASTER QUEUED THE REQUEST INSTEAD" (rd vms-85f)
wire-len:  204
sha256:    6113e02ff2db95a53867cea9a2046d9bb5b04fee64cb27d4b738c1e4cf4d8fac
%bytes
; REAL captured frame 8245 of tests/lab/captures/vms-d52c-ovmx-first-20261012/r3-ovmx-first.pcap
; -- a SECOND arrangement (heldout-proof-must-vary-topology): a different master
; (08:00:2b:7a:2a:ac), a different requester, a different resource ('LNM$CWLOGICALS')
; and a different mode (0x04, PW). Still a ROOT request: body[36:44] all zero.
@0    08 00 2b 7a 2a ac 08 00 2b 03 7e 61 60 07 bc 00
@16   aa 00 04 00 01 04 01 00 aa 00 04 00 02 04 4b 13
@32   e1 0b b5 11 01 00 12 00 e1 0b 00 00 b5 11 00 00
@48   e1 0b 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   09 00 52 2f 09 00 52 2f 74 11 9f 0b 05 00 3d 21
@80   02 01 00 00 01 00 03 00 00 00 00 00 10 02 60 20
@96   75 02 00 0c 10 01 04 00 10 00 01 00 00 00 00 00
@112  00 00 00 00 00 00 00 0e 4c 4e 4d 24 43 57 4c 4f
@128  47 49 43 41 4c 53 20 20 20 20 2a 00 00 00 00 00
@144  38 2b f9 7f 70 00 00 00 00 00 00 00 00 00 00 00
@160  00 41 00 00 00 00 d4 01 01 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff 63 c4 3c 47
