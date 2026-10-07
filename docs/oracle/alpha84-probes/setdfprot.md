# $SETDFPROT on OpenVMS Alpha V8.4 (observation)

MACRO-32 probe (tools/lab-alpha/probes/tdfprot.mar), run as SYSTEM on lab pod corpusalpha-1, 2026-10-07:

    get          STATUS=00010001 OLD=FA00
    set 0F00     STATUS=00010001 OLD=FA00
    get-again    STATUS=00010001 OLD=0F00

The status is RMS$_NORMAL (0x00010001), not SS$_NORMAL; the first call returned the default in force
(FA00 = S:RWED,O:RWED,G:RE,W: for that process), the second installed 0F00 and returned the previous
value, the third read 0F00 back. OVMX keeps its long-standing initial default 0xFF00 (not the lab's FA00,
which is the stock process default rather than something the service defines) -- see rd vms-44a.
