#!/usr/bin/env python3
"""capture_manifest.py -- vms-beb: the DECLARED inventory of every lab capture
a measurement tool may read, and which lab produced it.

WHY THIS EXISTS. On 2026-08-05 six lab-2 `vaxlab-4` captures produced by
vms-578 were deposited into `/data/training/vax/cluster/captures/`, which
every lab-1-grounded tool GLOBS and treats as the lab-1 grounding library.
lab-2 replicas reuse lab-1's SCSSYSTEMIDs and node MACs by design
(tests/lab/README.md), and lab-2's simh binary (aarch64) and message-type
vocabulary (a SUBSET of lab-1's) differ from lab-1's, so the deposit silently
moved the population every lab-1 figure is derived from: 23/34 checks in
scs_disc_measure, 13/27 in scs_reason_measure, 18/67 in scs_connect_data_measure
and 11/30 in scs_credit_measure went red against their own EXPECTED tables
(vms-096). The fix at the time was a per-tool `lab1_only()` that rejected any
filename containing the literal marker `-lab2-`. That is a BLOCKLIST: it only
catches a foreign file that happens to be named the way the six offenders were.
A capture that is mislabeled, renamed, or simply never marked would pass it
silently -- the exact failure mode this item exists to close.

This module is the ALLOWLIST replacement. It is a MANIFEST -- every capture any
tool here may read is named below, with the lab that produced it, DECLARED
rather than discovered. A tool that globs a directory now checks every hit
against this table and REFUSES (raises SystemExit) rather than silently
filtering when:

  * a file is in the directory but NOT in the manifest at all (an unknown
    capture -- add it here, declaring its lab, before any tool may read it), or
  * a file IS in the manifest, but under a DIFFERENT lab than the tool asked
    for (the vms-096 failure itself, generalised: this catches it regardless
    of what the file happens to be named).

Both directions are errors, not silent inclusions or silent drops. This is the
lab fence the item's done condition asks for: point a lab-1 census at a lab-2
capture and it REDS (see tests/vmsscs/test_capture_manifest.py).

Kept STDLIB-ONLY and side-effect-free at import time (no filesystem I/O), so
every tool that uses it can import it lazily from its own directory without
adding a network or filesystem dependency to the import itself, and the
figures-gate mutation harnesses that exec these tools' source can copy this
file alongside them exactly as they already do for dissect_sca.py.

Do NOT hand-edit this file to "fix" a red measurement tool by moving an entry
to a different lab, or by deleting an unknown-capture entry, unless the
capture actually was misfiled -- that is the exact move CLAUDE.md and the
item's constraint reject ("do NOT fix this by bumping EXPECTED... that bakes
the mixing in permanently"). If a NEW capture is produced, add it here with
its real lab, and record where it lives.
"""

import glob
import os

LAB1 = "lab-1"   # /data/training/vax/cluster/captures -- the hand-run SIMH lab
LAB2 = "lab-2"   # /data/training/vax/cluster/captures-lab2 -- k3s vaxlab-N pods

# ---------------------------------------------------------------------------
# THE MANIFEST. basename -> lab. One entry per capture file any tool here
# reads. Basenames are unique across both labs (checked by
# test_capture_manifest.py), so a basename-keyed table is unambiguous.
# ---------------------------------------------------------------------------
MANIFEST = {
    # --- lab-1: /data/training/vax/cluster/captures/*.pcap (47 files) ------
    "af2-established-rejoin-20260728.pcap": LAB1,
    "af2-firsttimer-established-20260728.pcap": LAB1,
    "c6d-vaxcluster-connect-20260728.pcap": LAB1,
    "cd0-baseline-current-20260728.pcap": LAB1,
    "cd0-bootB-zk1099-join-20260728.pcap": LAB1,
    "cd0-bootC-zk1099-votes2-20260728.pcap": LAB1,
    "cd0-restore-verify-20260728.pcap": LAB1,
    "ci3-addmember-20260728.pcap": LAB1,
    "ci3-join-membership-live2node-20260728.pcap": LAB1,
    "clean-vax1-test-20260728.pcap": LAB1,
    "control-member-meets-late-node-20260730.pcap": LAB1,
    "formation-01.pcap": LAB1,
    "formation-ci1-joinwindow.pcap": LAB1,
    "formation-ci1.pcap": LAB1,
    "ovmx-21e-sda.pcap": LAB1,
    "ovmx-21e-start.pcap": LAB1,
    "ovmx-5fe-channel-formed-20260728.pcap": LAB1,
    "ovmx-760-MEMBER-achieved-20260730.pcap": LAB1,
    "ovmx-760-barrier1-stall-20260730.pcap": LAB1,
    "ovmx-760-barrier5-dlmrefused-20260730.pcap": LAB1,
    "ovmx-760-dlm-refused-20260730.pcap": LAB1,
    "ovmx-760-lockmgrerr-20260730.pcap": LAB1,
    "ovmx-760-persist-10min-20260730.pcap": LAB1,
    "ovmx-760-relay-crash-20260730.pcap": LAB1,
    "ovmx-e4b-class03-removal-survived-20260731.pcap": LAB1,
    "ovmx-e81-by10-vc-bound-vax3-stalls-20260731.pcap": LAB1,
    "ovmx-e81-by11-reciprocated-still-no-join-20260731.pcap": LAB1,
    "ovmx-e81-bystander-ADDITION-SUCCESS-20260731.pcap": LAB1,
    "ovmx-e81-bystander-deadlock-20260730.pcap": LAB1,
    "ovmx-e81-bystander-fail-20260730.pcap": LAB1,
    "ovmx-e81-membergreet-nofire-20260730.pcap": LAB1,
    "ovmx-e81-newcomer-ignores-us-20260731.pcap": LAB1,
    "ovmx-e81-newcomer-refuses-60retx-20260731.pcap": LAB1,
    "satellite-niscs-boot-solicit.pcap": LAB1,
    "satellite-solicit-slice.pcap": LAB1,
    "scs-dlm-lockconflict.pcap": LAB1,
    "scs-idle-baseline.pcap": LAB1,
    "scs-node-leave.pcap": LAB1,
    "twonode-sameroot.pcap": LAB1,
    "vax1-hellos-clean.pcap": LAB1,
    "vax3-2to3-established-join-20260730.pcap": LAB1,
    "vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap": LAB1,
    "vms-9f3-admission-run1.pcap": LAB1,
    "vms-9f3-admission-run2.pcap": LAB1,
    "vms-9f3-admission-run3.pcap": LAB1,
    "vms-9f3-admission-run4-fresh.pcap": LAB1,
    "vms246-scsdir-0x4b-reached-20260728.pcap": LAB1,
    # nested under captures/ci1/ -- scs_connect_data_measure.py globs
    # `**/*.pcap` recursively and picks this one up too (pcaps_scanned=48).
    "vax1-verify.pcap": LAB1,

    # --- lab-2: /data/training/vax/cluster/captures-lab2/*.pcap (7 files) --
    # vaxlab-4, the vms-70e2/vms-578/vms-096 brackets. Read ONLY by
    # tools/cluster/scs_join_capability_measure.py.
    "vms096-V96-lab2-vaxlab4-20260805.pcap": LAB2,
    "vms578-B1-lab2-vaxlab4-20260805.pcap": LAB2,
    "vms578-B2-lab2-vaxlab4-20260805.pcap": LAB2,
    "vms578-B3-lab2-vaxlab4-20260805.pcap": LAB2,
    "vms70e2-A0-lab2-vaxlab4-20260805.pcap": LAB2,
    "vms70e2-A1-lab2-vaxlab4-20260805.pcap": LAB2,
    "vms70e2-A3-lab2-vaxlab4-20260805.pcap": LAB2,
}


