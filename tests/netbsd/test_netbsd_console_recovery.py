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

    def __init__(self, outcomes=(), rc=3, always_timeout=False,
                 always_prompt=False, single_outcomes=()):
        self.outcomes = list(outcomes)
        self.rc = rc
        self.always_timeout = always_timeout
        self.always_prompt = always_prompt
        # results for SINGLE-pattern expects (the nudge/resync prompt probes):
        # 'prompt' -> the idle prompt appeared; anything else -> TIMEOUT.
        self.single_outcomes = list(single_outcomes)
        self.sent = []
        self.before = b"OUTPUT-LINE"
        self.match = None

    def sendline(self, s):
        self.sent.append(s)

    def expect(self, patterns, timeout=None):
        if not isinstance(patterns, (list, tuple)):
            # SINGLE-pattern probe: a nudge/resync waiting for the idle prompt.
            o = self.single_outcomes.pop(0) if self.single_outcomes else "timeout"
            if o == "prompt":
                self.match = _FakeMatch(0)
                return 0
            raise pexpect.TIMEOUT("scripted single-pattern timeout")
        # Guard against regressing to the anita-incompatible TIMEOUT-in-list form.
        assert pexpect.TIMEOUT not in patterns and pexpect.EOF not in patterns, \
            "pexpect.TIMEOUT/EOF must NOT be in the pattern list (anita logs " \
            "self.match.group(0) after expect() and would crash)"
        if self.always_prompt:            # marker forever lost -> only the prompt
            self.match = _FakeMatch(0)
            return 1
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


def _assert_console_carries_only_markers():
    """run() MUST route a normal command's output to a guest file and echo only a
    bounded tail + the marker -- so bulk output can never flood the emulated UART
    and drop the marker (rd vms-f8a). A background LAUNCH must NOT be wrapped
    (subshell would change job semantics); its caller already redirects."""
    # normal command -> wrapped: output to the file, bounded tail to the console.
    child = _FakeChild(["marker"])
    _console(child).run("ls -R /root/ovmx", timeout=5, echo=False)
    sent = child.sent[0]
    assert nc.NetBSDConsole._LASTOUT in sent and "tail -c" in sent \
        and sent.lstrip().startswith("("), \
        "run() must wrap a normal command's output to a file; got: %s" % sent
    assert "ls -R /root/ovmx" in sent
    print("PASS console-only-markers: normal command's output is file-routed "
          "(subshell + tail -c), never streamed raw to the console")

    # background launch -> NOT wrapped (no subshell around the `&').
    child = _FakeChild(["marker"])
    _console(child).run("foo >/tmp/x 2>&1 &", timeout=5, echo=False)
    sent = child.sent[0]
    assert not sent.lstrip().startswith("(") and nc.NetBSDConsole._LASTOUT not in sent, \
        "a background launch must NOT be subshell-wrapped; got: %s" % sent
    print("PASS console-only-markers: background launch left unwrapped "
          "(job semantics preserved)")


