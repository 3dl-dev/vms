#!/usr/bin/env python3
"""check_lab_password_literals.py -- no lab node password is committed (rd vms-aa2).

The lab VAX/Alpha nodes' passwords (SYSTEM, DNTEST, the oracle-capture
SYSTEM password) are operator facts: benches and lab tools read them from the
environment (VAX_PW, DN_PW, VAXC_PASS, SYSTEM_PW, LAB_PW, LAB_NEWPW), filled
from the ovmx-lab k8s secret. The values that once leaked onto main were
rotated on every lab disk; this gate keeps them, and any value added to KNOWN,
from coming back.

KNOWN holds SHA-256 digests of the upper-cased literals, not the literals, so
the gate itself never republishes them. Every word of 6..32 letters/digits in
every tracked text file is upper-cased, hashed, and compared.

    check_lab_password_literals.py <repo>       scan; exit 1 on any hit
    check_lab_password_literals.py --selftest   prove the scanner finds a planted hit
"""
import hashlib
import os
import re
import subprocess
import sys
import tempfile

KNOWN = {
    "ace0313d107f6c85b24a884c57794b96842cd9de00a4f7b3bed5cdd7254333bd",  # lab VAXC SYSTEM
    "999715a5340fafe8cc114bc6df0f9b1b0f9a41c93cb5e2370a0c839c5355529c",  # lab DNTEST account
    "fd8e6f02ad9ac94c6d605b420c79397be4983e437c48f41ac1f91ff1f8d62eb4",  # oracle-capture SYSTEM
    "3311a9c40a8bd8391debf1d4af3aacabb4085bb67b39b06b92fd938703c9923e",  # lab-Alpha golden SYSTEM (pre-2026-11-05)
}
WORD = re.compile(rb"[A-Za-z0-9]{6,32}")


def digest(word):
    return hashlib.sha256(word.upper()).hexdigest()


def scan_bytes(data, known=KNOWN):
    """Line numbers (1-based) of every line carrying a known literal."""
    hits = []
    for n, line in enumerate(data.split(b"\n"), 1):
        for m in WORD.finditer(line):
            w = m.group(0)
            # a literal glued to following text ("PW21-Oct") still starts a
            # word: test every prefix of plausible password length too
            for end in range(6, len(w) + 1):
                if digest(w[:end]) in known:
                    hits.append(n)
                    break
            else:
                continue
            break
    return hits


def tracked_files(repo):
    out = subprocess.check_output(["git", "-C", repo, "ls-files", "-z"])
    return [f for f in out.decode().split("\0") if f]


def scan_repo(repo, known=KNOWN):
    bad = []
    for rel in tracked_files(repo):
        path = os.path.join(repo, rel)
        if os.path.islink(path) or not os.path.isfile(path):
            continue
        try:
            with open(path, "rb") as fh:
                data = fh.read()
        except OSError:
            continue
        if b"\0" in data[:8192]:
            continue  # binary
        for n in scan_bytes(data, known):
            bad.append("%s:%d" % (rel, n))
    return bad


def selftest():
    planted = "zZplantedPW1x"
    known = {digest(planted.encode())}
    with tempfile.TemporaryDirectory() as d:
        subprocess.check_call(["git", "init", "-q", d])
        with open(os.path.join(d, "clean.py"), "w") as fh:
            fh.write("PW = os.environ['VAX_PW']\n")
        with open(os.path.join(d, "leak.py"), "w") as fh:
            fh.write("# header\nPW = '%s'\n" % planted.lower())
        with open(os.path.join(d, "glued.log"), "w") as fh:
            fh.write("Password: %s21-Oct-2026 16:15:06\n" % planted)
        subprocess.check_call(["git", "-C", d, "add", "."])
        bad = sorted(scan_repo(d, known))
    want = ["glued.log:1", "leak.py:2"]
    if bad != want:
        print("SELFTEST FAIL: got %r, want %r" % (bad, want))
        return 1
    if scan_bytes(b"VAX_PW = os.environ['VAX_PW']") != []:
        print("SELFTEST FAIL: false positive on an env read")
        return 1
    print("selftest: OK (planted literal found case-insensitively, glued, not in env reads)")
    return 0


def main(argv):
    if argv[1:] == ["--selftest"]:
        return selftest()
    if len(argv) != 2:
        print(__doc__)
        return 2
    bad = scan_repo(argv[1])
    for b in bad:
        print("%s: a rotated lab password literal is committed -- read it from the environment (rd vms-aa2)" % b)
    if bad:
        return 1
    print("check_lab_password_literals: OK (no known lab password literal in tracked files)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
