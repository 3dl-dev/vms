#!/usr/bin/env python3
"""Tests for nodedrv.py boot hardening (vms-d3a). Run: python3 test_nodedrv_boot.py

Drives the REAL nodedrv.py against fake_vax_console.py -- no SIMH, no disks,
no bridge -- so it is safe to run while the lab is up.

What is asserted, and why it matters:
  clean       the exact boot command reaches the ROM
  drop_once   a lost console character is CAUGHT and RETRIED, and the command
              the ROM finally executes is still byte-exact. This is the
              duplicate-VAX1 bug: the old blind write executed the corrupted
              R5 token and booted the wrong system root.
  drop_always injection gives up, CR is NEVER sent (ROM executes nothing),
              .bootfail is written -- honest failure instead of a wrong root
  blind       --blind-boot reproduces the old unverified behaviour (documents
              that the escape hatch really is unverified)
  autoboot    a console that never halts at '>>>' is flagged, not silent
  detach      the driver is its own session leader (survives harness reaping)
"""
import os, re, shutil, signal, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
NODEDRV = os.path.join(HERE, '..', 'nodedrv.py')
FAKE = os.path.join(HERE, 'fake_vax_console.py')
BOOT = 'B/R5:10000000 DUA0'

failures = []


def check(name, cond, detail=''):
    print('%-4s %s%s' % ('ok' if cond else 'FAIL', name, '' if cond else '  -- ' + detail))
    if not cond:
        failures.append(name)


class Run:
    """One nodedrv.py run against a fake console in a throwaway nodedir."""

    def __init__(self, mode, extra=(), drop_at=4, detach=False):
        self.dir = tempfile.mkdtemp(prefix='nodedrv-test-')
        self.log = os.path.join(self.dir, 'node.log')
        self.result = os.path.join(self.dir, 'result')
        nodedir = os.path.join(self.dir, 'node')
        os.makedirs(nodedir)
        vax = os.path.join(nodedir, 'vax')
        with open(vax, 'w') as f:
            f.write('#!/bin/sh\nexec %s %s\n' % (sys.executable, FAKE))
        os.chmod(vax, 0o755)
        env = dict(os.environ, FAKE_MODE=mode, FAKE_RESULT=self.result,
                   FAKE_DROP_AT=str(drop_at))
        cmd = [sys.executable, NODEDRV, nodedir, self.log, '--boot', BOOT]
        if not detach:
            cmd.append('--no-detach')
        cmd += list(extra)
        self.p = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.STDOUT)

    def wait_for(self, pred, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if pred():
                return True
            time.sleep(0.2)
        return pred()

    def text(self):
        try:
            with open(self.log, 'rb') as f:
                return f.read().decode('latin-1')
        except OSError:
            return ''

    def commands(self):
        """Command lines the fake ROM actually executed."""
        try:
            with open(self.result, 'rb') as f:
                return [l.decode('latin-1') for l in f.read().splitlines()]
        except OSError:
            return []

    def bootfail(self):
        return os.path.exists(self.log + '.bootfail')

    def pid_file(self):
        for _ in range(50):
            try:
                with open(self.log + '.pid') as f:
                    return int(f.read().strip())
            except (OSError, ValueError):
                time.sleep(0.2)
        return None

    def stop(self):
        for p in (self.p.pid,):
            try:
                os.kill(p, signal.SIGKILL)
            except OSError:
                pass
        self.p.wait(timeout=10)
        shutil.rmtree(self.dir, ignore_errors=True)


def test_clean():
    r = Run('clean')
    try:
        r.wait_for(lambda: r.commands(), 30)
        cmds = r.commands()
        check('clean: ROM executed the exact boot command',
              cmds[:1] == [BOOT], 'got %r' % (cmds[:1],))
        check('clean: log records echo verification',
              'boot-echo-verified' in r.text(), r.text()[-300:])
        check('clean: no .bootfail', not r.bootfail())
    finally:
        r.stop()


def test_drop_once():
    r = Run('drop_once')
    try:
        r.wait_for(lambda: r.commands(), 40)
        cmds = r.commands()
        # The whole point: a character was lost, yet what ran is byte-exact.
        check('drop_once: ROM executed the exact boot command despite a dropped char',
              cmds[:1] == [BOOT], 'got %r' % (cmds[:1],))
        txt = r.text()
        check('drop_once: the drop was detected',
              'boot-echo-MISMATCH' in txt, txt[-400:])
        check('drop_once: and the retry verified',
              'boot-echo-verified' in txt, txt[-400:])
        check('drop_once: no .bootfail (it recovered)', not r.bootfail())
    finally:
        r.stop()


def test_drop_always():
    r = Run('drop_always', extra=('--boot-attempts', '2'))
    try:
        r.wait_for(lambda: r.bootfail(), 45)
        check('drop_always: .bootfail written', r.bootfail())
        txt = r.text()
        check('drop_always: BOOT-INJECT-FAILED logged',
              'BOOT-INJECT-FAILED' in txt, txt[-400:])
        # The load-bearing assertion: nothing was ever executed, so no root
        # was booted -- the node is halted at >>> instead of duplicating VAX1.
        check('drop_always: ROM executed NOTHING (no CR sent)',
              r.commands() == [], 'got %r' % (r.commands(),))
    finally:
        r.stop()


def test_blind_is_unverified():
    r = Run('drop_always', extra=('--blind-boot',))
    try:
        r.wait_for(lambda: r.commands(), 30)
        cmds = r.commands()
        check('blind: --blind-boot executes the CORRUPTED command (documents the risk)',
              len(cmds) == 1 and cmds[0] != BOOT, 'got %r' % (cmds,))
        check('blind: log marks it unverified',
              'blind, unverified' in r.text(), r.text()[-300:])
    finally:
        r.stop()


def test_autoboot_detected():
    r = Run('autoboot')
    try:
        r.wait_for(lambda: r.bootfail(), 30)
        check('autoboot: .bootfail written', r.bootfail())
        check('autoboot: AUTOBOOT-WITHOUT-INJECTION logged',
              'AUTOBOOT-WITHOUT-INJECTION' in r.text(), r.text()[-400:])
    finally:
        r.stop()


def test_setsid_detach():
    r = Run('clean', detach=True)
    try:
        pid = r.pid_file()
        check('detach: pid file written', pid is not None)
        if pid:
            # The property that makes it immune to a harness task-kill:
            # it is its own session leader, reparented to init.
            sid = os.getsid(pid)
            check('detach: driver is a session leader (sid == pid)',
                  sid == pid, 'pid=%d sid=%d' % (pid, sid))
            check('detach: launcher returned immediately',
                  r.p.wait(timeout=10) == 0)
            r.wait_for(lambda: r.commands(), 30)
            check('detach: detached driver still booted the node',
                  r.commands()[:1] == [BOOT], 'got %r' % (r.commands()[:1],))
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass
    finally:
        r.stop()


def test_no_detach_stays_in_session():
    r = Run('clean', detach=False)
    try:
        pid = r.pid_file()
        check('no-detach: driver keeps the caller session',
              pid == r.p.pid and os.getsid(pid) == os.getsid(0),
              'pid=%s popen=%s' % (pid, r.p.pid))
    finally:
        r.stop()


if __name__ == '__main__':
    for t in (test_clean, test_drop_once, test_drop_always, test_blind_is_unverified,
              test_autoboot_detected, test_setsid_detach, test_no_detach_stays_in_session):
        print('--- %s' % t.__name__)
        t()
    print()
    if failures:
        print('FAILED: %s' % ', '.join(failures))
        sys.exit(1)
    print('ALL PASS')
