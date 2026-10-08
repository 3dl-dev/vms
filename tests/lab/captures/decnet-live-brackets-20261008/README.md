# Live real-VAX brackets against a booted OVMX (2026-10-08)

DECnet lane (rd vms-30e). These are raw lab captures from a disposable lab pod (ovmx-lab/dnlab-1). The pod ran:

- VAX1 and VAX2: OpenVMS VAX V7.3 under SIMH, at 1.1 and 1.2.
- OVMX: x86_64 under QEMU TCG at 1.42, on the same bridge (br0).

The OVMX images were built by the lab-only `lab/dn-artifacts` workflow from these branches:

- work/vms-mail11-inbound at 8a6ae0fb (the MAIL bracket);
- 8f962efe, which is PR #1490 on top of main with #1483 (the SET HOST bracket).

The wire dumps are `tests/lab/tools`-style NSP/DAP dumps of a tcpdump on br0 (`ether proto 0x6003`), with hello frames dropped. Passwords are masked as `XX`.

| File | What it shows |
|---|---|
| `mail11-live-wire.txt` | VAX1 `MAIL/SUBJECT="OVMX bracket 20261008" BRACKET.TXT OVMX::SYSTEM` to the booted OVMX. Object-27 connect, then the Connect Confirm from NETACP. The recipient gets `01 00 00 00`, and so does the message after it is stored. The VAX printed no error. |
| `mail11-live-ovmx-console.txt` | The OVMX console reading that message back with OVMX MAIL: `DIRECTORY` (message 2 from `VAX1::SYSTEM`) and `READ 2` (From, To, CC, Subj, both body lines). It is raw: an unrelated typed line (`RUN ...AUTHORIZE` landing inside MAIL) is left in. Message 1 is the booted `--mail11-accept-test` replay of the committed oracle. |
| `sethost-la36-vax1-console.txt` | VAX1 `SET HOST OVMX`, an OVMX LOGINOUT login over CTERM, then `SHOW TERMINAL` showing `_RTA0:  Device_Type: LA36  Remote Port Info: VAX1::SYSTEM` and the characteristic grid conveyed from the VAX's TT$/TT2$ (rd vms-14b, PR #1483). |
| `sethost-la36-wire.txt` | The CTERM wire for that session, with the password masked. |
| `vax73-dirfull-recfmt.txt` | VAX1-only oracle (no OVMX involved): `DIRECTORY/FULL` record format and attributes lines for FIX-20, STREAM_LF, STREAM, STREAM_CR, VFC/PRN, UDF, FTN, a non-spanned VAR file, and a DCL `OPEN/WRITE` file, plus `F$FILE_ATTRIBUTES` MRS/LRL/RFM/RAT. This is ground truth for rd vms-a44. |

The FAL remote-verb brackets (OVMX and VAX-to-VAX) are kept with PR #1487's captures under `../decnet-fal-verbs-20261008/`.
