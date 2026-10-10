# Keystroke probes: terminal carriage control (rd vms-fc4)

These are not part of the keystroke-oracle ratchet (`docs/oracle/keystroke/`). They are
probe cases written to isolate one rule at a time: where a new line goes around records
and prompts. Each was played into the console (OPA0:) of a real OpenVMS VAX V7.3 node in a
lab pod (`tools/oracle/keystroke/capture_lab.sh`, kslab-f8c, 2026-10-09). The `*.ks` files
are the cases. `<CASE>/vax73.ks.txt` is what the console showed.

The rules `src/kernel-core/vms_tt.c` implements from them (its CARRIAGE CONTROL comment):

- A new line renders by cursor position:
  - at column 0 of an empty line, `CR`;
  - at column 0 of a line that has text (the line feed is owed), `LF`;
  - mid-line, `CR LF`.
- A record (one '\n'-terminated line of a program's output) is a new line, the text, and
  a `CR` that leaves the line feed owed (CC.EMPTY W1/W2/W3).
- A read that echoes pays an owed line feed before its prompt. A NOECHO read does not, so
  its prompt overprints the record's line (CC.MIX A2 vs N3).
- DCL's prompt is a new line, a fill NUL, then the text. `SET PROMPT/NOCARRIAGE_CONTROL`
  makes it three NULs (CC.PROMPT). INQUIRE's prompt is a new line with no NUL. A
  READ/PROMPT prompt has no carriage control (CC.READ, CC.MIX).

## Out-of-band characters (rd vms-f0fb)

`OB.PROMPT` and `OB.READ` were captured the same way on 2026-10-09. They are the basis for the
out-of-band handling in `src/kernel-core/vms_tt.c` and DCL:

- CTRL/Y and CTRL/C show `CR LF *INTERRUPT* CR LF`. This happens at a prompt, mid-line, inside
  `READ/PROMPT` and inside `INQUIRE`. The typed line is discarded, and DCL's next prompt starts
  with its own `CR LF`.
- With `SET NOCONTROL=Y`, CTRL/Y still shows `*INTERRUPT*`, but the read completes with the
  typed line, and DCL runs it (N2). So the driver shows the word, and the CTRL/Y AST decides
  whether anything is interrupted.
- CTRL/T (after `SET CONTROL=T`) shows the status line as a record. The read then reappears: the
  owed line feed, the prompt, and what had been typed (T2).
- CTRL/O at an idle prompt shows nothing.
- `Q.CTRLT`: the lab node's `SYLOGIN.COM` turns CTRL/T on (`SET CONTROL=T`), as VMS's site template
  does. That is why the ratchet's `OOB.PROMPT T` shows a status line before the case enables
  CTRL/T itself. With `SET NOCONTROL=T`, CTRL/T shows nothing and does not end the read.
- `Q.SETDEF` (rd vms-c174) shows that SET DEFAULT never checks whether the directory or device exists. SHOW DEFAULT then reports `%DCL-I-INVDEF, ... does not exist`. SET DEFAULT refuses only bad syntax: unbalanced brackets give `%DCL-W-DIRECT`, and more than eight levels gives `%RMS-F-DIR`.
