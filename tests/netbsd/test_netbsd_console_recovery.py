#!/usr/bin/env python3
#
# test_netbsd_console_recovery.py - unit test for netbsd_console.NetBSDConsole's
# lossy-serial end-marker recovery (rd vms-f8a).
#
# Under QEMU TCG the emulated serial console intermittently drops output: a
# command runs to completion but its `echo <marker>=$?=' line never reaches
# pexpect. A plain expect(marker, timeout) then stalls for the WHOLE budget on a
# marker that will never arrive (observed: a fast, idempotent module-absent
# negctl probe hanging 1200s in cold CI). run()/_await_marker() recover: if the
# idle prompt reappears WITHOUT the marker, the command finished but its marker
# line was lost, so an idempotent command is re-issued -- while a backgrounding
# command (which must not be duplicated) and any retriable=False command are
# NOT.
#
# This exercises that logic deterministically with a scripted fake pexpect child
# -- no QEMU needed -- so the recovery path is covered without waiting on a
# probabilistic real desync. Run: python3 tests/netbsd/test_netbsd_console_recovery.py
# (exit 0 = all cases pass, nonzero = failure).

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pexpect

import netbsd_console as nc


class _FakeMatch(object):
    def __init__(self, rc):
        self._rc = rc

    def group(self, _n):
        return str(self._rc)


class _FakeChild(object):
    """The slice of the pexpect API NetBSDConsole.run() uses, scripting a guest.

    `scenario' is one outcome per sendline: 'lost' -> the end marker was dropped
    and the idle prompt (expect index 1) appears instead; 'ok' -> the marker
    (index 0) arrives with exit status `rc'.
    """

    def __init__(self, scenario, rc=3):
        self.scenario = scenario
        self.rc = rc
        self.sent = []
        self.before = b"OUTPUT-LINE"
        self.match = None
        self._i = -1

    def sendline(self, s):
        self.sent.append(s)
        self._i += 1

    def expect(self, patterns, timeout=None):
        outcome = self.scenario[self._i]
        if outcome == "lost":
            return 1                 # idle prompt, no marker -> marker was lost
        self.match = _FakeMatch(self.rc)
        return 0                     # marker matched


def _console(child):
    con = nc.NetBSDConsole(child, logfn=lambda _m: None)
    con.prompt_re = re.escape("OVMX-RDY-deadbeef> ")
    con._resync_prompt = lambda _t: None   # its own path is not under test here
    return con


def _expect_ok(name, scenario, want_rc, want_sends):
    child = _FakeChild(scenario, rc=want_rc)
    rc, _out = _console(child).run("vmsmbx create_hold 1", timeout=300, echo=False)
    assert rc == want_rc, "%s: rc=%r != %r" % (name, rc, want_rc)
    assert len(child.sent) == want_sends, \
        "%s: %d send(s) != %d" % (name, len(child.sent), want_sends)
    print("PASS %s: rc=%d after %d send(s)" % (name, rc, len(child.sent)))


def _expect_no_retry(name, cmd, **kw):
    child = _FakeChild(["lost"])
    try:
        _console(child).run(cmd, timeout=5, echo=False, **kw)
    except pexpect.TIMEOUT:
        assert len(child.sent) == 1, "%s: %d send(s) != 1" % (name, len(child.sent))
        print("PASS %s: NOT re-issued (1 send), raised TIMEOUT" % name)
        return
    raise AssertionError("%s: expected TIMEOUT (no re-issue)" % name)


def main():
    # Idempotent command: recovered across 0, 1, 2 dropped markers.
    _expect_ok("clean",        ["ok"],               3, 1)
    _expect_ok("lost-then-ok", ["lost", "ok"],       3, 2)   # the cold-CI failure class
    _expect_ok("lost-lost-ok", ["lost", "lost", "ok"], 3, 3)

    # A backgrounding launch must never be re-issued (would spawn a duplicate).
    _expect_no_retry("bg-no-retry",
                     "vmsproctab bg P4APROC1 >/tmp/x 2>&1 & echo $! > /tmp/p")
    # An explicitly non-idempotent command (e.g. modload) must not be re-issued.
    _expect_no_retry("modload-no-retry", "modload x", retriable=False)

    print("ALL RECOVERY UNIT CASES PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