def _offenders(basenames, lab, manifest):
    """Split `basenames` into (unknown, wrong_lab) against `manifest`/`lab`.

    unknown: basenames the manifest has never heard of.
    wrong_lab: [(basename, declared_lab), ...] for basenames the manifest
    declares under a DIFFERENT lab than requested.
    """
    unknown, wrong_lab = [], []
    for b in sorted(set(basenames)):
        declared = manifest.get(b)
        if declared is None:
            unknown.append(b)
        elif declared != lab:
            wrong_lab.append((b, declared))
    return unknown, wrong_lab


def _refuse(where, lab, unknown, wrong_lab):
    lines = ["capture manifest disagreement in %s (asked for %s):" % (where, lab)]
    if unknown:
        lines.append(
            "  %d capture(s) not in the manifest -- add them to "
            "tools/cluster/capture_manifest.py MANIFEST, declaring which lab "
            "produced them, before any tool may read this directory:" % len(unknown))
        lines.extend("    %s" % u for u in unknown)
    if wrong_lab:
        lines.append(
            "  %d capture(s) belong to a DIFFERENT lab than requested -- do "
            "NOT raise EXPECTED to absorb them, move the file (or fix the "
            "manifest if it was the one that was wrong):" % len(wrong_lab))
        lines.extend("    %s (manifest says %s)" % (b, d) for b, d in wrong_lab)
    raise SystemExit("\n".join(lines))


def check_paths(paths, lab, manifest=MANIFEST):
    """Return `paths` unchanged, or refuse (SystemExit) naming every capture
    in them that is unknown to the manifest or declared for a different lab.

    This is the direct manifest-checked replacement for the old
    filename-marker `lab1_only()`: same "list of paths in, same list out, or
    die naming the offenders" contract, but checked against a declared
    allowlist instead of a `-lab2-` substring blocklist.
    """
    unknown, wrong_lab = _offenders((os.path.basename(p) for p in paths),
                                    lab, manifest)
    if unknown or wrong_lab:
        _refuse("path list", lab, unknown, wrong_lab)
    return paths


def scan(capdir, lab, pattern="*.pcap", recursive=False, manifest=MANIFEST):
    """Glob `capdir` for `pattern` and return the hits, refusing (SystemExit)
    if any hit is unknown to the manifest or belongs to a different lab.

    Unlike `check_paths`, this one does the globbing itself, so it is the
    direct replacement for `lab1_only(sorted(glob.glob(...)))` call sites.
    """
    if recursive:
        paths = sorted(glob.glob(os.path.join(capdir, "**", pattern), recursive=True))
    else:
        paths = sorted(glob.glob(os.path.join(capdir, pattern)))
    unknown, wrong_lab = _offenders((os.path.basename(p) for p in paths),
                                    lab, manifest)
    if unknown or wrong_lab:
        _refuse(capdir, lab, unknown, wrong_lab)
    return paths


def check_named(capdir, names, lab, manifest=MANIFEST, pattern="*.pcap"):
    """For tools that read a fixed, named set of captures rather than
    globbing: verify every name in `names` is declared for `lab`, AND audit
    `capdir` itself for any OTHER capture that disagrees with the manifest
    (an unrelated foreign file sitting next to the named ones must not go
    unnoticed either). Returns `names` unchanged, or refuses (SystemExit).
    """
    unknown, wrong_lab = _offenders(names, lab, manifest)
    if unknown or wrong_lab:
        _refuse("named list for %s" % capdir, lab, unknown, wrong_lab)
    if os.path.isdir(capdir):
        scan(capdir, lab, pattern=pattern, manifest=manifest)
    return names
