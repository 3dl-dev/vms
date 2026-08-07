#!/usr/bin/env python3
"""srmdrv.py -- drive the AXPbox serial console over TCP (rd vms-e2c).

AXPbox exposes serial0 as a telnet listener. This is the Alpha equivalent of
lab-1's nodedrv.py: connect, send console/DCL lines, tee everything to a log so
a run is auditable after the fact.

Usage:
    srmdrv.py [-p PORT] [-t SECONDS] [-l LOGFILE] [-w WAIT_FOR] [cmd ...]

Commands are sent one per line. With no commands it just listens and dumps
whatever the console produces, which is what you want for a first power-on.
"""
import argparse
import os
import socket
import sys
import time

# Telnet negotiation bytes. AXPbox speaks just enough telnet that a raw socket
# sees IAC sequences interleaved with console text; strip them or the SRM
# banner arrives peppered with 0xFF.
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240


def strip_telnet(buf: bytes, sock: socket.socket) -> bytes:
    """Remove IAC sequences, refusing every option we are offered."""
    out = bytearray()
    i = 0
    while i < len(buf):
        b = buf[i]
        if b != IAC:
            out.append(b)
            i += 1
            continue
        if i + 1 >= len(buf):
            break
        cmd = buf[i + 1]
        if cmd in (DO, DONT, WILL, WONT):
            if i + 2 >= len(buf):
                break
            opt = buf[i + 2]
            # Refuse everything: DO->WONT, WILL->DONT.
            reply = WONT if cmd in (DO, DONT) else DONT
            try:
                sock.sendall(bytes([IAC, reply, opt]))
            except OSError:
                pass
            i += 3
        elif cmd == SB:
            j = buf.find(bytes([IAC, SE]), i)
            i = len(buf) if j < 0 else j + 2
        else:
            i += 2
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-H", "--host", default="127.0.0.1")
    ap.add_argument("-p", "--port", type=int, default=21264)
    ap.add_argument("-t", "--timeout", type=float, default=60.0,
                    help="total seconds to stay connected; 0 means forever, "
                         "which is what a persistent lab node wants")
    ap.add_argument("-f", "--fifo", default=None,
                    help="path to a FIFO to read further command lines from. "
                         "AXPBOX EXITS WHEN THE CONSOLE CLIENT DISCONNECTS, so "
                         "a node that must outlive one command needs a pump "
                         "that stays attached and takes work through this.")
    ap.add_argument("-l", "--log", default=None)
    ap.add_argument("-w", "--wait-for", default=None,
                    help="gate: hold commands until this string appears, and "
                         "exit 0 once it has (and all commands are sent). "
                         "Without it, commands go out on a timer and anything "
                         "sent before the console is ready is silently eaten.")
    ap.add_argument("-d", "--delay", type=float, default=1.0,
                    help="seconds to wait between sent commands")
    ap.add_argument("-C", "--connect-timeout", type=float, default=60.0,
                    help="seconds to keep retrying the initial connect while "
                         "the emulator starts up")
    ap.add_argument("cmds", nargs="*")
    args = ap.parse_args()

    # Retry the CONNECT rather than probing the port first. A probe that opens
    # and closes a connection is itself a console client disconnecting, and
    # AXPbox exits on that -- a readiness check of the obvious shape powers the
    # machine off before the real console ever attaches. So the first connection
    # made to this port must be the one that stays.
    sock = None
    give_up = time.time() + args.connect_timeout
    while True:
        try:
            sock = socket.create_connection((args.host, args.port), timeout=10)
            break
        except (ConnectionRefusedError, OSError):
            if time.time() >= give_up:
                sys.stderr.write(
                    f"[srmdrv] no console on {args.host}:{args.port} after "
                    f"{args.connect_timeout:.0f}s\n")
                return 1
            time.sleep(0.5)
    sock.settimeout(0.5)

    fifo_fd = None
    fifo_buf = bytearray()
    if args.fifo:
        if not os.path.exists(args.fifo):
            os.mkfifo(args.fifo, 0o600)
        # O_RDWR, not O_RDONLY: holding a writer ourselves means the read end
        # never sees EOF when a `echo ... > fifo` writer exits, so the pump
        # does not have to reopen between every command.
        fifo_fd = os.open(args.fifo, os.O_RDWR | os.O_NONBLOCK)

    log = open(args.log, "ab") if args.log else None
    seen = bytearray()
    deadline = float("inf") if args.timeout == 0 else time.time() + args.timeout
    pending = list(args.cmds)
    next_send = time.time() + args.delay
    hit = args.wait_for is None

    try:
        while time.time() < deadline:
            try:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                text = strip_telnet(chunk, sock)
                if text:
                    seen += text
                    sys.stdout.buffer.write(text)
                    sys.stdout.flush()
                    if log:
                        log.write(text)
                        log.flush()
            except socket.timeout:
                pass

            # Drain the FIFO. Whole lines only -- a partial write must not be
            # sent to the console as if it were a command.
            if fifo_fd is not None:
                try:
                    data = os.read(fifo_fd, 4096)
                    if data:
                        fifo_buf += data
                        while b"\n" in fifo_buf:
                            line, _, fifo_buf = fifo_buf.partition(b"\n")
                            pending.append(line.decode(errors="replace").rstrip("\r"))
                except BlockingIOError:
                    pass

            if args.wait_for and not hit and args.wait_for.encode() in seen:
                hit = True
                # In FIFO mode the pump must stay attached whatever happens --
                # dropping the connection kills the emulator.
                if not pending and fifo_fd is None:
                    break
                # The console only just became ready. Restart the send timer
                # here: commands queued before this point must NOT fire, and
                # the prompt has not finished painting yet.
                next_send = time.time() + args.delay

            # `hit` gates sending. With -w, nothing goes out until the console
            # has proved it is at the prompt -- otherwise the SRM firmware is
            # still in memory test, eats the line, and the run looks like the
            # command silently did nothing.
            if hit and pending and time.time() >= next_send:
                line = pending.pop(0)
                sock.sendall(line.encode() + b"\r")
                sys.stdout.write(f"\n[sent] {line}\n")
                sys.stdout.flush()
                next_send = time.time() + args.delay
    finally:
        sock.close()
        if fifo_fd is not None:
            os.close(fifo_fd)
        if log:
            log.close()

    if args.wait_for and not hit:
        sys.stderr.write(f"\n[srmdrv] TIMEOUT: never saw {args.wait_for!r}\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
