#!/usr/bin/env python3
"""Persistent SIMH VAX node driver.

  nodedrv.py <nodedir> <logpath> [--date "25-JUL-2026 20:15"] [--boot "B DUA0"]
             [--no-detach] [--boot-attempts N] [--blind-boot]

Forks ./vax vax.ini in <nodedir> under a pty. Tees console -> <logpath>.
Creates an input FIFO at <logpath>.in : anything written to it is sent to the
VAX console (append \\r yourself, or send a line and we translate \\n->\\r).
Auto-answers boot prompts:
    '>>>'                 -> --boot (default 'B DUA0')  (once, echo-verified)
    'ENTER DATE AND TIME' -> the --date                 (each time seen)
Runs until killed (kill the pid in <logpath>.pid). Designed to be launched in
the background and then poked with:  printf 'SHOW CLUSTER\\r' > <logpath>.in

Two hardenings (vms-d3a; root cause shared with vms-d0f / vms-cd0 / vms-af2):

1. ECHO-VERIFIED CHAR-BY-CHAR BOOT INJECTION.  A blind burst write of
   'B/R5:10000000 DUA0' can lose a character in the emulated console silo.
   A corrupted R5 token silently selects the WRONG system root -- typically
   root SYS0 -- so the second node comes up as a duplicate 'VAX1' on the
   SHARED d0.dsk.  We therefore send the boot command one character at a
   time, wait for each character's echo, and only send the terminating CR
   once the accumulated echo ends with the exact command.  On mismatch we
   ^U the line and retry (--boot-attempts, default 3).

   If every attempt fails we DO NOT send CR.  The node stays halted at '>>>'
   -- an honest, obvious failure -- and we touch <logpath>.bootfail.  Booting
   an unverified root is never the safer option: a halted node is a two-line
   fix, a duplicate VAX1 writes to a shared disk.
   --blind-boot restores the old unverified burst write (escape hatch for a
   console that does not echo at all).

2. AUTO-BOOT DETECTION.  If --boot was given explicitly but the machine
   reaches VMS without ever halting at '>>>' (the vms-d0f 'dep bdr 0'
   unattended-auto-boot drift), the injection never ran and the node booted
   whatever root the console default names.  We cannot un-boot it, but we
   refuse to let it be silent: loud log marker + <logpath>.bootfail.

3. SETSID SELF-DETACH.  The driver re-parents itself to init (session leader)
   at startup so that an agent harness reaping its background tasks cannot
   kill the driver out from under a live node -- which stalls that node's
   console and, if it is the sole-vote node, quorum-hangs the whole lab
   (vms-af2).  --no-detach keeps it in the caller's session (tests, debugging).
"""
import os, sys, pty, select, time, errno

ECHO_TIMEOUT = 1.5     # seconds to wait for one character's echo
INTERCHAR    = 0.03    # pacing between characters
CTRL_U       = b'\x15'  # console delete-line

# Markers that mean "VMS is booting" -- if we see one of these and the boot
# command was never injected, the console auto-booted an unknown root.
BOOTED_MARKERS = (b'ENTER DATE AND TIME', b'SYSBOOT', b'OpenVMS')


def _read_some(fd, log, timeout):
    """Wait up to `timeout` for console output; log and return it (b'' on timeout)."""
    try:
        r, _, _ = select.select([fd], [], [], timeout)
    except (OSError, ValueError):
        return b''
    if fd not in r:
        return b''
    try:
        d = os.read(fd, 4096)
    except OSError:
        return b''
    if d:
        log.write(d)
    return d


def _drain(fd, log, secs):
    """Log everything the console emits for `secs`; return it."""
    out = b''
    end = time.monotonic() + secs
    while True:
        rem = end - time.monotonic()
        if rem <= 0:
            break
        d = _read_some(fd, log, rem)
        if not d:
            break
        out += d
    return out


def _visible(buf):
    """Reduce a raw echo stream to the characters that survived on the line."""
    out = bytearray()
    for b in buf:
        if b in (0x00, 0x0a):
            continue
        if b == 0x0d:            # CR -> the line restarts
            out.clear()
            continue
        if b == 0x08 or b == 0x7f:   # BS/DEL -> rub out one
            if out:
                out.pop()
            continue
        out.append(b)
    return bytes(out)


