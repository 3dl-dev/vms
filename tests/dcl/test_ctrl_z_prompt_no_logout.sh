#!/bin/bash
# TEST: Ctrl/Z at the interactive DCL prompt does not log out (vms-a70)
# EXPECT: contains:BEFORE-CTRLZ
# EXPECT: contains:AFTER-CTRLZ-STILL-HERE
# EXPECT: contains:SESSION-ENDED-BY-LOGOUT
# EXPECT_NOT: contains:SESSION-ENDED-EARLY
#
# Oracle: tests/lab/captures/decnet-sethost-inbound-20261005/vax-dcl-ctrlz.txt
# (real VAX V7.3: Ctrl/Z at "$ " echoes *EXIT* and DCL prompts again). DCL runs
# on a real pty (interactive mode, VEOF = Ctrl/Z); a command written AFTER the
# Ctrl/Z must still execute. NEGCTL: before the fix DCL broke out of its REPL on
# that EOF, so AFTER-CTRLZ-STILL-HERE never printed and the session ended early.
VMSDCL="${VMSDCL:-vmsdcl}"
python3 - "$VMSDCL" <<'PY'
import os, pty, sys, time, select
dcl = sys.argv[1]
pid, fd = pty.fork()
if pid == 0:
    os.execvp(dcl, [dcl])
out = b""
def pump(t):
    global out
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                d = os.read(fd, 4096)
            except OSError:
                return False
            if not d:
                return False
            out += d
    return True
pump(2.0)
os.write(fd, b'WRITE SYS$OUTPUT "BEFORE-CTRLZ"\r'); pump(1.5)
os.write(fd, b'\x1a'); pump(1.5)
alive = pump(0.2)
os.write(fd, b'WRITE SYS$OUTPUT "AFTER-CTRLZ-STILL-HERE"\r'); pump(1.5)
os.write(fd, b'LOGOUT\r'); pump(2.0)
_, status = os.waitpid(pid, 0)
txt = out.decode("latin-1")
sys.stdout.write(txt)
# The command after Ctrl/Z ran only if DCL survived the EOF.
if "AFTER-CTRLZ-STILL-HERE" in txt.split("AFTER-CTRLZ-STILL-HERE\"", 1)[-1]:
    print("\nSESSION-ENDED-BY-LOGOUT")
else:
    print("\nSESSION-ENDED-EARLY")
PY
