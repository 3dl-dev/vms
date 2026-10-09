# Design: the executive terminal driver (rd vms-f8c, epic vms-4eba)

Status: steps 1 and 5 below (core + Linux port + NetBSD port + the console read path, DCL
and LOGINOUT on $QIO, termios toggles removed) landed together, per the ruling that the
driver lands on Linux and NetBSD at once. `tests/qemu/test_kmod_tt.c` pins the bytes; the
keystroke oracle (`docs/oracle/keystroke/known-diff.txt`) measures what remains.

**Problem.** OVMX has no VMS terminal driver. Echo, line editing and type-ahead are the
substrate's Unix line discipline (n_tty); DCL uses readline; ^Y/^C are SIGINT/SIGQUIT. The
keystroke oracle (vms-370, `docs/oracle/keystroke/`) measures the result: 22 of 121 steps
match a real VAX V7.3 console. The worst one is that a character typed while DCL is busy
is echoed on receipt. On VMS it waits, unechoed, in the type-ahead buffer until a read
consumes it.

**Sources.** Clean-room (AGENTS.md Rule 8):
- *OpenVMS I/O User's Reference Manual*, Terminal Driver chapter: read and write function
  codes and modifiers, the terminator mask, type-ahead, line editing, out-of-band ASTs,
  read verify, item-list reads.
- *OpenVMS User's Manual*: the line-editing keys and command recall.
- The keystroke goldens: the observed bytes.

## 1. Shape: a class driver over a port driver

This follows TTDRIVER's own split.

| layer | VMS | OVMX |
|---|---|---|
| class driver | TTDRIVER | `src/kernel-core/vms_tt.c`: substrate-free. It holds the type-ahead buffer, read and write request handling, the editing state machine, echo generation, out-of-band detection, and broadcast redisplay. |
| port driver | YCDRIVER, TTA/OPA port | the substrate tty, reached through a *line discipline* that the executive registers: Linux `tty_ldisc_ops` (`src/kernel/vms_tt_linux.c`), NetBSD `struct linesw "vms_tt"` (`src/kernel-netbsd/vms_tt_netbsd.c`). The port only moves bytes. |
| UCB | per-terminal UCB | `struct vms_tt` hung off the executive device row (`vms_devtab.c`): OPA0:, and RTAn: for ptys |

Using a line discipline means the substrate's n_tty is *replaced* on that tty, not toggled.
There is no ECHO/ICANON for userspace to set. The port delivers every received byte to
`vms_tt_receive()` and transmits whatever the class driver emits.

**Binding** a line to a unit is `vms_kif_tt_attach(fd, unit)`: the transport selects the
line discipline (Linux `TIOCSETD`, NetBSD `TIOCSLINED`) and issues `VMS_TTIOC_BIND` on the
tty. The executive grants it only to a caller holding **CMKRNL**, decided by
`vms_prot_require_priv()` (`vms_prot.h`) -- the SYSGEN CONNECT bar -- never a substrate
capability. STARTUP binds the console OPA0: right after the executive attaches;
`ovmx_vterm_create` (run by the SYSTEM network daemon) binds a new RTAn:'s pty before
`$CREPRC` starts LOGINOUT there; LOGINOUT binds its terminal if nobody has.

**The console outlives its sessions.** On Linux a session leader whose controlling terminal
is the console hangs the console up on exit and re-opens its line discipline; NetBSD can
close the console line by revoking it. The class-driver instance is parked across that
close and taken back by the re-open (`vms_tt_set_port`), with its type-ahead and
outstanding `$QIO` reads intact. A read(2) blocked on the line in the substrate's own read
path is ended (it holds the line-discipline reference the re-open needs). A pty's hangup
is its far end leaving: that ends the binding.

**Lifetime.** An instance is created by the bind and referenced by every reader and port
holder. The port's memory and the device row live until the last reference goes
(`ops->release`, `vms_devtab_tt_release`), so a read that wakes after its line went away
touches nothing freed. Port ops are bracketed so a port can be replaced under them.

## 2. The read path ($QIO IO$_READVBLK / READLBLK / READPROMPT)

- **Type-ahead.** With no read outstanding, a received byte goes into the per-terminal
  type-ahead ring and nothing is echoed. Out-of-band characters (§4) are the exception.
  - The ring size is TTY_TYPAHDSZ, or TTY_ALTYPAHD when the ALTYPEAHD characteristic is
    set.
  - When the ring is full: if HOSTSYNC is set, send XOFF; otherwise echo BEL and drop the
    byte.
  - A terminal without TYPEAHEAD rejects unsolicited input.
