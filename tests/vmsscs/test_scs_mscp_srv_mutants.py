#!/usr/bin/env python3
"""
test_scs_mscp_srv_mutants.py -- vms-291: THE MUTATION BATTERY for the MSCP disk
server responder.

WHY IT EXISTS. test_scs_mscp_srv.c is a large green test, and a large green test
is a claim until something has been shown to make it red. Two of its assertions
are load-bearing in a way the others are not:

  (a) THE INV-6 BOUNDARY. Block data transfer is not implemented (rd vms-941).
      A READ with no transfer hook installed must answer Controller Error with a
      zero byte count -- NOT a Success for data that never moved. A silent fake
      success there is the exact bug class CLAUDE.md Rule 9 / INV-6 exists to
      kill, and it is the kind of regression that looks like a green build.
  (b) THE sec 3.4 CONTROLLER-ONLINE GATE. Every command other than SET
      CONTROLLER CHARACTERISTICS is refused until a class driver has completed
      one. Remove the gate and a stray frame on a half-open connection can mount
      a volume; nothing else in the tree would notice.

It also holds the THREE END-MESSAGE LENGTHS the documentation got wrong and only
the vms-291 serving capture corrected -- GUS 52 (Table A-7 says 48), WRITE 36
(not READ's 32), and the observed GUS tail word. Those three were WRONG AND
GREEN before the capture, because a test that only checks self-consistency
against the constant it is testing passes at any value. The header mutants below
are what makes those assertions pin a MEASUREMENT rather than a definition.

HOW IT WORKS. Copies src/vmsscs/{scs_mscp_srv,scs_mscp,scs_env,scs_member}.c, the include
directory and tests/vmsscs/test_scs_mscp_srv.c into a scratch tree, compiles and
runs the UNMUTATED copy first (the control -- a battery whose control is already
red scores meaningless kills), then applies each mutant on its own to the scratch
copy of the named file, recompiles, and requires a non-zero exit. The file is
restored after every mutant and the control is re-verified at the end. A mutant
whose `old` text is not present EXACTLY ONCE, or that leaves the file unchanged,
or that fails to COMPILE, is reported as a FAILURE and is never scored as a kill.
NOTHING under src/, docs/ or tests/ is written.

The mutant count and the kill count are PRINTED by this script and deliberately
NOT restated in any comment or CMake entry: a second copy of a figure is how the
vms-6b3 gate got itself rejected.

THE COMPILER comes from $OVMX_CC, then $CC, then `cc`. CMake passes
CMAKE_C_COMPILER through so this runs under whatever toolchain configured the
tree.

Clean-room (CLAUDE.md Rule 8): this file mutates OVMX's own source. It contains
no VMS-derived material beyond the field names already public in AA-L619A-TK.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC_DIR = os.path.join(ROOT, "src", "vmsscs")
TEST_C = os.path.join(HERE, "test_scs_mscp_srv.c")
CC = os.environ.get("OVMX_CC") or os.environ.get("CC") or "cc"

# The two files a regression in this module would actually land in. Keyed by the
# short name each mutant names below; the value is the path RELATIVE to the
# scratch copy of src/vmsscs.
TARGETS = {
    "srv.c": "scs_mscp_srv.c",
    "srv.h": os.path.join("include", "scs_mscp_srv.h"),
}

# (name, target, old, new). `old` must appear EXACTLY ONCE in the target file.
MUTANTS = [
    # ---- (a) THE INV-6 BOUNDARY: a READ that cannot move data must not lie ---
    ("INV6-READ-fakes-success-when-no-transfer-hook-exists", "srv.c",
     """        return build_transfer_end(
            end, end_len, cmd->cmd_ref, u->unit, SCS_MSCP_OP_READ,
            SCS_MSCP_STATUS(SCS_MSCP_ST_CTLR_ERR,
                            SCS_MSCP_SUB_CNT_INCONSISTENT),
            0u);""",
     """        return build_transfer_end(
            end, end_len, cmd->cmd_ref, u->unit, SCS_MSCP_OP_READ,
            SCS_MSCP_STATUS(SCS_MSCP_ST_SUCCESS, SCS_MSCP_SUB_NORMAL),
            byte_count);"""),
    ("INV6-READ-refusal-reports-bytes-that-never-crossed", "srv.c",
     """            SCS_MSCP_STATUS(SCS_MSCP_ST_CTLR_ERR,
                            SCS_MSCP_SUB_CNT_INCONSISTENT),
            0u);""",
     """            SCS_MSCP_STATUS(SCS_MSCP_ST_CTLR_ERR,
                            SCS_MSCP_SUB_CNT_INCONSISTENT),
            byte_count);"""),
    ("INV6-READ-refusal-is-not-counted", "srv.c",
     "        srv->xfer_refusals++;",
     "        (void)0;"),
    ("INV6-a-FAILED-block-transfer-is-reported-as-Success", "srv.c",
     "                SCS_MSCP_STATUS(SCS_MSCP_ST_HOST_BUF_ERR, 0u), moved);",
     "                SCS_MSCP_STATUS(SCS_MSCP_ST_SUCCESS, 0u), moved);"),

    # ---- (b) THE sec 3.4 CONTROLLER-ONLINE GATE ----------------------------
    ("GATE-Controller-Online-check-deleted", "srv.c",
     "        if (host == NULL || !host->ctlr_online) {",
     "        if (host == NULL) {"),
    ("GATE-a-REFUSED-SCC-still-takes-the-driver-Controller-Online", "srv.c",
     "        if (version != 0) {",
     "        if (version == 0xdeadu) {"),

    # ---- the three lengths the BOOK got wrong and the CAPTURE corrected -----
    ("WIRE-GUS-end-back-to-Table-A-7s-48-bytes", "srv.h",
     "#define SCS_MSCP_GUS_END_LEN 52",
     "#define SCS_MSCP_GUS_END_LEN 48"),
    ("WIRE-WRITE-end-collapsed-back-into-READs-32", "srv.c",
     """    size_t need = (base_opcode == SCS_MSCP_OP_WRITE) ? SCS_MSCP_WRITE_END_LEN
                                                     : SCS_MSCP_READ_END_LEN;""",
     "    size_t need = SCS_MSCP_READ_END_LEN;"),
    ("WIRE-observed-GUS-tail-not-written-on-a-valid-unit", "srv.c",
     """    put_le16(end + SCS_MSCP_E_GUS_TAIL, SCS_MSCP_E_GUS_TAIL_OBSERVED);
    return (long)SCS_MSCP_GUS_END_LEN;
}""",
     """    (void)0;
    return (long)SCS_MSCP_GUS_END_LEN;
}"""),
    ("WIRE-a-tail-word-is-INVENTED-for-the-stale-garbage-at-50-52", "srv.c",
     """    put_le16(end + SCS_MSCP_E_GUS_TAIL, SCS_MSCP_E_GUS_TAIL_OBSERVED);
    return (long)SCS_MSCP_GUS_END_LEN;
}""",
     """    put_le16(end + SCS_MSCP_E_GUS_TAIL, SCS_MSCP_E_GUS_TAIL_OBSERVED);
    put_le16(end + SCS_MSCP_E_GUS_TAIL + 2, SCS_MSCP_E_GUS_TAIL_OBSERVED);
    return (long)SCS_MSCP_GUS_END_LEN;
}"""),

    # ---- the measured SCC constant that is NOT an echo ----------------------
    ("SCC-echoes-the-hosts-flags-instead-of-the-measured-0xa004", "srv.c",
     "    put_le16(end + SCS_MSCP_E_CNTF, srv->ctlr_flags_reported);",
     "    put_le16(end + SCS_MSCP_E_CNTF, host ? host->ctlr_flags : 0u);"),

    # ---- the other refusals that must stay honest ---------------------------
    ("WRITE-is-falsely-acknowledged-instead-of-Write-Protected", "srv.c",
     """                              SCS_MSCP_STATUS(SCS_MSCP_ST_WRITE_PROT,
                                              SCS_MSCP_SUB_WP_SOFTWARE),""",
     """                              SCS_MSCP_STATUS(SCS_MSCP_ST_SUCCESS,
                                              SCS_MSCP_SUB_NORMAL),"""),
    ("READ-past-the-end-of-the-volume-is-serviced-not-refused", "srv.c",
     """    if ((uint64_t)lbn + (uint64_t)nblocks > (uint64_t)u->unit_size) {
        return build_transfer_end(
            end, end_len, cmd->cmd_ref, u->unit, SCS_MSCP_OP_READ,
            SCS_MSCP_STATUS(SCS_MSCP_ST_INVALID_CMD, SCS_MSCP_P_LBN * 256u), 0u);
    }""",
     "    (void)lbn;"),
    ("READ-on-a-unit-that-was-never-brought-Online-transfers-anyway", "srv.c",
     "    if (!u->online) {",
     "    if (0) {"),

    # ---- the GET UNIT STATUS walk a class driver enumerates with ------------
    ("GUS-MD.NXU-degrades-to-an-exact-match-so-the-walk-never-advances", "srv.c",
     "            ? scs_mscp_srv_next_unit(srv, cmd->unit)",
     "            ? scs_mscp_srv_find_unit(srv, cmd->unit)"),
    ("GUS-walk-terminator-reports-Success-instead-of-Unit-Offline", "srv.c",
     """                   SCS_MSCP_STATUS(SCS_MSCP_ST_OFFLINE,
                                   SCS_MSCP_SUB_OFL_UNKNOWN));
        put_le16(end + SCS_MSCP_E_GUS_TAIL, SCS_MSCP_E_GUS_TAIL_OBSERVED);""",
     """                   SCS_MSCP_STATUS(SCS_MSCP_ST_SUCCESS,
                                   SCS_MSCP_SUB_NORMAL));
        put_le16(end + SCS_MSCP_E_GUS_TAIL, SCS_MSCP_E_GUS_TAIL_OBSERVED);"""),
    ("GUS-a-served-but-unmounted-unit-claims-Unit-Online", "srv.c",
     """                          : SCS_MSCP_STATUS(SCS_MSCP_ST_AVAILABLE,
                                            SCS_MSCP_SUB_NORMAL);""",
     """                          : SCS_MSCP_STATUS(SCS_MSCP_ST_SUCCESS,
                                            SCS_MSCP_SUB_NORMAL);"""),

    # ---- ONLINE: the mount-verify step -------------------------------------
    ("ONLINE-Already-Online-is-reported-as-an-error", "srv.c",
     """    uint16_t status = u->online
                          ? SCS_MSCP_STATUS(SCS_MSCP_ST_SUCCESS,
                                            SCS_MSCP_SUB_ALREADY_ONLINE)""",
     """    uint16_t status = u->online
                          ? SCS_MSCP_STATUS(SCS_MSCP_ST_OFFLINE,
                                            SCS_MSCP_SUB_OFL_UNKNOWN)"""),
    ("ONLINE-ignores-the-hosts-unit-flag-word-it-is-supposed-to-echo", "srv.c",
     "        put_le16(end + SCS_MSCP_E_UNFL, (uint16_t)(host_flags | u->unit_flags));",
     "        put_le16(end + SCS_MSCP_E_UNFL, u->unit_flags); (void)host_flags;"),
    ("ONLINE-lets-a-host-clear-the-units-own-UF.WPS-by-asking", "srv.c",
     "        put_le16(end + SCS_MSCP_E_UNFL, (uint16_t)(host_flags | u->unit_flags));",
     "        put_le16(end + SCS_MSCP_E_UNFL, host_flags);"),

    # ---- the Invalid Command end message Table A-1 describes ----------------
    ("INVALID-CMD-end-message-carries-the-opcode-instead-of-just-OP.END",
     "srv.c",
     "    end_header(end, SCS_MSCP_HDR_LEN, cmd_ref, unit, 0u, 0u, status);",
     "    end_header(end, SCS_MSCP_HDR_LEN, cmd_ref, unit,\n"
     "               SCS_MSCP_OP_SET_CTLR_CHAR, 0u, status);"),
]


def fail(msg):
    print("FAIL: " + msg)
    return 1


def build_and_run(scratch, tag):
    """Compile the scratch tree's test and run it. Returns the exit code, or a
    negative number if the COMPILE failed (which is never a kill: a mutant that
    does not build proves nothing about the assertions)."""
    binpath = os.path.join(scratch, "t_" + tag)
    src = os.path.join(scratch, "vmsscs")
    sources = [
        os.path.join(scratch, "test_scs_mscp_srv.c"),
        os.path.join(src, "scs_mscp_srv.c"),
        os.path.join(src, "scs_mscp.c"),
        os.path.join(src, "scs_env.c"),
        os.path.join(src, "scs_member.c"),  # vms-020: scs_member_vms_time_now()
    ]
    cmd = [CC, "-std=gnu11", "-O0",
           "-I", os.path.join(src, "include"),
           "-o", binpath] + sources
    cp = subprocess.run(cmd, capture_output=True, text=True)
    if cp.returncode != 0:
        sys.stdout.write(cp.stderr[-4000:])
        return -1
    run = subprocess.run([binpath], capture_output=True, text=True)
    if run.returncode != 0:
        # Useful when a mutant is expected to kill and does not.
        sys.stdout.write(run.stdout[-2000:])
    return run.returncode


def main():
    failures = 0
    scratch = tempfile.mkdtemp(prefix="ovmx-mscp-srv-mutants-")
    try:
        shutil.copytree(SRC_DIR, os.path.join(scratch, "vmsscs"))
        shutil.copy(TEST_C, os.path.join(scratch, "test_scs_mscp_srv.c"))

        pristine = {}
        for key, rel in TARGETS.items():
            path = os.path.join(scratch, "vmsscs", rel)
            with open(path, encoding="utf-8") as fh:
                pristine[key] = fh.read()

        # THE CONTROL.
        rc = build_and_run(scratch, "control")
        if rc != 0:
            return fail("the UNMUTATED control does not build+pass (exit %d). "
                        "Every kill below would be meaningless." % rc)
        print("control: unmutated tree builds and passes")

        killed = 0
        for name, key, old, new in MUTANTS:
            if key not in TARGETS:
                failures += 1
                print("FAIL %s: unknown target %r" % (name, key))
                continue
            path = os.path.join(scratch, "vmsscs", TARGETS[key])
            base = pristine[key]
            count = base.count(old)
            if count != 1:
                failures += 1
                print("FAIL %s: its `old` text appears %d time(s) in %s, "
                      "expected exactly 1 -- the mutant does not apply and is "
                      "NOT scored as a kill" % (name, count, TARGETS[key]))
                continue
            mutated = base.replace(old, new, 1)
            if mutated == base:
                failures += 1
                print("FAIL %s: applying it changed nothing" % name)
                continue
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(mutated)
            rc = build_and_run(scratch, "mut")
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(base)
            if rc < 0:
                failures += 1
                print("FAIL %s: the mutated tree does NOT COMPILE, so it proves "
                      "nothing about the assertions" % name)
                continue
            if rc == 0:
                failures += 1
                print("FAIL %s: SURVIVED -- test_scs_mscp_srv is still green "
                      "with this behaviour broken" % name)
                continue
            killed += 1
            print("kill  %s" % name)

        # The control again, so a battery that corrupted its own scratch tree
        # cannot report a clean sweep.
        rc = build_and_run(scratch, "control2")
        if rc != 0:
            failures += 1
            print("FAIL: the control is no longer green after the battery "
                  "(exit %d) -- a target file was not restored" % rc)

        print("test_scs_mscp_srv_mutants: %d mutant(s), %d killed, "
              "%d failure(s)" % (len(MUTANTS), killed, failures))
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
