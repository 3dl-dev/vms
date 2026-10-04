#!/usr/bin/env python3
"""test_substrate_quiet.py - host unit test for drive_boot_vax's console-quiet
check (rd vms-553).

The OpenVMX/VAX console shows the VMS personality, never the NetBSD substrate's
boot. That is done at the source (the OVMX_QUIET kernel + quiet /boot built by
tools/cross-vax/build-vax-modular-kernel.sh from netbsd-ovmx-quiet.patch) and
asserted by the SIMH drivers on the RAW console via substrate_lines(). This
pins what that check catches and what it lets through, with no SIMH:

  1. TEETH: a real pre-vms-553 Node B console (V0.7-4, captured off the live
     demo) is full of substrate output -- secondary bootstrap, copyright,
     memory sizing, boot device, module DEBUG -- and every kind is caught.
  2. The executive's operator lines as the patched kernel writes them to the
     console (no timestamp, TOCONSOP) and the VMS boot are NOT flagged.
  3. The SAME operator line carrying a kernel timestamp IS flagged: a stamped
     line on the console means kernel printf leaked past the quiet gate.

Run: pytest tests/lab-vax/test_substrate_quiet.py -v
"""

import os
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

# Stub drive_boot_vax.py's two non-stdlib imports (as test_build_source_iso.py does).
if "anita" not in sys.modules:
    sys.modules["anita"] = types.ModuleType("anita")
_stub = tempfile.mkdtemp(prefix="ovmx-test-netbsd-stub-")
with open(os.path.join(_stub, "netbsd_console.py"), "w") as _fp:
    _fp.write("# stub for test_substrate_quiet.py (rd vms-553)\n")
os.environ.setdefault("OVMX_NETBSD_DIR", _stub)

import drive_boot_vax as dbv  # noqa: E402

V074_NODEB = os.path.join(
    REPO, "tests/lab/captures/vms-deploy-v074-20260930/clean-pass1/"
    "01-page-order/OVMXB.console.log")

# What the console of a quiet Node B is expected to look like.
QUIET_CONSOLE = """\
KA655-B V5.3, VMB 2.7
Performing normal system tests.
Tests completed.
>>>B DUA0
(BOOT/R5:0 DUA0)

  2..
-DUA0
  1..0..

%OVMX-I-EXEC, VMS executive attached on /dev/vms

    OpenVMX V0.7-5 - OpenVMS-compatible
     1-OCT-2026 13:03:27.75

%OVMX-I-SYSDISK, mounting system disk DUA0:
%OVMX-I-MOUNTED, system disk DUA0: mounted
%OVMX-I-SCSNODE, node name OVMXB set from SYS$SYSTEM:OVMXVMSSYS.PAR
%PEA0, cluster HELLO multicast group 257 (CLUSTER_AUTHORIZE)
%CNXMAN, waiting to form or join an OpenVMS Cluster
%STDRV-I-STARTUP, OpenVMX startup begun at  1-OCT-2026 13:04:36.98
%CNXMAN, this node is now a VAXcluster member
Username:"""


def test_real_pre_quiet_console_is_caught():
    with open(V074_NODEB, encoding="utf-8", errors="replace") as fp:
        hits = dbv.substrate_lines(fp.read())
    joined = "\n".join(hits)
    for want in (">> NetBSD/vax boot", "Copyright (c)", "The NetBSD Foundation",
                 "total memory", "avail memory", "boot device",
                 "DEBUG: module", "vms: cluster_seam", "SYSKRNL (NetBSD kernel)"):
        assert want in joined, "substrate line %r was not caught" % want


def test_quiet_vms_console_passes():
    assert dbv.substrate_lines(QUIET_CONSOLE) == []


def test_stamped_operator_line_is_a_leak():
    leaked = "[  92.3700030] %CNXMAN, this node is now a VAXcluster member"
    assert dbv.substrate_lines(QUIET_CONSOLE + "\n" + leaked) == [leaked]


def test_crlf_console_text():
    assert dbv.substrate_lines("ok\r\n[   1.0000000] mainbus0 (root)\r\n") == [
        "[   1.0000000] mainbus0 (root)"]
