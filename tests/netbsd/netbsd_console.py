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

    def __init__(self, child, logfn=None):
        self.child = child
        self._log = logfn or (lambda _m: None)
        self.prompt_re = None      # set by set_unique_prompt(); None until login

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
    def run(self, cmd, timeout=300, echo=True):
        """Run one /bin/sh command; return (exit_status, output_text).

        Precondition: the shell is at the idle unique prompt (guaranteed by
        login_root_sh(), or by the previous run()'s trailing prompt resync). We
        send the command plus a UNIQUE end marker carrying $?, expect exactly that
        marker (so the reply is unambiguously this command's), then RESYNC to the
        idle prompt -- draining the marker line's tail so the next command is,
        again, only ever sent into an idle shell.
        """
        if self.prompt_re is None:
            raise RuntimeError("login_root_sh()/set_unique_prompt() not called")
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
        self.child.expect(r"%s=(\d+)=" % re.escape(mk), timeout=timeout)
        rc = int(self.child.match.group(1))
        out = self.child.before
        # Resync to the idle prompt. Generous but bounded: draining a marker line
        # is trivial even on a slow guest, but give it room under TCG.
        self.child.expect(self.prompt_re, timeout=min(timeout, 120))
        if isinstance(out, bytes):
            out = out.decode("ascii", "ignore")
        out = out.replace("\r", "")
        if echo:
            self._log("$ %s   -> exit %d" % (cmd, rc))
            text = out.strip()
            if text:
                for line in text.splitlines():
                    print("    | %s" % line, flush=True)
        return rc, out

    def expect_prompt(self, timeout=120):
        """Wait for the idle unique prompt (used to resync after an ad-hoc
        interaction that did not go through run())."""
        self.child.expect(self.prompt_re, timeout=timeout)
