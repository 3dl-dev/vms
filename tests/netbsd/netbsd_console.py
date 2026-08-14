"""
netbsd_console.py - deterministic serial-console driving for the OVMX/NetBSD
QEMU harnesses (rd vms-2d9, epic vms-8e8). Shared by drive_netbsd.py (P2a),
drive_netbsd_p2b.py and drive_netbsd_p2c.py so EVERY NetBSD CI job drives the
guest console the same, reliable way.

WHY THIS EXISTS -- "flaky = broken" (CLAUDE.md Rule 8). The harnesses talk to a
NetBSD guest over an emulated serial console via pexpect. Under TCG (GitHub
runners have no /dev/kvm) the guest is slow and its output arrives in bursts.
The previous approach -- `sendline(cmd)` then `expect(FIXED_MARKER, FIXED_CLOCK)`
against the default `# ' prompt -- desynchronised intermittently:

  * `# ' is not unique: it occurs inside ordinary command/build output, so an
    `expect(r"# ")' could match the WRONG thing and leave the driver a command
    out of step forever after.
  * a command was sent WITHOUT first confirming the shell was idle, so if the
    previous command was still draining a burst, the new keystrokes interleaved
    with that output and the shell received a garbled line -- its reply marker
    then never appeared and the driver hit `pexpect.TIMEOUT'.
  * the pexpect buffer grew unbounded during a big burst; each incoming chunk
    re-scanned the whole accumulated buffer (O(n^2)), so a verbose command could
    make even a correct `expect` time out on the DRIVER side.

The result was random `pexpect.exceptions.TIMEOUT' failures that reddened NetBSD
CI jobs at random and blocked unrelated PRs. This module removes the
non-determinism at the source.

THE DETERMINISTIC CONTRACT
  1. A UNIQUE, HIGH-ENTROPY SHELL PROMPT. After login we set PS1 to a random
     sentinel (`OVMX-RDY-<nonce>> ') that cannot occur in normal output, so the
     idle prompt is matched UNAMBIGUOUSLY -- never a stray `# ' inside a log. The
     nonce lives in a shell variable and PS1 references it via ${..}, so the
     command LINE we send to set it does NOT contain the literal prompt (only the
     real, freshly-printed prompt does).
  2. RESYNC ON THE PROMPT, NOT A CLOCK. Every command is bracketed by the prompt:
     `run()' returns to the idle prompt after each command, so the NEXT command
     is only ever sent into an idle shell. A send can no longer interleave with a
     draining burst.
  3. A UNIQUE PER-COMMAND END MARKER carrying $?. Each command appends
     `echo <fresh-nonce>=$?=' and we expect THAT nonce. A reply is paired to
     exactly its own command -- never to stale output from a previous one.
  4. BOUNDED PER-COMMAND OUTPUT is the caller's job (redirect verbose builds to a
     file; see the drivers). Combined with (2), the pexpect buffer stays small so
     the O(n^2) scan can never dominate.

This is OVMX's own code, written against the public pexpect API; it copies no
third-party source (CLAUDE.md Rule 8).
"""

import re
import time

import pexpect

try:
    import secrets

    def _nonce(nbytes=8):
        return secrets.token_hex(nbytes)
except ImportError:  # very old Python; still fine
    import random

    def _nonce(nbytes=8):
        return "".join("%02x" % random.getrandbits(8) for _ in range(nbytes))


