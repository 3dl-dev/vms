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
# command (which must not be duplicated) and any retriable=False command are NOT.
#
# This exercises that logic deterministically with a scripted fake pexpect child
# -- no QEMU needed. It covers:
#   * clean delivery,
#   * a dropped marker recovered by re-issue (0/1/2 drops),
#   * a slice TIMEOUT (neither marker nor prompt this slice) -> loop, no crash
#     (the exact path that crashed a cold dispatch: the child is anita's pexpect
#     subclass, whose expect() logs self.match.group(0), so pexpect.TIMEOUT must
#     NEVER be put in the pattern list -- _await_marker catches it as an
#     exception; the fake asserts TIMEOUT is absent from the list),
#   * a slice TIMEOUT that persists to the overall deadline -> None, no crash,
#   * bg launch / retriable=False never re-issued.
# Run: python3 tests/netbsd/test_netbsd_console_recovery.py  (exit 0 = pass).

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
    """The slice of the pexpect API NetBSDConsole uses, scripting a guest.

    `outcomes' is consumed one per expect() call:
      'marker'  -> the end marker matched (index 0), exit status `rc';
      'prompt'  -> the idle prompt matched (index 1), no marker (marker lost);
      'timeout' -> this slice expired with neither seen -> raise pexpect.TIMEOUT
                   (as pexpect does when TIMEOUT is NOT in the pattern list).
    When `always_timeout' is set, every expect() raises TIMEOUT (models a marker
    that is never delivered, so the overall deadline must be honored).
    """

    def __init__(self, outcomes=(), rc=3, always_timeout=False):
        self.outcomes = list(outcomes)
        self.rc = rc
        self.always_timeout = always_timeout
        self.sent = []
        self.before = b"OUTPUT-LINE"
        self.match = None

    def sendline(self, s):
        self.sent.append(s)

    def expect(self, patterns, timeout=None):
        # Guard against regressing to the anita-incompatible TIMEOUT-in-list form.
        assert pexpect.TIMEOUT not in patterns and pexpect.EOF not in patterns, \
            "pexpect.TIMEOUT/EOF must NOT be in the pattern list (anita logs " \
            "self.match.group(0) after expect() and would crash)"
        if self.always_timeout or not self.outcomes:
            raise pexpect.TIMEOUT("scripted timeout")
        o = self.outcomes.pop(0)
        if o == "timeout":
            raise pexpect.TIMEOUT("scripted slice timeout")
        if o == "marker":
            self.match = _FakeMatch(self.rc)
            return 0
        if o == "prompt":
            self.match = _FakeMatch(0)
            return 1
        raise ValueError("bad outcome %r" % o)


def _console(child):
    con = nc.NetBSDConsole(child, logfn=lambda _m: None)
    con.prompt_re = re.escape("OVMX-RDY-deadbeef> ")
    con._resync_prompt = lambda _t: None   # its own path is not under test here
    return con


def _run_ok(name, outcomes, want_rc, want_sends):
    child = _FakeChild(outcomes, rc=want_rc)
    rc, _out = _console(child).run("vmsmbx create_hold 1", timeout=300, echo=False)
    assert rc == want_rc, "%s: rc=%r != %r" % (name, rc, want_rc)
    assert len(child.sent) == want_sends, \
        "%s: %d send(s) != %d" % (name, len(child.sent), want_sends)
    print("PASS %s: rc=%d after %d send(s)" % (name, rc, len(child.sent)))


def _run_no_retry(name, cmd, **kw):
    child = _FakeChild(["prompt"])
    try:
        _console(child).run(cmd, timeout=5, echo=False, **kw)
    except pexpect.TIMEOUT:
        assert len(child.sent) == 1, "%s: %d send(s) != 1" % (name, len(child.sent))
        print("PASS %s: NOT re-issued (1 send), raised TIMEOUT" % name)
        return
    raise AssertionError("%s: expected TIMEOUT (no re-issue)" % name)


def main():
    # Idempotent command: recovered across 0, 1, 2 dropped markers.
    _run_ok("clean",        ["marker"],                    3, 1)
    _run_ok("lost-then-ok", ["prompt", "marker"],          3, 2)   # cold-CI class
    _run_ok("lost-lost-ok", ["prompt", "prompt", "marker"], 3, 3)

    # Slice TIMEOUT (neither marker nor prompt yet) must LOOP, not crash -- the
    # exact branch that crashed the cold dispatch. Here two slices time out (slow
    # guest) then the marker arrives.
    child = _FakeChild(["timeout", "timeout", "marker"], rc=7)
    rc = _console(child)._await_marker("OVMXm-abcd", total_timeout=300)
    assert rc == 7, "slice-timeout-then-marker: rc=%r != 7" % rc
    print("PASS slice-timeout-then-marker: looped past 2 slice timeouts -> rc=7")

    # Slice TIMEOUT that never resolves must honor the overall deadline and
    # return None (no crash), not spin forever.
    child = _FakeChild(always_timeout=True)
    con = _console(child)
    con._MARKER_SLICE = 0.01
    rc = con._await_marker("OVMXm-abcd", total_timeout=0.2)
    assert rc is None, "slice-timeout-until-deadline: rc=%r != None" % rc
    print("PASS slice-timeout-until-deadline: honored deadline -> None (no crash)")

    # A backgrounding launch must never be re-issued (would spawn a duplicate).
    _run_no_retry("bg-no-retry",
                  "vmsproctab bg P4APROC1 >/tmp/x 2>&1 & echo $! > /tmp/p")
    # An explicitly non-idempotent command (e.g. modload) must not be re-issued.
    _run_no_retry("modload-no-retry", "modload x", retriable=False)

    print("ALL RECOVERY UNIT CASES PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
