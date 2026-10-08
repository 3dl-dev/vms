# Keystroke oracle: the OVMX terminal vs a real OpenVMS terminal (rd vms-370)

The semantic oracle (`docs/oracle/semantics/`) measures what system services *return*.
This directory measures what a person at the keyboard *sees*: when a typed character
is echoed (on receipt, or only when a read consumes it), what DELETE, ^U, ^R, ^X, ^J
put on the screen, what ^C ^Y ^O ^T ^S ^Q do to output in flight, how a read prompt,
a timed read, a broadcast that arrives mid-line, a recalled command, a long wrapped
line, the login and logout look -- byte for byte.

## How it works

1. **Case scripts** -- `tools/oracle/keystroke/cases/<CASE>.ks`: keystrokes with
   timing (`send`, `type ... gap=`, `wait`), each step a segment of the session.
   The language is documented in `tools/oracle/keystroke/ksplay.py`.
2. **The player** -- `ksplay.py` plays a case into a terminal and records every
   output byte with its time. On a real node it writes the console driver's raw FIFO
   (`<log>.raw`: `tests/lab/nodedrv.py`, `tests/lab-alpha/tools/srmdrv.py -R`); on
   OVMX it owns the QEMU serial line (`-serial stdio`). Same script, same timing.
3. **Goldens** -- `<CASE>/alpha84.ks.txt` and `<CASE>/vax73.ks.txt`: the transcript
   the case produced on the console (`OPA0:`) of a real OpenVMS Alpha V8.4 and VAX
   V7.3 node (`tools/oracle/keystroke/capture_lab.sh`), unmasked, with every control
   byte spelled out (`<CR>`, `<NUL>`, `<BS>`, `<ESC>`, `<BEL>`). `<CASE>/*.timing.jsonl`
   is the raw byte log with timestamps (provenance; not compared). Each golden
   records the sha256 of the case script it was played from; editing a case without
   re-capturing fails `ksdiff.py --goldens`.
4. **The gate** -- CI's persistent-boot job boots OVMX on the real executive and
   plays every case into its console (`tests/qemu/test_keystroke_oracle.sh`), then
   `ksdiff.py` compares each step. A case's `@mask` lines (times, PIDs, node names)
   and `@squeeze` (collapse a steady stream's repeated lines) are applied to both
   sides at compare time. OVMX is compared with the **Alpha V8.4** golden; VAX is the
   fallback, and steps where VAX and Alpha differ are reported ARCH-DIVERGENT.
5. **The ratchet** -- `known-diff.txt` lists every step known to differ, each with the
   rd item that tracks it. An unlisted difference fails; so does a listed step that
   now matches. The list only shrinks.

The VAX console is an LA36 (hardcopy): DELETE echoes `\X\`, ^U starts a new line. The
Alpha console is a video terminal. Follow each architecture's own oracle.

Clean-room (AGENTS.md Rule 8): goldens are the observed bytes of real systems'
terminals. Nothing is disassembled and no VSI/HPE source is used.
