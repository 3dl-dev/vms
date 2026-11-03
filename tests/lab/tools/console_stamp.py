#!/usr/bin/env python3
"""console_stamp.py <console-log> <lead-cs> <line-b64> <ere-b64 | -> -- the
in-pod half of `lab-console.sh stamp` (rd vms-6731).

Writes ONE line (CR appended) to <console-log>.in, the console's input FIFO,
exactly as `lab-console.sh send` does, but stamps it with the pod's own clock:
  * every %NOW% in the line becomes the pod's local time at the write plus
    <lead-cs> hundredths, in DCL absolute-time form dd-MMM-yyyy:hh:mm:ss.cc
    (what SET TIME=<time> takes);
  * with an ERE, it then watches the console log for the first NEW line that
    matches it (30 s at most) and notes when that line appeared.
It prints one line:
  WRITE <time> SEEN <time|-> DT <cs|-> LINE <the matched console line|->
WRITE is the pod clock at the write (without the lead), SEEN when the matched
line was first seen in the log, DT their difference in hundredths: the window
in which the node evaluated the line. Exit 1 if the ERE never matched.

The reference clock is the pod's because every console in it (the SIMH VAXes
and the QEMU OVMX member) runs on that host; the driver runs elsewhere.
"""
import base64
import datetime
import os
import re
import sys
import time


def dcltime(t):
    d = datetime.datetime.fromtimestamp(t)
    return d.strftime('%d-%b-%Y:%H:%M:%S.').upper() + '%02d' % (d.microsecond // 10000)


def main():
    if len(sys.argv) != 5:
        print(__doc__.split('\n')[0], file=sys.stderr)
        return 2
    log, lead = sys.argv[1], int(sys.argv[2])
    line = base64.b64decode(sys.argv[3]).decode()
    ere = None if sys.argv[4] == '-' else re.compile(base64.b64decode(sys.argv[4]).decode())
    start = os.path.getsize(log)
    with open(log + '.in', 'w') as f:
        t0 = time.time()
        f.write(line.replace('%NOW%', dcltime(t0 + lead / 100.0)) + '\r')
    if ere is None:
        print('WRITE %s SEEN - DT - LINE -' % dcltime(t0))
        return 0
    buf = ''
    deadline = t0 + 30
    while time.time() < deadline:
        with open(log, 'rb') as f:
            f.seek(start)
            buf = f.read().decode('latin-1')
        now = time.time()
        for ln in buf.replace('\r', '').replace('\0', '').split('\n'):
            if ere.search(ln):
                print('WRITE %s SEEN %s DT %d LINE %s'
                      % (dcltime(t0), dcltime(now), round((now - t0) * 100), ln.strip()))
                return 0
        time.sleep(0.02)
    print('WRITE %s SEEN - DT - LINE -' % dcltime(t0))
    return 1


if __name__ == '__main__':
    sys.exit(main())