class NetBSDConsole(object):
    """Deterministic pexpect driver for a NetBSD serial console (rd vms-2d9).

    Construct with the live pexpect child and an optional log callable. Call
    wait_for_login() once the guest is booting, then login_root_sh() to reach a
    /bin/sh with a UNIQUE prompt, then run() for each command.
    """

    # How long each expect() slice waits for a command's marker-or-prompt before
    # re-checking. The wait is sliced (not one blocking expect for the whole
    # budget) so a LOST marker -- detected by the idle prompt reappearing without
    # the marker -- is recovered in one slice instead of stalling the full budget.
    _MARKER_SLICE = 120

    def __init__(self, child, logfn=None):
        self.child = child
        self._log = logfn or (lambda _m: None)
        self.prompt_re = None      # set by set_unique_prompt(); None until login
        # NB: do NOT set child.searchwindowsize here. login_root_sh() matches the
        # NON-unique default `# ' prompt several times; bounding the search window
        # can make pexpect match a DIFFERENT `# ' than the buffer's first when the
        # boot motd is large, desynchronising the whole login handshake. The
        # lossy-serial robustness lives in run()/_await_marker instead, which key
        # on the UNIQUE end marker + unique prompt and so are safe to bound.

    # ---- boot / login ---------------------------------------------------
    def wait_for_login(self, boot_deadline):
        """Answer terminal DA/DSR queries until the `login:' prompt appears.

        The NetBSD getty probes the terminal (Device Attributes / Device Status
        Report escape sequences) before painting `login:'; a dumb pexpect pipe
        must answer or the prompt never comes. Unchanged in behaviour from the
        per-driver version this replaces.
        """
        self.child.timeout = boot_deadline
        while True:
            r = self.child.expect([r"\033\[c", r"\033\[5n", r"login:"])
            if r == 0:
                self.child.send("\033[?1;2c")
            elif r == 1:
                self.child.send("\033[0n")
            else:
                return

    def login_root_sh(self, cmd_timeout=300):
        """Log in as root, exec /bin/sh, set PATH, then install a UNIQUE prompt.

        After this returns, self.prompt_re matches ONLY the idle shell prompt and
        run() is usable.
        """
        self.child.timeout = cmd_timeout
        self.child.send("\n")
        self.child.expect(r"login:")
        self.child.send("root\n")
        self.child.expect(r"# ")
        self.child.sendline("exec /bin/sh")
        self.child.expect(r"# ")
        self.child.sendline(
            "PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH; umask 022")
        self.child.expect(r"# ")
        # CRITICAL for TCG reliability (rd vms-2d9): tame the serial console's
        # line discipline before running any real command.
        #   * `-echo': DISABLE input echo. Under loaded CI TCG the guest tty echoes
        #     a long command line back with 80-column WRAP artifacts (a stray
        #     space + BACKSPACE \x08 at the margin), which both confuses the
        #     pexpect scan and, worse, can garble what the shell parses (an
        #     unbalanced `if..fi' leaves it hung at a `> ' continuation prompt, so
        #     the end marker never comes -> pexpect.TIMEOUT). With echo off there
        #     is NO echoed input to wrap; only real command OUTPUT (the short end
        #     marker + the prompt) reaches pexpect, cleanly. We still log every
        #     command Python-side, so nothing is lost for debugging.
        #   * wide `columns'/`rows': so any long OUTPUT line does not wrap either.
        self.child.sendline("stty -echo columns 1000 rows 200 2>/dev/null")
        self.child.expect(r"# ")
        self.set_unique_prompt()

    def set_unique_prompt(self):
        """Replace PS1 with a high-entropy sentinel and sync to it.

        The nonce is stored in a shell variable; PS1 references it via ${..}, so
        the command line we send to set PS1 contains `${VAR}', not the expanded
        prompt -- only the real prompt the shell then prints carries the nonce.
        So expect(prompt_re) can only match the genuine idle prompt.
        """
        tag = _nonce(8)
        var = "__OVMXP"
        self.child.sendline("%s=%s" % (var, tag))
        self.child.expect(r"# ")                       # still the default prompt
        prompt = "OVMX-RDY-%s> " % tag
        self.prompt_re = re.escape(prompt)
        self.child.sendline('PS1="OVMX-RDY-${%s}> "' % var)
        self.child.expect(self.prompt_re)              # sync onto the new prompt

    # ---- command execution ----------------------------------------------
    def run(self, cmd, timeout=300, echo=True, retriable=True, bg_safe=False):
        """Run one /bin/sh command; return (exit_status, output_text).

        Precondition: the shell is at the idle unique prompt (guaranteed by
        login_root_sh(), or by the previous run()'s trailing prompt resync). We
        send the command plus a UNIQUE end marker carrying $?, wait for exactly
        that marker (so the reply is unambiguously this command's), then RESYNC to
        the idle prompt.

        LOSSY-SERIAL RECOVERY (rd vms-f8a). Under TCG the emulated serial console
        intermittently drops output: a command runs to completion but its
        `echo <marker>=$?=' line never reaches pexpect, and a plain
        expect(marker, timeout) then stalls for the WHOLE budget on a marker that
        will never arrive (observed: a fast, idempotent module-absent negctl probe
        hanging 1200s in cold CI). _await_marker() distinguishes the two cases by
        watching for the idle prompt: if the prompt reappears WITHOUT the marker,
        the command finished but its marker line was lost, so -- for an idempotent
        command -- we re-issue it (bounded attempts) instead of hanging. This is
        NOT a timeout bump: a genuinely slow guest, which keeps the prompt
        withheld, still gets the full `timeout'. A backgrounding command is NEVER
        re-issued (that would spawn a duplicate job); pass retriable=False for any
        other non-idempotent command (e.g. modload).
        """
        if self.prompt_re is None:
            raise RuntimeError("login_root_sh()/set_unique_prompt() not called")
        # A command that backgrounds a job it LEAVES RUNNING must not be blindly
        # re-issued (it would duplicate the job). Detect it conservatively: a
        # trailing `&', or a ` & ' launch followed by bookkeeping (the shape P2c
        # and the other shared-driver harnesses use). A collapsed proof phase that
        # backgrounds a helper INTERNALLY but runs it to completion -- kills it
        # and emits a single result token before returning -- is idempotent; it
        # passes bg_safe=True to opt back into retry despite containing ` & '.
        is_bg = (cmd.rstrip().endswith("&") or " & " in cmd) and not bg_safe
        can_retry = retriable and not is_bg
        # Retry an idempotent command to the OVERALL deadline, not a fixed count:
        # under a lossy TCG serial the marker can be dropped several times in a row
        # (a burst), so a fixed 3 gives up too early. Each re-issue gets whatever
        # budget remains; a genuinely slow guest still gets the whole `timeout' on
        # a single attempt (the prompt stays withheld, so _await_marker keeps
        # waiting rather than declaring the marker lost). The fix that actually
        # makes this converge, though, is FEWER markers: the driver runs each proof
        # phase as ONE in-guest command that emits a single result token.
        overall_deadline = time.time() + timeout
        attempt = 0
        while True:
            attempt += 1
            remaining = overall_deadline - time.time()
            if remaining <= 0:
                break
            mk = "OVMXm-%s" % _nonce(8)
            # A command ending in a bare `&' backgrounds a job -- `&' is ALREADY a
            # statement separator in /bin/sh, so appending `; echo ...' after it is
            # a syntax error (empty command between `&' and `;': NetBSD's ash
            # rejects it outright, "Syntax error: \";\" unexpected", which silently
            # drops the whole line and starves the driver of its end marker until
            # pexpect times out -- looks exactly like a hung guest, isn't one).
            # `& echo ...' (no `;') is valid: it launches the background job, then
            # runs `echo $?=' as the next statement on the same line, reporting the
            # shell's exit status for STARTING the job (the caller wants the launch
            # to have succeeded, not the async job's eventual completion status).
            if cmd.rstrip().endswith("&"):
                self.child.sendline("%s echo %s=$?=" % (cmd, mk))
            else:
                self.child.sendline("%s; echo %s=$?=" % (cmd, mk))

            rc = self._await_marker(mk, remaining)
            if rc is not None:
                out = self.child.before
                # Resync to the idle prompt (tolerant of a dropped prompt: we
                # already have rc, so a lost trailing prompt just needs a fresh
                # one nudged, not the whole command re-run).
                self._resync_prompt(min(timeout, 120))
                if isinstance(out, bytes):
                    out = out.decode("ascii", "ignore")
                out = out.replace("\r", "")
                if echo:
                    tail = "" if attempt == 1 else \
                        "   (recovered after %d lost marker(s))" % (attempt - 1)
                    self._log("$ %s   -> exit %d%s" % (cmd, rc, tail))
                    text = out.strip()
                    if text:
                        for line in text.splitlines():
                            print("    | %s" % line, flush=True)
                return rc, out

            # rc is None: the command completed but its end marker was lost to a
            # serial desync and we are back at an idle prompt. Re-issue idempotent
            # commands to the overall deadline; a bg/non-retriable one cannot be
            # safely re-run, so give up now with a clear timeout.
            if not can_retry:
                break
            self._log("  (console: end marker for `%s' lost to a serial desync; "
                      "shell is idle -- re-issuing, attempt %d)" % (cmd, attempt + 1))

        raise pexpect.TIMEOUT(
            "end marker never returned for `%s' within %ds after %d attempt(s) "
            "(retriable=%s, is_bg=%s)" % (cmd, timeout, attempt, retriable, is_bg))

    def _resync_prompt(self, timeout):
        """Wait for the idle unique prompt after a command's marker was seen.

        Tolerant of a serial desync that drops the trailing prompt: since the
        exit status is already in hand, a lost prompt just needs a fresh one
        nudged (a bare newline into the idle shell) rather than the command
        re-run. The unique prompt cannot be forged by command output, so a nudge
        is unambiguous.
        """
        try:
            self.child.expect(self.prompt_re, timeout=timeout)
            return
        except pexpect.TIMEOUT:
            pass
        for _ in range(3):
            self.child.sendline("")
            try:
                self.child.expect(self.prompt_re, timeout=20)
                return
            except pexpect.TIMEOUT:
                continue
        raise pexpect.TIMEOUT("idle prompt never resynced after a command")

    def _await_marker(self, mk, total_timeout):
        """Wait up to total_timeout for THIS command's end marker.

        Returns the int exit status once the marker is seen. Returns None if the
        idle prompt appears WITHOUT the marker first -- i.e. the command finished
        but its `echo <marker>=$?=' output was dropped by a transient serial
        desync -- so the caller may re-issue an idempotent command rather than
        burn the whole budget. A still-running (slow) guest withholds the prompt,
        so we keep waiting, sliced, up to total_timeout. The marker always prints
        BEFORE the prompt, so in the normal case idx==0 wins.
        """
        # IMPORTANT: do NOT put pexpect.TIMEOUT/EOF in this pattern list. The
        # child is anita's pexpect subclass whose expect() unconditionally logs
        # self.match.group(0) after every call; on a TIMEOUT-in-list expiry
        # self.match is the TIMEOUT *class*, so that log call raises
        # `AttributeError: type object 'TIMEOUT' has no attribute group'
        # (crashed a cold dispatch, rd vms-f8a). Catch the timeout as an
        # exception instead: pexpect raises it BEFORE anita's match-logging line,
        # so it never touches self.match.
        marker_re = r"%s=(\d+)=" % re.escape(mk)
        deadline = time.time() + total_timeout
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                return None
            try:
                idx = self.child.expect(
                    [marker_re, self.prompt_re],
                    timeout=min(remaining, self._MARKER_SLICE))
            except pexpect.TIMEOUT:
                # Neither the marker nor the prompt this slice -> the command is
                # still running (slow guest). Keep waiting up to total_timeout.
                continue
            if idx == 0:
                return int(self.child.match.group(1))
            return None                # idle prompt, no marker -> marker was lost

    def expect_prompt(self, timeout=120):
        """Wait for the idle unique prompt (used to resync after an ad-hoc
        interaction that did not go through run())."""
        self.child.expect(self.prompt_re, timeout=timeout)
