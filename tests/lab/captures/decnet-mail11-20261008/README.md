# DECnet MAIL-11 oracle: a real VAX `MAIL> SEND` to `VAX2::SYSTEM` (rd vms-47fd)

**2026-10-08**, lab dnlab-1. Two real OpenVMS VAX V7.3 nodes: VAX1 (1.1) sends
DECnet MAIL-11 (Session Control object 27) to VAX2 (1.2). Parent epic rd vms-30e.

| File | What it is |
|---|---|
| `vax-console.txt` | Both consoles: VAX1's `MAIL> SEND` to `VAX2::SYSTEM` (accepted) and to `VAX2::NOSUCHUSER` (refused), then VAX2's `MAIL> DIRECTORY` / `READ` of the delivered message. |
| `mail11-wire.txt` | Every NSP message on the wire, decoded by the lab's nspdump: Connect Initiates (with the 16-byte MAIL-11 connect user data), Connect Confirms (with the server's 16-byte accept data), link-service messages, and each data segment both ways with a Python bytes literal. |

## The sessions

1. **Sessions 1-2** (`RCI` -> `DI` reason 0x22): VAX2's MAIL object had no
   account and no executor nonprivileged user, so the connect was refused
   (`-SYSTEM-F-INVLOGIN` on VAX1). Kept as the refusal shape only.
2. **Session 3**: accepted. Sender `"SYSTEM      "`, recipient `"SYSTEM"` ->
   `01 00 00 00`, `00` (end of recipients), To `VAX2::SYSTEM`, CC (empty
   record), Subj, two body lines, `00` (end of message) -> `01 00 00 00`.
3. **Session 4**: recipient `"NOSUCHUSER"` -> status `12 81 7E 00`
   (`%MAIL-E-NOSUCHUSR`), the text record
   `%MAIL-E-NOSUCHUSR, no such user NOSUCHUSER at node VAX2`, and `00`; VAX1
   then disconnects.

## What it grounds

* `src/vmsdecnet/mail/dnet_mail11.c`: the record sequence, both reply shapes,
  the NOSUCHUSR status and text, and the connect / accept user data.
* `tests/vmsdecnet/test_dnet_mail11.c` (ctest `vmsdecnet_mail11_replay`): reads
  `mail11-wire.txt` itself, feeds every VAX1 segment of sessions 3 and 4 to the
  OVMX MAIL-11 receiver and asserts its replies equal VAX2's, byte for byte.
* `DECNETD.EXE --mail11-accept-test` (booted battery): the same segments through
  NETACP's real dispatch to a real `MAIL_SERVER.EXE` process; the battery then
  reads the stored message back with OVMX `MAIL`.

Clean-room: captured from the observed behaviour of real systems; no VSI/HPE
source or binary was examined. The 16-byte connect and accept user data are
carried verbatim and not field-decoded.
