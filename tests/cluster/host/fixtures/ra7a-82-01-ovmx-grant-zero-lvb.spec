%OVMX-CLUSTER-SPECIMEN-1
name:      ra7a-82-01-ovmx-grant-zero-lvb
class:     scs-msg
origin:    capture
capture:   trimmed.pcap.gz
frame:     26
spec:      vms_cluster_codec_dlm.h op-0x1a + GRANT valblk/seq (rd vms-26e)
wire-len:  204
sha256:    4d1682d990f20bc81643843787bd1a959f8a3b74976209a3ab47a2271b4ed482
%bytes
; RA7 fire A, same capture: THE DEFECT (rd vms-26e). OVMXE, now RA7T's
; master, GRANTS VAX2's VALBLK request with body[36:56] ALL ZERO -- no value
; block, sequence 0 -- where the tree it adopted held RMLVB-0123456789 at
; sequence 1 (frames 8 and 15). VAX2's SDA then read Valblk 0 / Seqnum 0.
; Kept as the BEFORE specimen; the fixed executive must not produce it.
; Every byte cited.
@0    08 00 2b f7 b8 42 52 54 00 00 e5 01 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 06 04 4b 13
@32   38 01 90 00 01 00 12 00 38 01 00 00 90 00 00 00
@48   38 01 00 00 01 00 00 02 92 00 04 00 0a 00 02 00
@64   0d 00 52 2f 05 00 b8 11 6f 00 17 01 0a 00 cd 1e
@80   82 01 00 00 01 00 02 00 00 00 00 00 62 03 00 01
@96   bf 03 00 0b 10 00 00 00 01 00 fa 00 00 00 00 00
@112  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@128  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@160  56 4d 58 20 56 30 2e 37 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 ff ff ff ff 3c 25 6f eb
