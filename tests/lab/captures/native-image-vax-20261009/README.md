# Images LINKed on real OpenVMS VAX V7.3, and what that system said about them (2026-10-09)

Evidence for rd vms-b869, the VAX rung of vms-3b3f. Captured on a disposable lab pod
(`n3b3f-vax`, node `VAX1`, OpenVMS VAX V7.3 under SIMH), driven over the console FIFO.
The programs are our own MACRO-32 sources (`tests/native-images/vax/`); the node has
MACRO and LINK but no C compiler. Each `.EXE` was recovered byte for byte from `DUMP`.

| image | on the node |
|---|---|
| `HELLO.EXE` | `SYS$QIOW` + `LIB$PUT_OUTPUT` lines, `$STATUS` `%X00000001` |
| `RETST.EXE` | `$STATUS` `%X0FEDC0A9` |
| `CSTDIO.EXE` | `DECC$PUTS` + `DECC$DPRINTF` (VAX has no `DECC$TXPRINTF`) lines, `%X00000001` |
| `MAIN3.EXE` / `MAIN4.EXE` | two modules + our shareable `MYSHR` (transfer vector, `UNIVERSAL=`); with `MYSHR` defined, three lines, `%X00000001`; `MAIN4` (linked against MYSHR 2.0) against 1.0: `-SYSTEM-F-SHRIDMISMAT` `%X100020BC` |
| refusals | MYSHR not found: `-CLI-E-IMAGEFNF` `%X100388B2`; a text file: `-IMGACT-F-BADHDR` `%X104D8C84` (Alpha says NOTNATIVE) |

The system services are called at absolute P1 addresses (the P1 system service vector,
`SYS$QIOW` = `7FFEDE00` ...), not through a shareable image; `LIBRTL` and `DECC$SHR`
routines are called through fixup-vector cells that hold the routine's transfer-vector
offset (`LIB$PUT_OUTPUT` = `0x478`). `docs/oracle/vax73-symvec-offsets.txt` collects the
values from our probe `ORDSVX` (`ORDSVX.*`). VAX V7.3 defines no `DECC$SHR` logical
(`LNM-shareables.txt`). Link maps list only our modules plus shareable references and the
0-byte `SYS$P1_VECTOR` symbol module.
