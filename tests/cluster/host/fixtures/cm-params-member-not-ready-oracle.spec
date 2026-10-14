%OVMX-CLUSTER-SPECIMEN-1
name:      cm-params-member-not-ready-oracle
class:     scs-msg
origin:    capture
capture:   fcb-L2-params.pcap
spec:      vms_cluster_codec_cm.h VMS_OFB_CM_PFLAGS (rd vms-f4e)
wire-len:  204
sha256:    74154df2623481e8ff7245570f2fc7621565a7fedf25530de23252f368c74728
%bytes
; REAL captured record 5 of tests/lab/captures/vms-fcb-lockdirwt-20261004/fcb-L2-params.pcap: a real VAX V7.3 cat-0x01 op-0x01 PARAMS from a member of a two-member cluster whose body[82] is 0x2a, a member not yet ready to serve its clusterwide database (rd vms-f4e).
@0    08 00 2b 81 28 a1 08 00 2b 92 0b 09 60 07 bc 00
@16   aa 00 04 00 03 04 01 00 aa 00 04 00 02 04 5b 13
@32   17 00 19 00 01 00 12 00 17 00 00 00 19 00 00 00
@48   17 00 00 00 01 00 00 02 92 00 04 00 0a 00 00 00
@64   11 00 ea 74 0e 00 e9 74 02 00 00 00 00 00 00 00
@80   01 01 00 00 21 50 00 00 00 00 02 00 01 00 00 00
@96   01 00 02 00 80 b4 22 b4 cd 37 bc 00 c0 7e fb e9
@112  cd 37 bc 00 03 00 00 00 00 00 00 00 00 00 00 00
@128  00 00 00 00 00 00 00 00 c0 7e fb e9 cd 37 bc 00
@144  10 00 00 00 01 00 00 00 00 00 2a 00 d9 01 00 00
@160  56 37 2e 33 20 20 20 20 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 00 00 00 00 00 00 00 00