def main():
    _assert_console_carries_only_markers()

    # Idempotent command: recovered across 0, 1, 2 dropped markers.
    _run_ok("clean",        ["marker"],                    3, 1)
    _run_ok("lost-then-ok", ["prompt", "marker"],          3, 2)   # cold-CI class
    _run_ok("lost-lost-ok", ["prompt", "prompt", "marker"], 3, 3)

    # HIGH drop rate: a long burst of dropped markers must still recover to the
    # deadline (retry is bounded by the OVERALL timeout, not a fixed count -- the
    # fixed-3 that gave up on the 4th cold dispatch is gone). 12 drops -> 13 sends.
    _run_ok("burst-12-drops", ["prompt"] * 12 + ["marker"], 3, 13)

    # Marker NEVER delivered (idempotent cmd): retry to the deadline, then a clean
    # TIMEOUT -- must not hang past the budget and must not crash.
    child = _FakeChild(always_prompt=True)
    try:
        _console(child).run("some idempotent phase", timeout=0.3, echo=False)
        raise AssertionError("persistent-drop: expected TIMEOUT")
    except pexpect.TIMEOUT:
        assert len(child.sent) >= 2, \
            "persistent-drop: should have re-issued several times, got %d" \
            % len(child.sent)
        print("PASS persistent-drop-to-deadline: re-issued %d times then TIMEOUT "
              "(no hang, no crash)" % len(child.sent))

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

    # BOTH-DROP nudge: a slice elapses with NEITHER marker nor prompt (both were
    # dropped); a nudge's probe sees the idle prompt (no marker) -> fast re-issue
    # signal (None), not a deadline burn (the failure that reddened a cold job on
    # a zero-output `rm -f'). Assert a nudge newline WAS sent.
    child = _FakeChild(outcomes=["timeout", "prompt"])
    con = _console(child)
    con._MARKER_SLICE = 0.02
    rc = con._await_marker("mk", total_timeout=30, allow_nudge=True)
    assert rc is None, "both-drop-nudge: rc=%r != None" % rc
    assert "" in child.sent, "both-drop-nudge: no nudge newline was sent"
    print("PASS both-drop-nudge: nudge probe saw prompt-without-marker -> fast "
          "re-issue (marker+prompt both dropped, no deadline burn)")

    # CRITICAL: a slow command that FINISHES during the nudge window leaves the
    # marker right before the prompt; the nudge probe must match the MARKER (not
    # the trailing prompt) and return rc -- NOT falsely declare it lost and
    # re-issue (that looped a build restarting itself). Assert rc is returned.
    child = _FakeChild(outcomes=["timeout", "marker"], rc=5)
    con = _console(child)
    con._MARKER_SLICE = 0.02
    rc = con._await_marker("mk", total_timeout=30, allow_nudge=True)
    assert rc == 5, "nudge-finds-marker: rc=%r != 5 (false re-issue!)" % rc
    print("PASS nudge-finds-marker: marker arriving during the nudge window is "
          "honored (no false re-issue of a slow command)")

    # No nudge when nudging is disallowed (e.g. a background launch): a stray
    # newline must NEVER be injected; the deadline is the only backstop.
    child = _FakeChild(always_timeout=True)
    con = _console(child)
    con._MARKER_SLICE = 0.02
    rc = con._await_marker("mk", total_timeout=0.2, allow_nudge=False)
    assert rc is None
    assert "" not in child.sent, "no-nudge: a nudge was injected when disallowed"
    print("PASS no-nudge-when-disallowed: no newline injected while awaiting a "
          "non-nudge (e.g. background-launch) marker")

    # A command that ENDS by backgrounding a job (trailing `&') must never be
    # re-issued (would duplicate it).
    _run_no_retry("bg-trailing-no-retry", "vmsproctab bg P4APROC1 >/tmp/x 2>&1 &")
    # A ` & ' launch followed by bookkeeping (the P2c/shared-driver shape) must
    # ALSO not be re-issued by default -- guards P2c against a duplicate launch.
    _run_no_retry("bg-amp-no-retry",
                  "vmseflag wait 66 >/tmp/w 2>&1 & echo $! >/tmp/wpid")
    # An explicitly non-idempotent command must not be re-issued.
    _run_no_retry("nonidempotent-no-retry", "some raw command", retriable=False)

    # A collapsed proof phase backgrounds a helper INTERNALLY (` & ') but runs to
    # completion and passes bg_safe=True -- it MUST retry despite the ` & '.
    child = _FakeChild(["prompt", "marker"], rc=3)
    con = _console(child)
    rc, _o = con.run("vmsproctab bg X >/tmp/a 2>&1 & sleep 2; kill %1; echo T=PASS",
                     timeout=300, echo=False, bg_safe=True)
    assert rc == 3 and len(child.sent) == 2, \
        "bg_safe-retries: rc=%r sends=%d" % (rc, len(child.sent))
    print("PASS bg_safe-retries: ` & '-internal phase with bg_safe=True re-issued "
          "on marker loss (2 sends)")

    print("ALL RECOVERY UNIT CASES PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
