#!/usr/bin/env python3
"""check_transport_seam.py - userspace reaches the executive ONLY through the
libvmssys transport seam (rd vms-bbde; docs/design-executive-process-lifecycle.md
section 5.1).

Fails when C code in src/ outside the seam and outside the executive itself
  * names a VMS_IOCTL_* request (the transport's encoding of a service), or
  * spells the executive device node "/dev/vms" in a string literal.

The seam is src/libvmssys (vms_kif.c, the policy layer; kif_transport_*.c, the
substrate transports; kif_calls.h, the explicit-handle service names). The
executive trees (src/kernel, src/kernel-core, src/kernel-netbsd) define the
requests and are out of scope. Comments are ignored -- prose may name the
device; code may not.

  check_transport_seam.py [ROOT]              check the tree (default: repo root)
  check_transport_seam.py --file F [F ...]    check just these files (negctl)

Exit 0 clean, 1 on a violation, 2 on a usage error.
"""
import os
import re
import sys

SEAM_DIRS = ("src/libvmssys/",)
EXEC_DIRS = ("src/kernel/", "src/kernel-core/", "src/kernel-netbsd/")
TOKEN = re.compile(r"\bVMS_IOCTL_[A-Z0-9_]+")
DEVLIT = re.compile(r'"/dev/vms"')


def strip_comments(text):
    """Remove /* */ and // comments, keeping string and char literals intact
    (a "/*" inside a string is not a comment)."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            q = c
            j = i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        else:
            out.append(c)
            i += 1
    return "".join(out)


STRLIT = re.compile(r'"(?:[^"\\\n]|\\.)*"')


def violations(path, text):
    """A request token counts only in code (a message string that mentions one
    is prose); the device literal counts wherever code spells it."""
    found = []
    code = strip_comments(text)
    for lineno, line in enumerate(code.split("\n"), 1):
        for m in TOKEN.finditer(STRLIT.sub('""', line)):
            found.append((path, lineno, "names the transport request %s" % m.group(0)))
        if DEVLIT.search(line):
            found.append((path, lineno, 'spells the executive device "/dev/vms"'))
    return found


def in_scope(rel):
    if not rel.startswith("src/") or not rel.endswith((".c", ".h")):
        return False
    return not rel.startswith(SEAM_DIRS + EXEC_DIRS)


def main(argv):
    if len(argv) > 1 and argv[1] == "--file":
        files = [(f, f) for f in argv[2:]]
        if not files:
            print(__doc__)
            return 2
    else:
        root = os.path.abspath(argv[1] if len(argv) > 1 else
                               os.path.join(os.path.dirname(__file__), "..", ".."))
        files = []
        for d, _, names in os.walk(os.path.join(root, "src")):
            for nm in names:
                full = os.path.join(d, nm)
                rel = os.path.relpath(full, root).replace(os.sep, "/")
                if in_scope(rel):
                    files.append((full, rel))
    bad = []
    for full, rel in sorted(files):
        with open(full, encoding="utf-8", errors="replace") as fh:
            bad.extend(violations(rel, fh.read()))
    if bad:
        print("FAIL: %d place(s) reach the executive around the transport seam" % len(bad))
        for path, lineno, why in bad:
            print("  %s:%d: %s" % (path, lineno, why))
        print("Route the call through libvmssys (vms_kif_* or kif_calls.h kif_call) -- rd vms-bbde.")
        return 1
    print("OK: %d source file(s) outside the seam name no transport request and no /dev/vms"
          % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