- **A read** may write a prompt (READPROMPT P5/P6, with carriage control) and purge
  type-ahead first (IO$M_PURGE). It then consumes type-ahead and new input one character
  at a time:
  - **echo when consumed** (unless IO$M_NOECHO or the NOECHO characteristic);
  - **edit** (§3) unless IO$M_NOFILTR or PASTHRU;
  - **terminate** on a character in the terminator mask: P4 short or long form, or the
    standard set when P4 is 0.
  - The IOSB carries the offset to the terminator, the terminator, and its size; escape
    sequences are handled under IO$M_ESCAPE.
  - IO$M_TIMED (P3 seconds) ends the read with SS$_TIMEOUT.
  - A signal does not end a read: it is *suspended* (slot, partial line, prompt, deadline
    kept; input still consumed and echoed) and resumed when the same owner re-enters
    (SA_RESTART, or the kif's `KIF_WAIT_CALL`). ^Y/^C end it SS$_ABORT in the driver.
  - $QIO P4 (terminator descriptor) and P5 (prompt) are address-sized in `starlet.h`,
    as 64-bit OpenVMS passes every $QIO argument as a quadword.
  - IO$M_TRMNOECHO suppresses echo of the terminator.
  - An IO$_READVBLK with an item list (IO$M_EXTEND) carries the initial string, used for
    recall, and the insert/overstrike mode.
- **Completion** is a real $QIO completion: IOSB, event flag and AST through the
  executive. It is never a synchronous userspace `read()`.

## 3. Line editing (VMS_STYLE_INPUT, LINE_EDITING, INSERT editing)

The driver owns these keys:
- DELETE: rub out the last character. A hardcopy terminal echoes the `\X\` form; a scope
  terminal echoes BS SP BS.
- ^U: delete to the start of the line.
- ^R: redisplay the prompt and the line.
- ^X: delete the line and purge type-ahead.
- ^J (LF): delete the word to the left.
- ^H/BS: move to the start of the line. ^E: move to the end.
- ^D and LEFT: move left. ^F and RIGHT: move right.
- ^A: toggle insert/overstrike for this read.
- ^Z: echoes `*EXIT*` and terminates.

The exact echo bytes per terminal type come from the goldens, not from this table. The VAX
console is an LA36, which is hardcopy.

## 4. Out-of-band characters

^C ^Y ^O ^T ^S ^Q are acted on **when received**, whether or not a read is outstanding:
- ^S/^Q: XON/XOFF, which holds or releases output, under TTSYNC.
- ^O: discard output until the next ^O, a read, or a write with IO$M_CANCTRLO. Echoes
  `*OUTPUT OFF*` / `*OUTPUT ON*`.
- ^Y and ^C: delivered as ASTs to the process that enabled them (IO$_SETMODE |
  IO$M_CTRLYAST / IO$M_CTRLCAST), through the executive's AST queue (`vms_ast.c`). DCL
  enables both and prints `*INTERRUPT*` itself. With no AST enabled, ^C is handled as ^Y.
- ^T: signals the process to print its status line.
- IO$M_OUTBAND (P2 mask): a generic out-of-band AST.

None of these is a Unix signal.

## 5. Write path and broadcast

IO$_WRITEVBLK / WRITELBLK go through the class driver. This gives it ^O discard, ^S hold,
wrap at the width, and the carriage control RMS asks for. It also lets the driver know
where the cursor is. $BRKTHRU / REPLY write *through* the driver (`vms_tt_broadcast()`). If
a read is in progress, the driver writes the message on its own lines, then redisplays the
prompt and the partial line. That is the BC.READ golden.

## 6. Userspace

- DCL, LOGINOUT and the CRTL read the terminal only by $QIO. DCL issues READPROMPT, not
  readline.
- Command recall belongs to DCL, as on VMS: up-arrow or ^B terminates the read, and DCL
  re-issues it with the recalled line as the initial string.
- Every `tcsetattr` ECHO/ICANON/ISIG toggle is removed:
  - `dcl_main.c` setup_vms_eof
  - `dcl_terminal.c` vms_terminal_apply ECHO
  - `dcl_cmd_set.c` dcl_read_noecho_line, which becomes IO$M_NOECHO
  - `sys_qio.c` qio_terminal_setmode PASSALL/NORMAL, which becomes the PASTHRU
    characteristic in the driver
  - `ovmx_init.c` boot_console_disable_echo, which is replaced by attaching the driver
  - LOGINOUT's ECHO re-enable
- With no executive there is no driver: a terminal $QIO returns SS$_NOSUCHDEV. There is no
  userspace fallback (Rule 9).

## 7. Device characteristics

The characteristics come from the executive device row: devchar/devchar2, width, page and
type, from `vms_devtab.c` and `vms_devtab_nb.h`. SET TERMINAL changes them there, and the
driver reads them per operation. Access control goes through the row's owner and
protection (`vms_prot.h`), not Unix permissions.

## 8. Landing, one reviewable PR per capability

Each PR is gated by the keystroke oracle's known-diff shrinking, and each new behaviour
gets a negctl defect anchor (`tests/qemu/facility_defects.sh`) with a `test_kmod_tt.c`
suite.

1. **Core + Linux port + console read path**
   - type-ahead held unechoed, echo on consume, terminators, READPROMPT, PURGE, NOECHO,
     TIMED
   - DCL and LOGINOUT read through $QIO
   - termios toggles gone
   - closes vms-d732 and most of vms-bc5
2. **Line editing** (vms-eda8) and **carriage control** (vms-fc4).
3. **Out-of-band ASTs**: ^Y ^C ^O ^T ^S ^Q (vms-f0fb). DCL's ^Y moves from SIGINT to the
   AST.
4. **Recall** via the item-list read and its initial string (vms-eb3d), **broadcast
   redisplay** (vms-53a), **wrap** (vms-cef).
5. **NetBSD port**: the `linesw` binding of the same core (VAX).
6. **CTERM / SET HOST**: RTAn: ptys get the same line discipline, so remote sessions ride
   the driver.
