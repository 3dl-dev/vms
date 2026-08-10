#!/usr/bin/env python3
"""
test_scs_credit_live_mutants.py -- vms-aa1: THE MUTATION BATTERY for the live
flow-control account.

WHY IT EXISTS. vms-76e and vms-1d2 shipped a fully unit-tested pp. 2-43..2-45
credit account that NOTHING CALLED, and every one of those tests was green the
whole time. A green run said nothing about whether the daemon used the account,
because the tests called the account themselves. vms-aa1 moved the assertions
onto the production path (tests/vmsscs/test_scsd_wire.c, the four
test_credit_* cases), and this battery is what makes "those assertions would
notice" a MEASUREMENT instead of a claim: each mutant below breaks one of the
three live behaviours in src/vmsscs/scsd.c, and the C test must go RED.

HOW IT WORKS. Copies src/vmsscs/*.c, src/vmsscs/include/, src/libvms/include/
and tests/vmsscs/test_scsd_wire.c into a scratch tree, compiles and runs the
UNMUTATED copy first (the control -- a battery whose control is already red
scores meaningless kills), then applies each mutant on its own to the scratch
copy of scsd.c, recompiles, and requires a non-zero exit. The file is restored
and the control re-verified after every mutant. A mutant whose `old` text is
absent, or that leaves the file unchanged, is reported as a FAILURE and is never
scored as a kill. NOTHING under src/, docs/ or tests/ is written.

The mutant count and the kill count are PRINTED by this script and deliberately
NOT restated in any comment or CMake entry: a second copy of a figure is how the
vms-6b3 gate got itself rejected.

THE COMPILER comes from $OVMX_CC, then $CC, then `cc`. CMake passes
CMAKE_C_COMPILER through so this runs under whatever toolchain configured the
tree (the musl-gcc static build included).
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC_DIR = os.path.join(ROOT, "src", "vmsscs")
TEST_C = os.path.join(HERE, "test_scsd_wire.c")
CC = os.environ.get("OVMX_CC") or os.environ.get("CC") or "cc"
# vms-d34: sysgen_params.h's readers (pulled in via scsd.c) now call vmsfs
# functions for real, so this scratch compile needs the already-built
# vmsfs/vmslnm libraries -- CMake passes their paths through (see
# tests/vmsscs/CMakeLists.txt). Empty when run outside CMake/ctest; the
# build below then fails at link time with a clear undefined-reference
# error rather than a silent mislink.
VMSFS_LIB = os.environ.get("OVMX_VMSFS_LIB")
VMSLNM_LIB = os.environ.get("OVMX_VMSLNM_LIB")

# (name, old, new). `old` must appear EXACTLY ONCE in the scratch scsd.c.
MUTANTS = [
    # ---- clause 1: the send path stamps the Pending Receive Credit ---------
    ("SEND-no-stamp-at-choke-point",
     "    const uint8_t *out = scsd_credit_stamp_outbound(frame, len, scratch);",
     "    const uint8_t *out = frame; (void)scratch;"),
    ("SEND-stamps-a-constant-instead-of-the-account",
     "    if (scs_credit_stamp_header(scratch + 14, h.total_sca_len, (unsigned)credit) != 0) {",
     "    if (scs_credit_stamp_header(scratch + 14, h.total_sca_len, 1u) != 0) {"),
    ("SEND-no-debit-of-send-credit",
     "    int credit = scs_credit_on_send(cdt);",
     "    int credit = (int)cdt->pending_receive_credit;"),
    ("SEND-stamps-every-mtype-not-just-application-messages",
     "    if (h.kind != SCS_RX_APP_MESSAGE) {\n        return frame; /* MTYPE != 10 -- the credit field is not a piggyback */\n    }",
     "    if (0) {\n        return frame;\n    }"),
    # ---- clause 2: the receive path banks the inbound credit field ---------
    ("RECV-no-bank-of-the-application-message-credit",
     "                    (void)scs_credit_on_recv(rcdt, h.credit);",
     "                    (void)scs_credit_on_recv(rcdt, 0u);"),
    ("RECV-no-grant-from-the-ACCEPT_REQ-extension",
     "                    (void)scs_credit_grant_from_peer(rcdt, h.credit);",
     "                    (void)0;"),
    ("RECV-no-buffer-release-so-nothing-is-ever-pending",
     "                        (void)scs_credit_release_buffer(ccdt);",
     "                        (void)0;"),
    ("RECV-banks-frames-the-p2-35-source-check-refuses",
     "                scs_cdl_resolve(&scsd_cdl, h.dest_conid, h.src_conid, &rcdt) == SCS_DELIVER_OK) {",
     "                ((rcdt = scs_cdl_lookup(&scsd_cdl, h.dest_conid)) != NULL)) {"),
    # ---- clause 3: the kill switch actually gates the change --------------
    ("SWITCH-send-half-ignores-OVMX_NO_CREDIT_ACCOUNTING",
     "    if (!scs_credit_enabled() || !scsd_cdl_ready || len <= 14) {",
     "    if (!scsd_cdl_ready || len <= 14) {"),
    # ---- the grounded offset is load-bearing ------------------------------
    ("WIRE-stamps-the-wrong-SCA-offset",
     "    memcpy(scratch, frame, len);\n    if (scs_credit_stamp_header(scratch + 14, h.total_sca_len,",
     "    memcpy(scratch, frame, len);\n    scratch[14 + 46] = 0x5a;\n    if (scs_credit_stamp_header(scratch + 14, h.total_sca_len,"),
]


def fail(msg):
    print("FAIL: " + msg)
    return 1


def build_and_run(scratch, tag):
    """Compile the scratch tree's test and run it. Returns the exit code, or a
    negative number if the COMPILE failed (which is never a kill: a mutant that
    does not build proves nothing about the assertions)."""
    binpath = os.path.join(scratch, "t_" + tag)
    sources = [os.path.join(scratch, "test_scsd_wire.c")]
    sources += sorted(
        os.path.join(scratch, "vmsscs", f)
        for f in os.listdir(os.path.join(scratch, "vmsscs"))
        if f.endswith(".c") and f != "scsd.c"
    )
    cmd = [CC, "-std=gnu11", "-O0",
           "-I", os.path.join(scratch, "vmsscs", "include"),
           "-I", os.path.join(scratch, "libvms_include"),
           "-I", os.path.join(scratch, "vmsfs_include"),
           "-o", binpath] + sources
    extra_libs = [lib for lib in (VMSFS_LIB, VMSLNM_LIB) if lib]
    cmd += extra_libs
    for lib in extra_libs:
        # Full-path shared-object link inputs don't carry their own rpath;
        # without this the binary links but fails at exec with "cannot open
        # shared object file".
        cmd += ["-Wl,-rpath," + os.path.dirname(lib)]
    cp = subprocess.run(cmd, capture_output=True, text=True)
    if cp.returncode != 0:
        sys.stdout.write(cp.stderr[-4000:])
        return -1
    run = subprocess.run([binpath], capture_output=True, text=True)
    return run.returncode


def main():
    failures = 0
    scratch = tempfile.mkdtemp(prefix="ovmx-credit-mutants-")
    try:
        shutil.copytree(SRC_DIR, os.path.join(scratch, "vmsscs"))
        shutil.copytree(os.path.join(ROOT, "src", "libvms", "include"),
                        os.path.join(scratch, "libvms_include"))
        # vms-d34: sysgen_params.h (in libvms_include, pulled in by scsd.c)
        # now calls through to vmsfs (SYS$SYSTEM:OVMXVMSSYS.PAR version
        # resolution), so this standalone compile needs vmsfs's headers too.
        shutil.copytree(os.path.join(ROOT, "src", "vmsfs", "include"),
                        os.path.join(scratch, "vmsfs_include"))
        # The test #includes "../../src/vmsscs/scsd.c"; keep that path valid
        # inside the scratch tree by placing the test two levels down.
        test_dir = os.path.join(scratch, "a", "b")
        os.makedirs(test_dir)
        os.makedirs(os.path.join(scratch, "src"), exist_ok=True)
        os.symlink(os.path.join(scratch, "vmsscs"),
                   os.path.join(scratch, "src", "vmsscs"))
        shutil.copy(TEST_C, os.path.join(test_dir, "test_scsd_wire.c"))
        # Flatten: the copy at the scratch root is what build_and_run compiles,
        # and its relative include has to reach scratch/src/vmsscs/scsd.c.
        with open(TEST_C, encoding="utf-8") as fh:
            body = fh.read()
        body = body.replace('#include "../../src/vmsscs/scsd.c"',
                            '#include "src/vmsscs/scsd.c"')
        with open(os.path.join(scratch, "test_scsd_wire.c"), "w",
                  encoding="utf-8") as fh:
            fh.write(body)

        daemon = os.path.join(scratch, "vmsscs", "scsd.c")
        with open(daemon, encoding="utf-8") as fh:
            pristine = fh.read()

        # THE CONTROL.
        rc = build_and_run(scratch, "control")
        if rc != 0:
            return fail("the UNMUTATED control does not build+pass "
                        "(exit %d). Every kill below would be meaningless." % rc)
        print("control: unmutated tree builds and passes")

        killed = 0
        for name, old, new in MUTANTS:
            count = pristine.count(old)
            if count != 1:
                failures += 1
                print("FAIL %s: its `old` text appears %d time(s) in scsd.c, "
                      "expected exactly 1 -- the mutant does not apply and is "
                      "NOT scored as a kill" % (name, count))
                continue
            mutated = pristine.replace(old, new, 1)
            if mutated == pristine:
                failures += 1
                print("FAIL %s: applying it changed nothing" % name)
                continue
            with open(daemon, "w", encoding="utf-8") as fh:
                fh.write(mutated)
            rc = build_and_run(scratch, "mut")
            with open(daemon, "w", encoding="utf-8") as fh:
                fh.write(pristine)
            if rc < 0:
                failures += 1
                print("FAIL %s: the mutated tree does NOT COMPILE, so it proves "
                      "nothing about the assertions" % name)
                continue
            if rc == 0:
                failures += 1
                print("FAIL %s: SURVIVED -- test_scsd_wire is still green with "
                      "this behaviour broken" % name)
                continue
            killed += 1
            print("kill  %s" % name)

        # The control again, so a battery that corrupted its own scratch tree
        # cannot report a clean sweep.
        rc = build_and_run(scratch, "control2")
        if rc != 0:
            failures += 1
            print("FAIL: the control is no longer green after the battery "
                  "(exit %d) -- scsd.c was not restored" % rc)

        print("test_scs_credit_live_mutants: %d mutant(s), %d killed, "
              "%d failure(s)" % (len(MUTANTS), killed, failures))
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
