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
