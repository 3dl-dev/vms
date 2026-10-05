%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-dir-register-vax1
class:     scs-msg
origin:    capture
capture:   dirrole-L1L2.pcap
spec:      vms_cluster_codec_dlm.h "THE DIRECTORY ROLE" (rd vms-8219)
wire-len:  204
sha256:    3497a7dff84df180f3a609b6aa8d3401bf7013f028e6ce686817c88f1743f90e
%bytes
; REAL captured frame 2 of tests/lab/captures/vms-8219-dir-role-20261004/dirrole-L1L2.pcap: VAX1 registering a resource it masters with its new directory node VAX2 at VAX2's admission (op-0x0d, L2).
; body[8:10]=02 0d; body[28:40]=000000000000000000000000; body[44:48]=0000000d;
; name=b'SYS$_$2$DUA0:'; body[128:132]=63fe262d.
@0    08 00 2b 92 0b 09 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 5b 13
@32   ac 00 5e 01 01 00 12 00 ac 00 00 00 5e 01 00 00
@48   ac 00 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   0a 00 ea 74 0a 00 53 2f 16 01 64 00 05 00 73 1e
@80   02 0d 03 00 01 00 02 00 00 00 00 00 00 00 00 00
@96   00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@112  00 00 00 00 00 00 00 0d 53 59 53 24 5f 24 32 24
@128  44 55 41 30 3a 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@160  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 00 00 00 00 63 fe 26 2d
