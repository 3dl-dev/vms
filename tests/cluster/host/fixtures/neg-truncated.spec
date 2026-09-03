%OVMX-CLUSTER-SPECIMEN-1
name:      neg-truncated-sca
class:     unknown
origin:    synthetic
spec:      negative control -- SCA ethertype, frame truncated below the SCA header
wire-len:  20
sha256:    9bfde6ca12e4912712fbe3231eab1b7ca966bd3424321a555560ad19a63c12e7
%bytes
;
; NEGATIVE CONTROL: a real 0x6007 ethertype whose frame stops inside the SCA
; header. The classifier must return VMS_CODEC_E_SHORT with cls UNKNOWN, and
; every accessor must refuse rather than read past the buffer. This is the
; seed the fuzz harness mutates hardest.
;
@0    ab 00 04 01 01 01
@6    08 00 2b 4a b7 15
@12   60 07
@14   76 00                      ; claims a 120-byte payload it does not have
@16   ab 00 04 01
