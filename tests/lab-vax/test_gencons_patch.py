#!/usr/bin/env python3
"""test_gencons_patch.py - host unit test for tools/cross-vax/netbsd-ovmx-gencons.patch
(rd vms-25f).

THE DEFECT. NetBSD's VAX console receive interrupt handler, gencnrint(), begins

    if (sc->alive == 0)
        return;

and only then reads RXDB. OVMX closes the console line whenever a session leader
exits (the vnode is revoked: gencnclose() clears `alive`), but the receiver's
interrupt stays enabled (RCSR<6>). A character that arrives in that window is
taken by the interrupt and ignored; RCSR<7> (DONE) stays set. The request is
never withdrawn: real hardware re-asserts it at once (a livelock at IPL 14), and
the edge-modelled simulator the browser demo runs on never raises it again, so the
console received nothing for the rest of the run. Reproduced deterministically in
the pcjs VAX with the V0.7-12 Node B disk: the interrupt is delivered (SCB 0xF8),
the handler takes its early return, RXCS reads 0xC0 forever, and intReq is zero.

WHAT THIS PINS, with no NetBSD tree and no emulator:
  1. TEETH: the unpatched upstream gencons.c has the early return with no RXDB read
     on the closed path (the defect exists in the source the build patches).
  2. The patch applies to that source, and afterwards the closed path reads RXDB
     before it returns.
  3. The open path is unchanged: still reads RXDB once, then hands it to the line
     discipline.
  4. The kernel build applies the patch, idempotently.

Run: pytest tests/lab-vax/test_gencons_patch.py -v
"""

import os
import re
import shutil
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PATCH = os.path.join(REPO, "tools/cross-vax/netbsd-ovmx-gencons.patch")
UPSTREAM = os.path.join(HERE, "fixtures/gencons.c.netbsd-10")
BUILD = os.path.join(REPO, "tools/cross-vax/build-vax-modular-kernel.sh")
REL = "sys/arch/vax/vax/gencons.c"


def rint_body(src):
    m = re.search(r"\nstatic void\ngencnrint\(void \*arg\)\n\{(.*?)\n\}\n", src, re.S)
    assert m, "gencnrint() not found"
    return m.group(1)


def closed_path_reads_rxdb(src):
    """True when the `alive == 0` arm reads RXDB before it returns."""
    body = rint_body(src)
    m = re.search(r"if \(sc->alive == 0\)(.*?)\n\ti = mfpr", body, re.S)
    assert m, "alive==0 guard not found"
    arm = m.group(1)
    return "mfpr(pr_rxdb" in arm and "return" in arm


def patched_tree():
    d = tempfile.mkdtemp(prefix="ovmx-gencons-")
    dst = os.path.join(d, REL)
    os.makedirs(os.path.dirname(dst))
    shutil.copy(UPSTREAM, dst)
    subprocess.run(["patch", "-d", d, "-p1", "-s", "-i", PATCH], check=True)
    return d, dst


def test_teeth_upstream_closed_path_does_not_read_rxdb():
    src = open(UPSTREAM).read()
    assert not closed_path_reads_rxdb(src), \
        "upstream gencnrint already reads RXDB when closed: the patch is unnecessary"


def test_patch_applies_and_closed_path_reads_rxdb():
    d, dst = patched_tree()
    try:
        assert closed_path_reads_rxdb(open(dst).read())
    finally:
        shutil.rmtree(d)


def test_open_path_unchanged():
    d, dst = patched_tree()
    try:
        body = rint_body(open(dst).read())
        tail = body.split("i = mfpr", 1)[1]
        assert "pr_rxdb[sc->unit]) & 0377" in "i = mfpr" + tail
        assert "(*tp->t_linesw->l_rint)(i, tp);" in tail
        assert body.count("mfpr(pr_rxdb") == 2, "one read per arm, no more"
    finally:
        shutil.rmtree(d)


def test_patch_is_idempotent_for_the_build():
    d, dst = patched_tree()
    try:
        r = subprocess.run(["patch", "-d", d, "-p1", "--dry-run", "-s", "-i", PATCH],
                           capture_output=True)
        assert r.returncode != 0, "a second apply must be detectable, so the build guards it"
        assert "rd vms-25f" in open(dst).read(), "the build's idempotence marker is in the patched file"
    finally:
        shutil.rmtree(d)


def test_kernel_build_applies_the_patch():
    sh = open(BUILD).read()
    assert "netbsd-ovmx-gencons.patch" in sh
    assert "grep -q 'rd vms-25f'" in sh
