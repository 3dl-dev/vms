# Images LINKed on real OpenVMS Alpha V8.4, and what that system said about them (2026-10-08)

Evidence for rd vms-3b3f: an `.EXE` linked on real OpenVMS Alpha activates unchanged on
OVMX/Alpha. Captured on a disposable lab pod (`n3b3f-alpha`, node `ALPHA1`, OpenVMS Alpha
V8.4 on AXPbox ES40), driven over the console FIFO.

## What was built

Every program is our own MACRO-32 source (`tests/native-images/alpha/*.MAR`). The node has
`MACRO` and `LINK` but no C compiler (`CC` -> `%DCL-W-IVVERB`), so the C RTL probe
(`CSTDIO.MAR`) calls `DECC$PUTS` / `DECC$TXPRINTF` from MACRO-32. Each `.EXE` in
`tests/native-images/alpha/` is the node's output file, recovered byte for byte from
`DUMP` (`*.DMP.txt` here) and checked by its ASCII column and header fields.

| image | built with | on the node |
|---|---|---|
| `HELLO.EXE` | `MACRO HELLO`, `LINK/MAP/FULL/CROSS HELLO` | prints via `SYS$QIOW` then `LIB$PUT_OUTPUT`; `$STATUS` `%X00000001` (`HELLO.RUN.txt`) |
| `RETST.EXE` | `MACRO RETST`, `LINK/MAP/FULL/CROSS RETST` | no shareable calls; returns `%X0FEDC0A9`, which is `$STATUS` (`RETST.RUN.txt`) |
| `MYSHR.EXE` | `LINK/SHAREABLE/MAP/FULL/CROSS MYSHR,MYSHR.OPT/OPTIONS` | our own shareable, two universal procedures, `GSMATCH=LEQUAL,1,0` |
| `MAIN3.EXE` | `LINK/MAP/FULL/CROSS/EXECUTABLE=MAIN3 MAIN3A,MAIN3B,MAIN3.OPT/OPTIONS` | two modules + `MYSHR`; with `MYSHR` defined it prints three lines, `$STATUS` `%X00000001` (`MAIN3.RUN.txt`) |
| `CSTDIO.EXE` | `MACRO CSTDIO`, `LINK/MAP/FULL/CROSS CSTDIO` | `DECC$SHR` `puts` + `printf`; `$STATUS` `%X00000001` (`CSTDIO.RUN.txt`) |

Each link map's Object Module Synopsis lists only our own modules plus the shareable images
referenced (LIBRTL, SYS$PUBLIC_VECTORS, DECC$SHR, MYSHR), with 0 bytes contributed by those
shareable images. No library module was linked into any of these images, so they hold only
our code and the linker's image metadata.

`ORDS.MAR` (`tools/lab-alpha/probes/`) references every routine OVMX implements; its link map
(`ORDS.MAP.txt`) gives each one's symbol-vector offset in the image that defines it. Those
offsets are collected in `docs/oracle/alpha84-symvec-offsets.txt` and are what
`src/vmslink/vms_vectors/*.vec` cite. (`ORDS` itself pulled `OTS$HOME_ARGS` from STARLET.OLB,
so its image is not kept.)

## What the activator does when it cannot activate

| case | real output | `$STATUS` |
|---|---|---|
| `RUN MAIN3` with `MYSHR` not found (`NEG-noshr.txt`) | `%DCL-W-ACTIMAGE, error activating image MYSHR` / `-CLI-E-IMAGEFNF, image file not found ...MYSHR.EXE;` | `%X100388B2` |
| `RUN MAIN3` against `MYSHR` relinked `GSMATCH=LEQUAL,2,0` (`NEG-gsmatch.txt`) | `... MYSHR` / `-CLI-E-IMGNAME, image file ...MYSHR2.EXE;1` / `-SYSTEM-F-SHRIDMISMAT, ident mismatch with shareable image` | `%X100020BC` |
| `RUN NOTIMG` (a text file named `.EXE`, `NEG-notimg.txt`) | `... NOTIMG` / `-CLI-E-IMGNAME, image file ...NOTIMG.EXE;1` / `-IMGACT-F-NOTNATIVE, image is not an OpenVMS Alpha image` | `%X104D8CFC` |

## How the format was read

`ANALYZE/IMAGE` reports (`*.ANL.txt`) name every header field, image section descriptor and
fixup and give its value. Matching those values to the bytes of the same file fixes each
field's offset; `src/imgact/imgact_eihd.h` records the result and
`src/imgact/test/test_eihd_parse.c` checks every field against these reports. Clean-room:
nothing was disassembled and no VSI/HPE source, header or binary was read.