def _inject_boot_verified(fd, log, boot, attempts):
    """Type `boot` one echo-confirmed character at a time, then CR.

    Returns True only if the console echoed the command back exactly and the
    CR was sent. Never sends CR on an unverified line.
    """
    want = boot.encode()
    for att in range(1, attempts + 1):
        _drain(fd, log, 0.5)                 # let the prompt settle
        os.write(fd, CTRL_U)                 # start from a known-empty line
        _drain(fd, log, 0.5)
        echo = b''
        dropped = None
        for i in range(len(want)):
            ch = want[i:i + 1]
            os.write(fd, ch)
            end = time.monotonic() + ECHO_TIMEOUT
            got = b''
            while time.monotonic() < end:
                d = _read_some(fd, log, max(0.0, end - time.monotonic()))
                if not d:
                    continue
                got += d
                if ch in got:
                    break
            echo += got
            if ch not in got:
                dropped = i
                break
            time.sleep(INTERCHAR)

        line = _visible(echo)
        if dropped is None and line.endswith(want):
            os.write(fd, b'\r')
            log.write(b"\n[drv:boot-echo-verified attempt %d '%s']\n" % (att, want))
            return True

        why = (b'no echo for char %d (%r)' % (dropped, want[dropped:dropped + 1])
               if dropped is not None else b'echo mismatch')
        log.write(b"\n[drv:boot-echo-MISMATCH attempt %d: %s; want=%r line=%r]\n"
                  % (att, why, want, line))
        os.write(fd, CTRL_U)                 # discard the corrupt line
        _drain(fd, log, 0.5)
    return False


def main():
    a = sys.argv[1:]
    nodedir, logpath = a[0], a[1]
    date = "25-JUL-2026 20:15"
    if '--date' in a:
        date = a[a.index('--date') + 1]
    boot = "B DUA0"
    boot_explicit = '--boot' in a
    if boot_explicit:
        boot = a[a.index('--boot') + 1]
    attempts = 3
    if '--boot-attempts' in a:
        attempts = int(a[a.index('--boot-attempts') + 1])
    verify = '--blind-boot' not in a
    detach = '--no-detach' not in a

    if detach and os.getpid() != os.getsid(0):
        # Become a session leader parented to init: immune to the caller's
        # (or an agent harness's) process-group / background-task teardown.
        if os.fork() > 0:
            os._exit(0)
        os.setsid()

    fifopath = logpath + '.in'
    failpath = logpath + '.bootfail'
    try:
        os.unlink(failpath)
    except OSError:
        pass
    try:
        os.mkfifo(fifopath)
    except FileExistsError:
        pass

    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(nodedir)
        os.execv('./vax', ['./vax', 'vax.ini'])
        os._exit(127)

    with open(logpath + '.pid', 'w') as f:
        f.write(str(os.getpid()))
    log = open(logpath, 'wb', buffering=0)

    def fail(msg):
        log.write(b'\n[drv:' + msg + b']\n')
        try:
            with open(failpath, 'w') as fh:
                fh.write(msg.decode('ascii', 'replace') + '\n')
        except OSError:
            pass

    # open FIFO read end non-blocking, keep a write end so we never get EOF
    fifo_r = os.open(fifopath, os.O_RDONLY | os.O_NONBLOCK)
    fifo_w = os.open(fifopath, os.O_WRONLY)  # keep-open writer

    tail = b''
    boot_sent = False
    autoboot_flagged = False
    while True:
        try:
            r, _, _ = select.select([fd, fifo_r], [], [], 2.0)
        except (OSError, ValueError):
            break
        if fd in r:
            try:
                data = os.read(fd, 4096)
            except OSError as e:
                if e.errno == errno.EIO:
                    break
                raise
            if not data:
                break
            log.write(data)
            tail = (tail + data)[-600:]
            if not boot_sent and b'>>>' in tail:
                boot_sent = True
                if verify:
                    if not _inject_boot_verified(fd, log, boot, attempts):
                        fail(b"BOOT-INJECT-FAILED after %d attempts: console echo never "
                             b"matched %r -- CR NOT sent, node left halted at >>> rather "
                             b"than booting an unverified root" % (attempts, boot.encode()))
                else:
                    time.sleep(1)
                    os.write(fd, boot.encode() + b'\r')
                    log.write(b'\n[drv:' + boot.encode() + b' (blind, unverified)]\n')
                tail = b''
            if (boot_explicit and not boot_sent and not autoboot_flagged
                    and any(m in tail for m in BOOTED_MARKERS)):
                autoboot_flagged = True
                fail(b"AUTOBOOT-WITHOUT-INJECTION: reached VMS without ever halting at "
                     b'>>>, so --boot %r never ran. The console auto-booted its default '
                     b'root -- on a shared disk this is how a node comes up as a '
                     b'DUPLICATE VAX1 (vms-d0f). Check nodedir vax.ini for an active '
                     b"'dep bdr 0'." % boot.encode())
            if b'ENTER DATE AND TIME' in tail:
                time.sleep(1)
                os.write(fd, date.encode() + b'\r')
                log.write(b'\n[drv:date]\n')
                tail = b''
        if fifo_r in r:
            try:
                cmd = os.read(fifo_r, 4096)
            except OSError:
                cmd = b''
            if cmd:
                cmd = cmd.replace(b'\n', b'\r')
                os.write(fd, cmd)
    log.write(b'\n[drv:emulator exited]\n')
    log.close()


if __name__ == '__main__':
    main()
