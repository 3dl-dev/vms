%OVMX-CLUSTER-SPECIMEN-1
name:      neg-not-sca-ethertype
class:     unknown
origin:    synthetic
spec:      negative control -- not a VMS frame
wire-len:  60
sha256:    041d0b237bc82f6be9fbdb0991015c8d7187958dd720dbf79639cf86ead896b0
%bytes
;
; NEGATIVE CONTROL, not a capture extract and not composed from the spec: an
; ordinary IPv4 frame. The codec must answer VMS_CODEC_E_NOTSCA and classify it
; UNKNOWN. A cluster port that classifies a non-0x6007 frame at all is a port
; that will one day answer one.
;
@0    ff ff ff ff ff ff
@6    02 00 00 4f 56 58
@12   08 00                      ; ethertype 0x0800 = IPv4, not 0x6007
@14   45 00 00 2e 00 01 00 00 40 00 00 00
