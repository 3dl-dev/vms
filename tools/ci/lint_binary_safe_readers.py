#!/usr/bin/env python3
"""lint_binary_safe_readers.py -- a test that reads a VALUE out of a console log
must be binary-safe (rd vms-906).

usage: lint_binary_safe_readers.py <repo-root>      lint the tree
       lint_binary_safe_readers.py --selftest <repo-root>
                                                    lint it, then PROVE the lint
                                                    reds a planted violation

WHY THIS EXISTS, MEASURED. Merge-group run 39698326725 reddened the SCSNODE and
CLUSTER_CONFIG_LAN e2e gates and took every group behind #1841 down with it. The
cause was one option letter: `sysgen_count()` read a QEMU console log with
`grep -oE`. A console log carries NUL bytes, GNU grep therefore calls the file
BINARY, and `-o` prints NOTHING even though the matching line is right there --
so the helper returned an empty string and four checks compared against it.
Every OTHER reader in those files used `grep -q`, where binary detection still
EXITS 0 on a match, which is why only the reader that needed the TEXT was bitten.

WHY IT IS FLAKY-CLASS AND NOT JUST A BUG. grep's binary detection fires when a
NUL appears in the data it has read, so the SAME code passes or fails depending
on where in the file the NUL lands relative to the match. A gate whose verdict
depends on byte timing is a broken gate, not an intermittent one.

THE TWO RULES, both mechanical:

  RULE 1 -- `grep` with `-o` MUST carry `-a`.
      `-o` prints the MATCHED TEXT, which is exactly what binary detection
      suppresses. `-a` ("treat binary as text") is a no-op on text input, so
      this rule can be satisfied everywhere without changing any behaviour.

  RULE 2 -- a `grep` whose LINE OUTPUT IS CAPTURED from a FILE OPERAND must
      carry `-a` (or be one of the status/count/name-only forms -q/-c/-l/-L,
      which binary detection does not corrupt).
      On a binary file grep prints "Binary file X matches" INSTEAD of the line,
      so `X=$(grep PAT file)` is the same defect wearing different clothes.

WHAT IS DELIBERATELY NOT FLAGGED, and why it is sound:
  * a grep reading a PIPE from printf/echo of a shell variable -- a shell
    variable cannot contain a NUL (command substitution strips them), which is
    why test_sysgen_versioning_e2e.sh passed in the very run that failed;
  * `grep -q` / `-c` / `-l` with their output discarded or used as a status:
    binary detection changes neither the exit code nor the count;
  * `sed -n ...p` and `awk` readers: neither has binary detection at all. They
    are listed by `--report` for the record, not failed.

CLEAN-ROOM: reads this repository's own shell scripts.
"""
import os
import re
import sys

# The directories this lint governs: every harness that drives a QEMU or SIMH
# console. rd vms-906's done-condition names the first two; the rest are here
# because the same console logs are read there.
SCAN_DIRS = (
    os.path.join('tests', 'qemu'),
    os.path.join('tests', 'lab', 'tools'),
    os.path.join('tests', 'lab-vax'),
    os.path.join('tests', 'lab-alpha'),
    os.path.join('tests', 'integration'),
)

GREP = re.compile(r'(?<![\w/-])grep\b([^|;&)#\n]*)')
OPTS = re.compile(r'(?<![\w-])-([A-Za-z]+)\b')
# A file operand: a quoted "$VAR"/"$VAR/path", a $VAR, or a bare path with a /
# or a .log/.txt/.out suffix. Deliberately conservative -- a pattern argument is
# usually quoted with ' and holds no / at the start.
FILE_OPERAND = re.compile(r'"\$[A-Za-z_][\w]*[^"]*"|\$[A-Za-z_][\w]*|'
                          r'(?<![\'"])[\w./$-]+\.(?:log|txt|out|pcap|dmp)\b')
STATUS_ONLY = set('qcl')          # -q -c -l -L: binary-safe by construction
SOFT = ('sed -n', 'awk ')


def shell_files(root):
    out = []
    for d in SCAN_DIRS:
        base = os.path.join(root, d)
        if not os.path.isdir(base):
            continue
        for name in sorted(os.listdir(base)):
            if name.endswith('.sh'):
                out.append(os.path.join(base, name))
    return out


def opt_letters(argstr):
    """Every short-option letter in one grep invocation's arguments."""
    letters = set()
    for m in OPTS.finditer(argstr):
        letters.update(m.group(1))
    return letters


def strip_strings(argstr):
    """The invocation with quoted arguments blanked, so a PATTERN that happens
    to contain `-o`, `foo.log` or `-- '-ovmx'` is not read as an option or a
    file. Double quotes are kept when they look like a VARIABLE operand, which
    is how a file argument is spelled here."""
    return re.sub(r"'[^']*'", " '' ", argstr)


