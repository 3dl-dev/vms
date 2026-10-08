# IO$_ACCESS with an attribute list on OpenVMS Alpha V8.4 (observation)

MACRO-32 program `tools/lab-alpha/probes/tatr.mar` (assembled with the lab's own `MACRO`,
linked with `LINK`): `$PARSE` + `$SEARCH` of `SYS$SCRATCH:ATRT.TXT` (two versions written by DCL
`OPEN/WRITE`), `$ASSIGN` of the device, then

    $QIOW_S CHAN, FUNC=#<IO$_ACCESS!IO$M_ACCESS>, IOSB, P1=FIB descriptor (FIB$W_DID from NAM$W_DID),
            P2=#name descriptor ("ATRT.TXT;2" -- NAM$L_NAME, name+type+version),
            P3=#resultant length word, P4=#resultant descriptor (256 bytes),
            P5=#attribute list (ATR$C_ASCNAME/ATR$S_ASCNAME, ATR$C_CREDATE/ATR$S_CREDATE,
                                ATR$C_RECATTR/ATR$S_RECATTR, terminated by a zero longword)

printed with `$FAO`:

    TATR: NAME=[ATRT.TXT;2]
    TATR: QIOW=00000001 IOSB=0001 RESLEN=10 RES=[ATRT.TXT;2]
    TATR: ASCNAME40=[ATRT.TXT;2                              ]
    TATR: CREDATE=00BC3AB2526AA54E

So: the service and the I/O status are SS$_NORMAL; P3/P4 receive the resultant file name
`NAME.TYP;VER` and its length (10); `ATR$C_ASCNAME` returns the same `NAME.TYP;VER` padded with
SPACES to the item size (`!AF` shows no unprintable bytes in the first 40); `ATR$C_CREDATE` is the
64-bit creation time. Codes and sizes: `docs/oracle/alpha84-starlet-defs/ATRDEF.txt`.

Captured 2026-10-08 on lab pod selfhost-alpha-2 (ALPHA1, cloned from the golden image). Nothing
disassembled (Rule 8): this is the service's observable output.
