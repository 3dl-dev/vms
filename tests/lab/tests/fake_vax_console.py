#!/usr/bin/env python3
"""Fake KA655 console, for testing nodedrv.py's boot injection. TEST-ONLY.

Stands in for `./vax vax.ini` under nodedrv.py's pty. Like SIMH it puts the
tty in raw mode and does its OWN echoing, so a test can make a character
vanish exactly the way the emulated console silo loses one.

env:
  FAKE_MODE    clean | drop_once | drop_always | autoboot
  FAKE_DROP_AT 1-based index of the received character to swallow (default 4,
               which is the '5' of 'B/R5:10000000 DUA0' -> corrupt R5 token)
  FAKE_RESULT  file; every command line terminated by CR is appended to it.
               A test asserts on this: it is what the ROM would have executed.

'clean'      echoes everything faithfully.
'drop_once'  loses one character on the first typing pass only (the real bug:
             a retry succeeds).
'drop_always' loses it on every pass (injection must give up, never send CR).
'autoboot'   never prints '>>>' -- boots straight to VMS, the way a console
             with 'dep bdr 0' does (vms-d0f duplicate-VAX1 drift).
"""
import os, sys, tty, time

MODE = os.environ.get('FAKE_MODE', 'clean')
DROP_AT = int(os.environ.get('FAKE_DROP_AT', '4'))
RESULT = os.environ.get('FAKE_RESULT', '/tmp/fake_vax_result')


def out(b):
    os.write(1, b)


def sink():
    """Swallow input forever so the pty stays open."""
    while True:
        try:
            if not os.read(0, 1):
                return
        except OSError:
            return


def main():
    try:
        tty.setraw(0)
    except Exception:
        pass
    out(b'\r\nKA655-B V5.3, VMB 2.7\r\n')

    if MODE == 'autoboot':
        time.sleep(0.5)
        out(b'\r\n%VMS-I-BOOT, booting console default root\r\n')
        out(b'\r\nENTER DATE AND TIME (DD-MMM-YYYY HH:MM) ')
        sink()
        return

    out(b'\r\n>>> ')
    line = bytearray()
    nchar = 0      # characters received since the line was last cleared
    passes = 0     # typing passes started (a pass = first char after a ^U)
    while True:
        try:
            c = os.read(0, 1)
        except OSError:
            break
        if not c:
            break

        if c == b'\x15':                    # ^U -- delete line
            line.clear()
            nchar = 0
            out(b'^U\r\n>>> ')
            continue

        if c == b'\r':                      # execute
            cmd = bytes(line)
            with open(RESULT, 'ab') as f:
                f.write(cmd + b'\n')
            out(b'\r\n%VMS-I-BOOT ' + cmd + b'\r\n')
            out(b'\r\nENTER DATE AND TIME (DD-MMM-YYYY HH:MM) ')
            line.clear()
            nchar = 0
            continue

        nchar += 1
        if nchar == 1:
            passes += 1
        drop = (nchar == DROP_AT and
                (MODE == 'drop_always' or (MODE == 'drop_once' and passes == 1)))
        if drop:
            continue                        # lost in the silo: no echo, not stored
        line += c
        out(c)


if __name__ == '__main__':
    main()
