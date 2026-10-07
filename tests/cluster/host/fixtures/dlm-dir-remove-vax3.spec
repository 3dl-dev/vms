%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-dir-remove-vax3
class:     scs-msg
origin:    capture
capture:   dirrole-L1L2.pcap
spec:      vms_cluster_codec_dlm.h "THE DIRECTORY ROLE" (rd vms-8219)
wire-len:  204
sha256:    7976e70e5ebbe0f0a1c112673763d2e6f472b12ab118d52161d904ea8b08f4d2
%bytes
; REAL captured frame 1 of tests/lab/captures/vms-8219-dir-role-20261004/dirrole-L1L2.pcap: VAX3, master of DLMTC, removing its entry from the directory node VAX1 after the last lock went (op-0x04, never answered).
; body[8:10]=02 04; body[28:40]=000001000100000001000000; body[44:48]=00000305;
; name=b'DLMTC'; body[128:132]=e34b9afc.
@0    aa 00 04 00 01 04 08 00 2b 81 28 a1 60 07 bc 00
@16   aa 00 04 00 01 04 01 00 aa 00 04 00 03 04 4b 13
@32   ad 1d 5b 34 01 00 12 00 ad 1d 00 00 5b 34 00 00
@48   ad 1d 00 00 01 00 00 02 92 00 04 00 0a 00 00 00
@64   0d 00 52 2f 0a 00 1e e9 8d 33 de 1c 00 00 00 00
@80   02 04 00 00 01 00 01 00 28 44 00 80 a9 09 00 03
@96   3a 03 00 09 00 00 01 00 01 00 00 00 01 00 00 00
@112  0a 00 00 00 00 00 03 05 44 4c 4d 54 43 00 60 00
@128  f4 c1 f8 7f 7e 00 00 00 09 ca f8 7f 83 00 00 00
@144  09 ca f8 7f 7c 00 00 00 09 ca f8 7f 00 00 60 d7
@160  10 41 00 00 00 00 2a 00 01 00 00 00 0b 00 01 00
@176  00 00 00 00 00 00 00 00 80 d2 98 87 58 d7 98 87
@192  00 00 00 00 00 00 00 00 e3 4b 9a fc