def violations_in(path):
    """[(line number, rule, the text)] for one script."""
    out = []
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for n, line in enumerate(f, 1):
            code = line.split('#', 1)[0]
            if 'grep' not in code:
                continue
            for m in GREP.finditer(code):
                args = m.group(1)
                # Option letters are read with quoted arguments BLANKED: a
                # pattern like `-- '-ovmx'` is not an option cluster, and
                # reading it as one made this lint flag grep -q (measured
                # 2026-10-22, 72 violations -> 71).
                letters = opt_letters(strip_strings(args))
                if 'a' in letters:
                    continue
                if 'o' in letters:
                    out.append((n, 'RULE 1 (-o needs -a)', line.rstrip()))
                    continue
                if letters & STATUS_ONLY:
                    continue
                if '>/dev/null' in code or '> /dev/null' in code:
                    continue
                # RULE 2: a captured line read from a FILE operand.
                captured = '$(' in code or '`' in code
                if not captured:
                    continue
                tail = strip_strings(args)
                # drop the first non-option token: that is the PATTERN
                toks = [t for t in tail.split() if not t.startswith('-')]
                if len(toks) < 2:
                    continue          # pattern only: the input is a pipe
                if any(FILE_OPERAND.fullmatch(t) for t in toks[1:]):
                    out.append((n, 'RULE 2 (captured line from a file needs -a)',
                                line.rstrip()))
    return out


def report_soft(root):
    """sed/awk readers, listed for the record. Neither has binary detection."""
    n = 0
    for path in shell_files(root):
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            for line in f:
                if any(s in line for s in SOFT):
                    n += 1
    return n


def lint(root, quiet=False):
    bad = []
    for path in shell_files(root):
        for n, rule, text in violations_in(path):
            bad.append((os.path.relpath(path, root), n, rule, text))
    if not quiet:
        print('lint_binary_safe_readers: %d scripts, %d sed/awk readers '
              '(listed, not failed), %d violation(s)'
              % (len(shell_files(root)), report_soft(root), len(bad)))
        for rel, n, rule, text in bad:
            print('  %s:%d  %s\n      %s' % (rel, n, rule, text.strip()))
    return bad


def premise_holds():
    """THE MECHANISM, re-measured rather than remembered: on THIS system's grep,
    does a NUL in the file really suppress `-o` output, and does `-a` really
    restore it? The lint is only worth running if it does, and if a future grep
    stops behaving this way this check SAYS SO instead of passing vacuously.

    Returns (suppressed_without_a, restored_with_a, note).
    """
    import subprocess
    import tempfile
    fd, path = tempfile.mkstemp()
    try:
        with os.fdopen(fd, 'wb') as f:
            f.write(b'noise\x00noise\n%SYSGEN-I-LOADED, 41 parameters\n')
        def run(args):
            r = subprocess.run(['grep'] + args + ['-oE', '[0-9]+ parameters',
                                                  path],
                               capture_output=True)
            return r.stdout.decode('latin1').strip()
        without, with_a = run([]), run(['-a'])
        return (without == '', with_a == '41 parameters',
                'without -a: %r   with -a: %r' % (without, with_a))
    finally:
        os.unlink(path)


def selftest(root):
    suppressed, restored, note = premise_holds()
    print('  premise (a NUL in the file suppresses -o, and -a restores it): %s'
          % note)
    if not (suppressed and restored):
        print('  NOTE: this grep does not show the measured behaviour. The lint '
              'is still enforced (the rule is portable and free), but the '
              'defect it exists for would not reproduce here.')
    bad = lint(root)
    if bad:
        print('FAIL: the tree has %d violation(s)' % len(bad))
        return 1
    # TEETH: plant one of each rule in a scratch copy and require the lint to
    # catch both. A lint that cannot fail proves nothing (rd vms-181's rule).
    import tempfile
    import shutil
    ok = True
    for rule, text in (
            ('RULE 1', "N=$(grep -oE '%SYSGEN-I-LOADED, [0-9]+' \"$LOG\")\n"),
            ('RULE 2', "ROW=$(grep -F 'Username:' \"$CONSOLE_LOG\")\n")):
        tmp = tempfile.mkdtemp()
        try:
            d = os.path.join(tmp, 'tests', 'qemu')
            os.makedirs(d)
            with open(os.path.join(d, 'test_planted.sh'), 'w') as f:
                f.write('#!/bin/sh\n' + text)
            got = lint(tmp, quiet=True)
            if len(got) == 1 and got[0][2].startswith(rule):
                print('  ok: the lint reds a planted %s violation' % rule)
            else:
                print('  FAIL: a planted %s violation was NOT caught (%r)'
                      % (rule, got))
                ok = False
        finally:
            shutil.rmtree(tmp)
    print('lint_binary_safe_readers selftest: %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main(argv):
    if len(argv) >= 2 and argv[1] == '--selftest':
        return selftest(argv[2] if len(argv) > 2 else '.')
    root = argv[1] if len(argv) > 1 else '.'
    bad = lint(root)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
