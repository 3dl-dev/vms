#!/usr/bin/env python3
"""Persistent SIMH VAX node driver.

  nodedrv.py <nodedir> <logpath> [--date "25-JUL-2026 20:15"]

Forks ./vax vax.ini in <nodedir> under a pty. Tees console -> <logpath>.
Creates an input FIFO at <logpath>.in : anything written to it is sent to the
VAX console (append \\r yourself, or send a line and we translate \\n->\\r).
Auto-answers boot prompts:
    '>>>'                 -> 'B DUA0'      (once)
    'ENTER DATE AND TIME' -> the --date    (each time seen)
Runs until killed (kill the pid in <logpath>.pid). Designed to be launched in
the background and then poked with:  printf 'SHOW CLUSTER\\r' > <logpath>.in
"""
import os, sys, pty, select, time, errno

def main():
    a = sys.argv[1:]
    nodedir, logpath = a[0], a[1]
    date = "25-JUL-2026 20:15"
    if '--date' in a: date = a[a.index('--date')+1]
    boot = "B DUA0"
    if '--boot' in a: boot = a[a.index('--boot')+1]
    fifopath = logpath + '.in'
    try: os.mkfifo(fifopath)
    except FileExistsError: pass

    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(nodedir)
        os.execv('./vax', ['./vax', 'vax.ini'])
        os._exit(127)

    with open(logpath + '.pid', 'w') as f: f.write(str(os.getpid()))
    log = open(logpath, 'wb', buffering=0)
    # open FIFO read end non-blocking, keep a write end so we never get EOF
    fifo_r = os.open(fifopath, os.O_RDONLY | os.O_NONBLOCK)
    fifo_w = os.open(fifopath, os.O_WRONLY)  # keep-open writer

    tail = b''
    boot_sent = False
    while True:
        try:
            r,_,_ = select.select([fd, fifo_r], [], [], 2.0)
        except (OSError, ValueError):
            break
        if fd in r:
            try: data = os.read(fd, 4096)
            except OSError as e:
                if e.errno == errno.EIO: break
                raise
            if not data: break
            log.write(data)
            tail = (tail + data)[-600:]
            if not boot_sent and b'>>>' in tail:
                time.sleep(1); os.write(fd, boot.encode()+b'\r'); boot_sent = True
                log.write(b'\n[drv:'+boot.encode()+b']\n'); tail = b''
            if b'ENTER DATE AND TIME' in tail:
                time.sleep(1); os.write(fd, date.encode()+b'\r')
                log.write(b'\n[drv:date]\n'); tail = b''
        if fifo_r in r:
            try: cmd = os.read(fifo_r, 4096)
            except OSError: cmd = b''
            if cmd:
                cmd = cmd.replace(b'\n', b'\r')
                os.write(fd, cmd)
    log.write(b'\n[drv:emulator exited]\n'); log.close()

if __name__ == '__main__':
    main()
