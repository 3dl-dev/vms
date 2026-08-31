#!/usr/bin/env python3
"""Roadmap reconcile + publish — rd (source of truth) -> roadmap doc + site data.

WHAT THIS IS
    A REPEATABLE, IDEMPOTENT checkpoint tool. rd is the single source of truth for
    release status (milestones are encoded as labels rel-0.5..rel-1.0; the 1.0-gate
    epics are named below). This script snapshots rd, computes per-milestone and
    per-gate-epic status DETERMINISTICALLY, and regenerates:

      1. the GENERATED block inside docs/release-roadmap-to-1.0.md  (in-repo, detailed,
         carries item IDs + the labeling-gap report)
      2. build/roadmap.json                                          (canonical machine
         export, with IDs — the internal artifact)
      3. <site>/data/roadmap.json                                    (curated + trademark
         -scrubbed, MILESTONE-LEVEL, no internal IDs — the public site view)

    The site's roadmap/ and status/ pages fetch data/roadmap.json (same pattern as
    /compat/ -> data/compat-surface.json). The page HTML is stable; only the DATA is
    regenerated each checkpoint — so re-running with unchanged rd is byte-identical.

IDEMPOTENCY
    - Every collection is sorted by a stable key (item id / milestone order).
    - There are NO wall-clock timestamps in the output. The only date is the --as-of
      stamp (a DATE, not a time), which defaults to today (UTC) and can be pinned so a
      CI check is reproducible within the day. Pass the SAME --as-of to get byte-
      identical output.
    - --check exits non-zero if regenerating WOULD change any tracked file (drift gate).

CONTINUATION-IDENTITY NOTE (see ~/.claude/CLAUDE.md)
    Milestone THEMES / workstream summaries / release notes below are EDITORIAL naming,
    not derived status — they are stable curation. Everything with the word "status",
    "progress", "done", "open", "blocked" is RE-DERIVED from the live rd snapshot on
    every run and is never frozen here.

USAGE
    python3 tools/roadmap/reconcile.py                 # regenerate (calls `rd list --all --json`)
    python3 tools/roadmap/reconcile.py --site-dir ../openvmx-site
    python3 tools/roadmap/reconcile.py --rd-json snap.json --as-of 2026-08-14 --check
    python3 tools/roadmap/reconcile.py --print-gaps    # just report rd-labeling gaps
    python3 tools/roadmap/reconcile.py --check-cascade --site-dir ../openvmx-site
                                                       # verify every public surface
                                                       # reflects the latest release tag
                                                       # (fails loudly on a demo/site that
                                                       # silently trails the shipped cut)
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import re
import subprocess
import sys
from collections import Counter

# --------------------------------------------------------------------------- #
#  EDITORIAL CONFIG (stable curation — NOT derived status)                     #
# --------------------------------------------------------------------------- #

# Ordered milestone ladder. `band` classifies unshipped milestones for the public
# status badge when no tag exists yet. `theme` is the public one-line.
MILESTONES = [
    ("0.3", "shipped",
     "A real system — the command language, file system, system services, and kernel "
     "executive stand on their own."),
    ("0.4", "shipped",
     "Installs and boots faithfully — the product installs to a target disk and reboots "
     "into a login."),
    ("0.5", "shipped",
     "The authenticity flip — RMS reads and writes genuine Files-11 ODS-2 over the "
     "executive ACP (the /vms passthrough retired on the runtime path), binary SYSUAF "
     "and Purdy login, and a userland that builds itself in-guest."),
    ("0.6", "shipped",
     "Cluster correctness — a real distributed lock manager over the SCS wire, "
     "RMS behind the DLM, and cluster membership resident in the executive."),
    ("0.7", "planned",
     "Cluster wire fidelity — the SCS/MSCP connection manager answers a real VAX "
     "byte-for-byte."),
    ("0.8", "planned",
     "Rejoin and satellite boot — a removed node rejoins under its own identity; "
     "diskless satellites boot from a served disk."),
    ("0.9", "planned",
     "Feature-complete — a voting member joins, serves genuine ODS-2 storage, holds "
     "locks, and evacuates a live node; TCP/IP, DECnet, and the self-hosting toolchain "
     "reach done. The last features land here."),
    ("1.0", "goal",
     "Hardened and proven — feature-frozen on 0.9: authenticity enforced by the "
     "executive, not by convention, and the whole system proven on real hardware and in "
     "extended cluster interop against a real VAX. The release you trust; fixes only."),
]
MILESTONE_ORDER = [m[0] for m in MILESTONES]

# The 1.0-gate epics ("workstreams" on the public site). Order is the presentation order.
#   id, public name, public one-line summary, milestone band it lands by
GATE_EPICS = [
    ("vms-6b8", "Executive substrate",
     "Shared system state owned by the kernel executive: logical names, processes, "
     "locks, event flags, and devices survive across processes.", "0.5"),
    ("vms-8ad", "Command-surface parity",
     "Real breadth and depth across the DCL command set and system utilities — no facades.",
     "continuous"),
    ("vms-678", "Self-hosting toolchain",
     "OVMX builds OVMX from within: the librarian and linker run as native images with no "
     "host tools in the build path. The in-guest tcc is a labelled bootstrap; the faithful "
     "destination is the existing OpenVMS GCC and MMK ports building on OVMX unchanged, over "
     "a genuine VMS-compatibility surface.", "0.5-0.9"),
    ("vms-098", "Cluster configuration",
     "Provision a node into a cluster the VMS way: SYSGEN parameters, AUTOGEN, "
     "CLUSTER_AUTHORIZE, and CLUSTER_CONFIG.", "0.5-0.9"),
    ("vms-67f", "TCP/IP networking",
     "A VMS-faithful IP networking layered product — its own installable kit on the base "
     "operating system: the network device, sockets, configuration and management, the "
     "resolver, the client tools, and a bundled SSH.", "0.5-0.9"),
    ("vms-30e", "DECnet Phase IV",
     "Clean-room DECnet: SET HOST and file transfer to and from a lab node.", "0.9"),
    ("vms-19e", "Kernel substrate",
     "SYSKRNL: the OVMX executive layered over the Linux and NetBSD kernels — not a "
     "kernel of its own. Built from pinned upstream source with the VMS modules "
     "in-tree, curated per architecture.", "0.5"),
    ("vms-8e8", "VAX as a first-class platform",
     "OVMX runs natively on VAX through a NetBSD system kernel: the executive, ODS-2 "
     "storage, and DCL boot on real VAX emulation, co-released across every architecture.",
     "0.5-0.9"),
]
GATE_IDS = [g[0] for g in GATE_EPICS]

# Editorial release notes, tag -> one line. Tags without an entry get a generic note.
RELEASE_NOTES = {
    "V0.6-2": "Two increments up the 1.0 networking and cluster-configuration long poles, no engine changes. DECnet Phase IV gets its first real engine code: a routing-layer endnode-HELLO codec that decodes every field of and byte-identically re-encodes the captured real-VAX oracle specimen (clean-room, derived only from the DNA Phase IV Routing spec plus the lab capture) — the foundation rung under the forthcoming NSP transport, adjacency state machine, and the engine go/no-go. And cluster identity can now be authored the VMS-canon way: CLUSTER_CONFIG_LAN.COM, with a CLUSTER_CONFIG.COM front end, drives SYSGEN SET + WRITE CURRENT to persist SCSNODE and SCSSYSTEMID into OVMXVMSSYS.PAR over the executive ACP, honestly declining the operations it does not yet implement, proven end-to-end against the booted DCL (authors an identity, rejects an over-long SCSNODE, reads it back from the store). It rides the SYS$INPUT-to-image and .PAR-write mechanisms that already shipped.",
    "V0.6-1": "Bug-fix patch: SPAWN now works in the booted runtime. In V0.6, DCL SPAWN failed with %DCL-F-CREPRC because a non-root interactive session's $CREPRC child tried to re-stamp a privileged identity the executive correctly refused; the child now inherits the creator's identity by continuation from its unforgeable parent (a new VMS_IOCTL_REGISTER_SUBPROCESS path), leaving the SS$_NOPRIV self-declaration guard intact. The VMS User Acceptance battery goes 62/66 → 66/66. Found by KVM runtime verification that CI's TCG-flake had masked.",
    "V0.6": "Cluster correctness, complete: OVMX is now a genuine VMScluster participant. The distributed lock manager runs the full H0–H11 ladder on the real executive over the SCS wire — cross-node $ENQ grant, contention and block-then-grant, BLKAST delivery, resource mastering and remastering, LVB replication, and distributed deadlock detection — and RMS file-share and record locking reach the real DLM arbitrator on real /dev/vms (INV-6, no flock fallback). Cluster membership now lives in the executive: SHOW CLUSTER and $GETSYI read the real member block and the userspace file-facade is fully excised, with rejoin and parameter adoption proven. Plus the oracle-driven UX-fidelity gate: a continuous, structure-tolerant golden-diff of DCL/SHOW output against byte-exact real-VMS captures, with the SHOW family proven fabrication-free and real structural gaps tracked as an honest, gated backlog. (Quorum, votes, and MSCP-served volumes remain post-0.6.)",
    "V0.5-11": "The VAX DCL/SHOW acceptance battery goes fully green (101 of 101) — every VAX user-visible surface is now faithful. Real per-process accounting binds to the kernel's maintained accessors (calcru CPU, rulwps live-aggregated page faults, vm_resident_count resident pages), the SYSUAF quota facility returns real SHOW PROCESS/QUOTAS values, F$PID preserves its %08X pid format across every DCL coercion path, and a device error-count writer records genuine block-I/O errors. Plus the Alpha decc$_malloc64 allocator unifies with mallocng (EVAX strong-over-weak), rail-proven. Accounting reads the value the kernel's own ps/kinfo path reads, never a raw struct field a map names (INV-6, no false-zeros).",
    "V0.5-10": "The UX-fidelity de-fabrication batch completes: every confirmed hollow or fabricated SHOW/F$ surface is now real executive data or an honest omission (INV-6) — SHOW SYSTEM with an oracle-captured golden, SHOW WORKING_SET's real working-set size, F$PID's real executive pids, SHOW ERROR's real per-device error counts, and F$GETQUI honoring the caller's queue. Plus a chunked producer-load cap and the Alpha LLP64 malloc-width fix, both rail-proven on the real /dev/vms executive.",
    "V0.5-9": "A broad batch: oracle-driven UX-fidelity de-fabrications held to live-VMS goldens, the distributed lock manager's H5 two-node SCS-wire milestone, the TCP/IP Services configuration plane (TCPIP$CONFIG, TCPIP$ logicals, SET/SHOW INTERFACE), and further GCC-port toolchain rungs up the do-it-like-VMS ladder.",
    "V0.5-8": "The distributed lock manager's cross-node $ENQ GRANT crown — a lock granted on one OVMX node is honored on another over real SCS — with Alpha co-release parity, the operator-reported boot newline fix, and the reframing of TCP/IP Services as a first-class layered product.",
    "V0.5-7": "The GCC-port-on-the-real-executive release. The genuine alpha-dec-vms OpenVMS GCC port runs as an OVMX-Alpha image on the live /dev/vms executive — a P1 milestone up the do-it-like-VMS ladder: activated via IMGACT over the mounted ODS-2 ACP, decc$main binds its producers (DECC$SHR/LIBOTS) over the ACP, and crt0->main returns the executive sentinel $STATUS=0x0035A019 (faithful C$_EXIT1(3), control-verified). The last three activation gaps close honestly against the real runtime — LIBOTS OTS$ register-preservation, the C-RTL auxv/R0 path, and the wired alpha-dec-vms musl syscall backend (callsys ABI, native Alpha syscall numbers, wruniq TP) — with the faithfulness lock catching four real gaps that qemu-user had hidden. Cluster: the OVMX<->OVMX member/initiator role is complete — two OVMX nodes form a VMScluster against each other (rung-0 solicit, rung-VC 0x41 START initiate, rung-ADD 0x5b joiner accept) — and a live cross-node $ENQ round-trip A->B->A over SCS works through the distributed lock manager (granting nothing, INV-6). Networking: a real OpenSSH sshd session rides entirely on the executive's own primitives over BGn: — bind/listen/accept over BGn:, a materialized [bgconn] fd for the session, getpeername/setsockopt answered from the executive socket, and byte-exact data through IO$_READVBLK/WRITEVBLK, with no AF_UNIX and no raw host socket. QA'd under KVM boot-to-login with the full SHOW battery VMS-faithful (42/42 acceptance); the frozen-verify red legs confirmed no-new-vs-baseline (TCG-flake, vms-898a).",
    "V0.5-6": "Image activation proven end-to-end, and the filesystem converges on a single executive path. The do-it-like-VMS image activation now runs end-to-end against a real /dev/vms — a VMS-standard activation context on the live executive, not a userspace stand-in — the Tier-1 flagship for the runtime. Filesystem convergence: the legacy ODS-2 VFS driver is atomically retired across Linux, NetBSD, and the shared core (~16k lines removed), leaving a single Files-11 ACP executive path. Toolchain hardening up the do-it-like-VMS ladder: the Alpha and VAX cross toolchains now pull source-hash-keyed prebuilt images from ghcr (killing the ftp.gnu.org build flake), the Alpha DECC$SHR vector is enumerated from the linker's own EVAX read view, and the multi-TU LINK.EXE self-host fixpoint is ported from BUILD.COM to MMK (additive, gen2==gen3 proven). VAX substrate: the exec_socket_* seam moves BGn: networking into kernel-core with Linux and NetBSD backends, and vms_stdio/vms_futex build on VAX to close the freestanding-facility gap. Cut through the all-architectures gate (x86_64, aarch64, VAX, and Alpha green-by-SHA on the tagged commit).",
    "V0.5-5": "VAX login end-to-end, executive faithfulness, and Alpha toolchain hardening — the first release cut through an all-architectures gate (x86_64, aarch64, VAX, and Alpha all proven green on the exact commit before tagging). The VAX installed single-disk authenticates SYSTEM/MANAGER to a DCL prompt, and RUN /DETACHED now names the console OPA0: instead of the raw substrate path. The executive-boundary audit tracer is wired operational at image activation, so a raw syscall an image issues becomes a visible finding rather than a silent bypass. Alpha toolchain: a DST-tolerant object reader, container-format-aware C-RTL architecture auto-detection, LINK.EXE hard-errors a strong-vs-strong duplicate definition on the EVAX path (exempting same-library members, VMS first-module-wins), the stdio streams export as data universals, and $CREPRC fails honestly when a UIC or privilege override cannot reach the executive row.",
    "V0.5-4": "VAX authentication reaches DCL. The VAX login chain lands end-to-end: the Purdy-S hash defeats a gcc-vax DImode miscompile by construction so it matches the real binary SYSUAF, JOB_CONTROL establishes SYSTEM identity at startup, and $CREPRC stamps executive identity with RUN /UIC//PRIVILEGES honored. Alpha becomes a co-release peer with a genuine C runtime: a wiring gate reds the cut on any broken Alpha build, the Alpha C-RTL shareables (DECC$SHR's 538 universals and LIBOTS$SHR's 11 OTS$ routines) are built from real musl and libgcc with zero undefined, the GCC port's crt0 links zero-deferred against them, and an FP divide-by-zero raises SS$_HPARITH through the condition handler into $STATUS. Toolchain and faithfulness hardening: a standing shell-portability lint gate, LINK.EXE hard-errors a strong-vs-strong multiple definition (%LINK-F-MULDEF), and the executive-boundary audit tracer (seccomp user-notification, observe-only) makes every raw syscall an image issues visible as a finding — the Phase-A instrument under the executive-boundary program.",
    "V0.5-3": "QA-remediation, acceptance-gate-proven. Fixes the basic-command breakage that shipped in V0.5-2 and installs a standing boot-and-run DCL/SHOW acceptance gate so it cannot recur — the gate boots the real image, logs in, runs the commands a user types, and asserts VMS-faithful output. SHOW USERS lists real interactive and spawned processes (was empty); WRITE F$GETSYI and other lexical functions evaluate rather than printing literal tokens; SHOW DEVICE shows mount state, volume label, and free blocks; SHOW QUOTA is de-fabricated to an honest %SYSTEM-F-NODISKQUOTA (no invented UIC/blocks, INV-6); SHOW DEVICES/SYMBOL wildcard/STATUS real $GETJPI accounting; bare DIRECTORY resolves the rooted login default (was %DIRECT-W-NOFILES); DIRECTORY header/columns and the SPAWN /PROCESS= qualifier. Also: os-release VERSION_ID SSOT guard, the roadmap Ledger reconcile, and the alpha-dec-vms cc1 entry-label decoration up the do-it-like-VMS ladder.",
    "V0.5-2": "Restores x86_64 boot-to-login (vms-656): a native-link build-flag drift had dropped the shipped RMS's ODS-2 ACP arm, so STARTUP.COM could not resolve SYS$STARTUP:VMS$PHASES.DAT over the executive ACP — genuine Files-11 ACP search-list resolution is restored (the POSIX fallback removed) with a drift-catching guard, and x86_64 boots to the Username: prompt again. Builds the OpenVMS GCC-port crt0 surface up the do-it-like-VMS ladder: IMGACT presents a genuine VMS image-activation context (Alpha standard call), decc$main produces argc/argv/envp, C$_EXIT1 is a C-RTL globalvalue, and LINK.EXE reads the alpha-dec-vms port's native EVAX object (cross-image SYMG import binding, dsc$descriptor_s canonical binding) with no ELF force-down. Also: vmssshd fail-honest on executive identity refusal (INV-6), the vms-040 executive-boundary audit, genuine $ALLOC/$DALLOC over a NetBSD executive device table, the vms-329 VAX-runtime ACP cutover work, RMS multiblock ACP read-ahead, and SPAWN visibility in SHOW USERS/SYSTEM.",
    "V0.5-1": "Hardens the Alpha authentic-login gate and lands C++ first-light. The ODS-2 executive ACP is proven on x86_64 and Alpha LP64 (which boot and run RMS over the executive ACP); on NetBSD/VAX the ACP codec is built and unit-proven but is NOT yet wired into the runtime — the VAX image set builds with OVMX_HAVE_ACP undefined and boots via the Files-11 VFS/POSIX path (converting the VAX runtime onto the ACP is tracked as vms-d5d/vms-049, V0.5-2+; the vms-d9c VAX-boot gate is green to PROVISION.EXE via that path). Alpha authentic binary-SYSUAF login carries a standing green-by-SHA CI gate, and C++ first-light — a real C++ program (constructors, std::string/iostream, throw/catch) runs to exit-0 as an OVMX image, proven across x86_64, Alpha LP64, and VAX ILP32.",
    "V0.5": "The authenticity flip: RMS reads and writes genuine Files-11 ODS-2 over the executive ACP (the /vms passthrough retired on the runtime path); binary $UAFDEF SYSUAF + Purdy login proven on x86_64 and Alpha LP64; and the shipped MMK.EXE self-hosts the userland in-guest (TCC->LIBRARIAN->LINK->activate, zero bash, byte-identical). The NetBSD/VAX substrate runs on the Files-11 VFS/POSIX path; wiring the VAX runtime onto the executive ACP is tracked as vms-d5d/vms-049 (V0.5-2+).",
    "V0.4-6": "Real OpenSSH key exchange over the executive network path, genuine ODS-2 read/write/INITIALIZE foundations, cluster rejoin proof, VAX co-release, and a sharded kernel-executive gate.",
    "V0.4-5": "Feature pack marching toward 0.5.",
    "V0.4-4": "Feature pack marching toward 0.5.",
    "V0.4-3": "Feature pack marching toward 0.5.",
    "V0.4-2": "Feature pack marching toward 0.5.",
    "V0.4-1": "Dense feature pack toward 0.5.",
    "V0.4": "Installs to a target disk and reboots into a login.",
    "V0.3-9": "Executive-backed DCL, cluster rejoin, OPCOM messages.",
    "0.3-4": "SET ACCOUNTING / SET VOLUME, clean cluster leave.",
    "0.3-3": "Conversational boot, DCL parser fidelity.",
    "0.3-2": "DCL fidelity, phases 0 and 1.",
    "0.3-1": "Rebrand, single QEMU runtime.",
    "0.3": "A real system: DCL, RMS, system services, and a kernel executive.",
    "0.2": "Install, log in, and run a real RMS indexed-file application.",
}

# How many recent releases the public site surfaces.
PUBLIC_RELEASE_LIMIT = 8

GEN_BEGIN = ("<!-- GENERATED:BEGIN roadmap-reconcile — do not edit by hand, "
             "run tools/roadmap/reconcile.py -->")
GEN_END = "<!-- GENERATED:END roadmap-reconcile -->"
# On first run (no markers yet) the block is inserted immediately before this anchor.
DOC_ANCHOR = "## 2. The 1.0 objective and its pillars"

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ROADMAP_DOC = os.path.join(REPO_ROOT, "docs", "release-roadmap-to-1.0.md")
BUILD_JSON = os.path.join(REPO_ROOT, "build", "roadmap.json")

# Public site origin, used to build the Atom feed's stable ids and links.
SITE_URL = "https://openvmx.3dl.dev"


# --------------------------------------------------------------------------- #
#  rd snapshot + rollups                                                        #
# --------------------------------------------------------------------------- #

TERMINAL = {"done", "cancelled", "failed"}


def load_snapshot(rd_json_path: str | None) -> list[dict]:
    if rd_json_path:
        with open(rd_json_path) as fh:
            return json.load(fh)
    out = subprocess.run(
        ["rd", "list", "--all", "--json"],
        capture_output=True, text=True, check=True,
    ).stdout
    return json.loads(out)


def index(items: list[dict]) -> dict[str, dict]:
    return {x["id"]: x for x in items}


def child_map(items: list[dict]) -> dict[str, list[str]]:
    kids: dict[str, list[str]] = {}
    for x in items:
        p = x.get("parent_id")
        if p:
            kids.setdefault(p, []).append(x["id"])
    return kids


def descendants(root: str, kids: dict[str, list[str]]) -> list[str]:
    """Transitive children of root (excludes root), deduped + sorted (idempotent)."""
    seen: set[str] = set()
    stack = list(kids.get(root, []))
    while stack:
        i = stack.pop()
        if i in seen:
            continue
        seen.add(i)
        stack += kids.get(i, [])
    return sorted(seen)


def rollup(ids: list[str], by: dict[str, dict]) -> dict:
    """Status rollup for a set of item ids. Deterministic."""
    sc = Counter(by[i]["status"] for i in ids if i in by)
    total = sum(sc.values())
    cancelled = sc.get("cancelled", 0) + sc.get("failed", 0)
    done = sc.get("done", 0)
    blocked = sc.get("blocked", 0) + sc.get("waiting", 0)
    denom = total - cancelled
    open_ = denom - done
    pct = round(100 * done / denom) if denom else 0
    return {
        "total": total, "done": done, "open": open_,
        "blocked": blocked, "cancelled": cancelled, "pct": pct,
        "by_status": dict(sorted(sc.items())),
    }


def milestone_items(label: str, items: list[dict]) -> list[str]:
    return sorted(x["id"] for x in items if label in (x.get("labels") or []))


def compute(items: list[dict]) -> dict:
    by = index(items)
    kids = child_map(items)

    # ---- milestones (explicit rel-* label signal) ----
    milestones = []
    for ver, band, theme in MILESTONES:
        label = f"rel-{ver}"
        ids = milestone_items(label, items)
        r = rollup(ids, by)
        # public status badge: a shipped band stays shipped; otherwise reflect signal
        if band == "shipped":
            status = "shipped"
        elif band == "goal":
            status = "goal"
        elif r["done"] and r["done"] < (r["total"] - r["cancelled"]):
            status = "in_progress"
        elif r["done"] and r["done"] == (r["total"] - r["cancelled"]) and r["total"]:
            status = "complete"
        else:
            status = "planned"
        milestones.append({
            "version": ver, "band": band, "theme": theme,
            "status": status, "label": label,
            "rollup": r,
            "items": [{"id": i, "status": by[i]["status"], "title": by[i]["title"]}
                      for i in ids],
        })

    # ---- gate-epic workstreams (parent_id descendant rollup) ----
    workstreams = []
    for gid, name, summary, band in GATE_EPICS:
        node = by.get(gid, {})
        desc = descendants(gid, kids)
        r = rollup(desc, by)
        self_status = node.get("status", "missing")
        # workstream status from its tree
        if not desc:
            wstatus = self_status
        elif r["done"] == 0:
            wstatus = "planned"
        elif r["done"] == (r["total"] - r["cancelled"]) and r["total"]:
            wstatus = "complete"
        else:
            wstatus = "in_progress"
        workstreams.append({
            "id": gid, "name": name, "summary": summary, "band": band,
            "self_status": self_status,
            "status": wstatus,
            "rollup": r,
            "labels": sorted(node.get("labels") or []),
            "descendant_count": len(desc),
        })

    # ---- releases (git tags) ----
    releases = git_releases()

    # ---- rd-labeling gaps ----
    gaps = labeling_gaps(items, by, kids)

    return {
        "milestones": milestones,
        "workstreams": workstreams,
        "releases": releases,
        "gaps": gaps,
    }


def git_releases() -> list[dict]:
    """Release-like tags, newest first, deterministic order."""
    try:
        raw = subprocess.run(
            ["git", "-C", REPO_ROOT, "tag"],
            capture_output=True, text=True, check=True,
        ).stdout.split()
    except Exception:
        return []

    def key(tag: str):
        t = tag.lstrip("Vv")
        # split "0.4-1" -> (0,4,1); "0.3" -> (0,3,-1); ignore non release-ish
        main, _, pt = t.partition("-")
        parts = main.split(".")
        try:
            nums = [int(p) for p in parts]
        except ValueError:
            return None
        pt_n = int(pt) if pt.isdigit() else -1
        while len(nums) < 3:
            nums.append(0)
        return (nums[0], nums[1], nums[2], pt_n)

    scored = [(key(t), t) for t in raw]
    scored = [(k, t) for k, t in scored if k is not None]
    # newest first; tie-break on the tag string for determinism
    scored.sort(key=lambda kt: (kt[0], kt[1]), reverse=True)
    out = []
    for _, tag in scored:
        out.append({"tag": tag, "note": RELEASE_NOTES.get(tag, "Point release."),
                    "date": _tag_date(tag)})
    return out


def _tag_date(tag: str) -> str | None:
    """Committer date of the tag's commit, strict ISO-8601 (RFC 3339).

    Deterministic (a tag's commit date is fixed), so surfacing it does not
    break the reconcile idempotency contract — it is not a wall-clock stamp.
    """
    try:
        return subprocess.run(
            ["git", "-C", REPO_ROOT, "log", "-1", "--format=%cI", tag],
            capture_output=True, text=True, check=True,
        ).stdout.strip() or None
    except Exception:
        return None


def labeling_gaps(items: list[dict], by: dict[str, dict],
                  kids: dict[str, list[str]]) -> dict:
    """Where rd labeling is incomplete, so the generated view can't lie silently."""
    gate_missing_rel = []
    for gid, name, _summary, band in GATE_EPICS:
        node = by.get(gid, {})
        labels = node.get("labels") or []
        rels = [l for l in labels if l.startswith("rel-")]
        if not rels:
            gate_missing_rel.append({
                "id": gid, "name": name, "expected_band": band,
                "level_epic": node.get("level") == "epic",
            })
    # rel items whose parent gate epic (if any) carries no rel label
    orphan_band = []
    gate_set = set(GATE_IDS)
    for x in items:
        rels = [l for l in (x.get("labels") or []) if l.startswith("rel-")]
        p = x.get("parent_id")
        if not rels and p in gate_set:
            gnode = by.get(p, {})
            if not any(l.startswith("rel-") for l in (gnode.get("labels") or [])):
                orphan_band.append({"id": x["id"], "parent": p})
    return {
        "gate_epics_missing_rel_label": gate_missing_rel,
        "unbanded_children_of_unlabeled_gates_count": len(orphan_band),
    }


# --------------------------------------------------------------------------- #
#  render: in-repo roadmap doc GENERATED block                                  #
# --------------------------------------------------------------------------- #

BADGE = {
    "shipped": "SHIPPED", "complete": "COMPLETE", "in_progress": "in progress",
    "planned": "planned", "goal": "1.0 goal", "active": "in progress",
    "inbox": "planned", "blocked": "blocked", "missing": "MISSING", "waiting": "waiting",
}


def render_doc_block(data: dict, as_of: str) -> str:
    L = []
    L.append(GEN_BEGIN)
    L.append("")
    L.append("## Live status — generated")
    L.append("")
    L.append(f"> Reconciled from rd (source of truth) **as of {as_of}** by "
             "`tools/roadmap/reconcile.py`. Milestones are the `rel-*` labels; "
             "workstreams are the 1.0-gate epics rolled up over their child items. "
             "Re-derive any line from `rd show <id>` before acting on it.")
    L.append("")

    # milestone ladder
    L.append("### Milestone ladder")
    L.append("")
    L.append("| Milestone | Theme | Status | Done | Open | Blocked | Signal |")
    L.append("|---|---|---|---:|---:|---:|---:|")
    for m in data["milestones"]:
        r = m["rollup"]
        sig = f"{r['pct']}%" if r["total"] else "—"
        L.append(f"| **{m['version']}** | {m['theme']} | {BADGE.get(m['status'], m['status'])} "
                 f"| {r['done']} | {r['open']} | {r['blocked']} | {sig} |")
    L.append("")

    # workstreams
    L.append("### 1.0-gate workstreams (epic rollups)")
    L.append("")
    L.append("| Workstream | Epic | Lands by | Status | Done/Total | Blocked |")
    L.append("|---|---|---|---|---:|---:|")
    for w in data["workstreams"]:
        r = w["rollup"]
        band = w["band"].replace("continuous", "continuous").replace("-", "→")
        L.append(f"| {w['name']} | `{w['id']}` | {band} | {BADGE.get(w['status'], w['status'])} "
                 f"| {r['done']}/{r['total'] - r['cancelled']} | {r['blocked']} |")
    L.append("")

    # per-milestone item detail
    L.append("### Per-milestone items (rd)")
    L.append("")
    for m in data["milestones"]:
        if not m["items"]:
            continue
        L.append(f"**{m['version']}** — {m['label']} "
                 f"({m['rollup']['done']}/{m['rollup']['total'] - m['rollup']['cancelled']} done)")
        L.append("")
        for it in m["items"]:
            L.append(f"- `{it['id']}` [{it['status']}] {it['title']}")
        L.append("")

    # recent releases
    L.append("### Shipped releases (git tags)")
    L.append("")
    for rel in data["releases"][:12]:
        L.append(f"- **{rel['tag']}** — {rel['note']}")
    L.append("")

    # labeling gaps
    L.append("### rd-labeling gaps (fix these to keep the source accurate)")
    L.append("")
    gaps = data["gaps"]
    gm = gaps["gate_epics_missing_rel_label"]
    if gm:
        L.append("Gate epics carrying **no `rel-*` label** — the generated milestone view "
                 "cannot place their milestone from rd alone; the band below is editorial "
                 "(`GATE_EPICS` in the script), not derived:")
        L.append("")
        for g in gm:
            lvl = "" if g["level_epic"] else " · also not `level=epic`"
            L.append(f"- `{g['id']}` — {g['name']} (editorial band: {g['expected_band']}){lvl}")
        L.append("")
    else:
        L.append("- All gate epics carry a `rel-*` label. ✓")
        L.append("")
    n = gaps["unbanded_children_of_unlabeled_gates_count"]
    L.append(f"- Children of unlabeled gate epics with no `rel-*` of their own: **{n}** "
             "(they inherit the editorial band; label them to make rd authoritative).")
    L.append("")
    L.append(GEN_END)
    return "\n".join(L)


def splice_doc(doc_text: str, block: str) -> str:
    if GEN_BEGIN in doc_text and GEN_END in doc_text:
        pre = doc_text.split(GEN_BEGIN)[0]
        post = doc_text.split(GEN_END, 1)[1]
        return pre + block + post
    # first run: insert before the anchor section
    if DOC_ANCHOR in doc_text:
        pre, _, post = doc_text.partition(DOC_ANCHOR)
        return pre + block + "\n\n---\n\n" + DOC_ANCHOR + post
    # fallback: append
    return doc_text.rstrip() + "\n\n---\n\n" + block + "\n"


# --------------------------------------------------------------------------- #
#  render: canonical + public JSON                                              #
# --------------------------------------------------------------------------- #

def canonical_json(data: dict, as_of: str) -> str:
    obj = {
        "meta": {
            "generated": as_of,
            "source": "rd (rel-* labels + 1.0-gate epics)",
            "note": "Canonical machine export. Internal — carries item IDs.",
            "tool": "tools/roadmap/reconcile.py",
        },
        "milestones": data["milestones"],
        "workstreams": data["workstreams"],
        "releases": data["releases"],
        "gaps": data["gaps"],
    }
    return json.dumps(obj, indent=1, sort_keys=False, ensure_ascii=False) + "\n"


def _scrub(s: str) -> str:
    # same trademark contract as the compat export (data/REFRESH.md)
    s = s.replace("VSI OpenVMS", "VMS").replace("OpenVMS", "VMS")
    import re
    s = re.sub(r"\bVSI\b", "vendor", s)
    return s


# Editorial one-liner for the next in-progress point release (INV-0 curation, like
# RELEASE_NOTES). Version is DERIVED (latest tag + 1); this is just the theme.
NEXT_POINT_THEME = "Actively landing on the current line."


def _next_point_release(releases: list[dict]):
    """The next in-progress POINT release, derived from the newest shipped tag
    (releases is newest-first). "V0.5-4" -> "V0.5-5"; "V0.5" -> "V0.5-1".
    Returns {version, theme} or None. Drives the site's rail-next so the public
    view tracks the point-release cadence, not a jump to the next milestone."""
    if not releases:
        return None
    import re
    m = re.match(r"^(V?\d+\.\d+)(?:-(\d+))?$", releases[0].get("tag", ""))
    if not m:
        return None
    base, pt = m.group(1), m.group(2)
    return {"version": f"{base}-{(int(pt) + 1) if pt else 1}",
            "theme": NEXT_POINT_THEME}


def public_json(data: dict, as_of: str) -> str:
    """MILESTONE-LEVEL, curated, trademark-scrubbed. NO internal item IDs."""
    ladder = []
    for m in data["milestones"]:
        r = m["rollup"]
        ladder.append({
            "version": m["version"],
            "theme": _scrub(m["theme"]),
            "status": m["status"],
            "progress": {"done": r["done"], "open": r["open"],
                         "blocked": r["blocked"], "total": r["total"] - r["cancelled"],
                         "pct": r["pct"]},
        })
    streams = []
    for w in data["workstreams"]:
        r = w["rollup"]
        streams.append({
            "name": _scrub(w["name"]),
            "summary": _scrub(w["summary"]),
            "lands_by": w["band"],
            "status": w["status"],
            "progress": {"done": r["done"], "total": r["total"] - r["cancelled"],
                         "pct": r["pct"]},
        })
    releases = [{"tag": rel["tag"], "note": _scrub(rel["note"])}
                for rel in data["releases"][:PUBLIC_RELEASE_LIMIT]]
    obj = {
        "meta": {
            "generated": as_of,
            "source": "OVMX release board (rd)",
            "note": "Derived, milestone-level public view. Regenerated each checkpoint; "
                    "do not hand-edit.",
            "scrubbed": "trademark scrub applied on render (public site)",
            "nextPointRelease": _next_point_release(data["releases"]),
        },
        "vocab": {
            "status": {
                "shipped": "Released as a tag.",
                "in_progress": "Actively landing.",
                "complete": "All tracked work done; not yet cut.",
                "planned": "Scoped, not started.",
                "goal": "The 1.0 objective.",
            }
        },
        "milestones": ladder,
        "workstreams": streams,
        "releases": releases,
    }
    return json.dumps(obj, indent=1, sort_keys=False, ensure_ascii=False) + "\n"


def atom_feed(data: dict, as_of: str) -> str:
    """Atom 1.0 release feed for the public site (one entry per shipped tag).

    Entry <updated> is the tag's committer date (deterministic); the feed
    <updated> is the newest entry's date, so re-running reconcile with no new
    tag produces a byte-identical file (idempotency contract, like the JSON).
    Notes are trademark-scrubbed with the same contract as the site JSON.
    """
    from xml.sax.saxutils import escape

    releases = data["releases"][:PUBLIC_RELEASE_LIMIT]

    def entry_updated(rel: dict) -> str:
        # Fall back to the as-of DATE (midnight UTC) only if a tag has no commit
        # date — real release tags always resolve one.
        return rel.get("date") or f"{as_of}T00:00:00+00:00"

    feed_updated = entry_updated(releases[0]) if releases else f"{as_of}T00:00:00+00:00"

    L = []
    L.append('<?xml version="1.0" encoding="utf-8"?>')
    L.append('<feed xmlns="http://www.w3.org/2005/Atom">')
    L.append(f"  <title>OpenVMX releases</title>")
    L.append("  <subtitle>Shipped releases of OpenVMX — the DCL and RMS "
             "operating environment on the Linux and NetBSD kernels.</subtitle>")
    L.append(f'  <link href="{SITE_URL}/atom.xml" rel="self"/>')
    L.append(f'  <link href="{SITE_URL}/" rel="alternate"/>')
    L.append(f"  <id>tag:openvmx.3dl.dev,2026:releases</id>")
    L.append(f"  <updated>{feed_updated}</updated>")
    L.append("  <author><name>OpenVMX</name></author>")
    for rel in releases:
        tag = rel["tag"]
        note = _scrub(rel["note"])
        updated = entry_updated(rel)
        L.append("  <entry>")
        L.append(f"    <title>{escape(tag)}</title>")
        L.append(f"    <id>tag:openvmx.3dl.dev,2026:release/{escape(tag)}</id>")
        L.append(f"    <updated>{updated}</updated>")
        # The site has no per-release page; point the reader at the roadmap,
        # which lists the shipped tags.
        L.append(f'    <link href="{SITE_URL}/roadmap/" rel="alternate"/>')
        L.append(f"    <summary>{escape(note)}</summary>")
        L.append("  </entry>")
    L.append("</feed>")
    return "\n".join(L) + "\n"


# --------------------------------------------------------------------------- #
#  write helpers (idempotent)                                                   #
# --------------------------------------------------------------------------- #

def write_if_changed(path: str, content: str, changed: list, check: bool) -> None:
    old = None
    if os.path.exists(path):
        with open(path) as fh:
            old = fh.read()
    if old == content:
        return
    changed.append(path)
    if check:
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write(content)


def print_gaps(data: dict) -> None:
    gaps = data["gaps"]
    print("rd-labeling gaps:")
    gm = gaps["gate_epics_missing_rel_label"]
    if not gm:
        print("  - all gate epics carry a rel-* label")
    for g in gm:
        lvl = "" if g["level_epic"] else "  (also not level=epic)"
        print(f"  - {g['id']} {g['name']}: NO rel-* label; editorial band "
              f"{g['expected_band']}{lvl}")
    print(f"  - unbanded children of unlabeled gates: "
          f"{gaps['unbanded_children_of_unlabeled_gates_count']}")


# --------------------------------------------------------------------------- #

def collapse_problems(items: list[dict], data: dict, min_items: int) -> list[str]:
    """Return the reasons this snapshot looks collapsed/empty (empty list == healthy).

    THE ALL-ZEROS INCIDENT (2026-08-15): reconcile ran against an empty/stale rd
    snapshot (rd unsynced / `rd list` returned nothing) and published a public
    roadmap.json where EVERY milestone had total=0 and EVERY workstream was
    'missing' — the live site showed all 0%. Nothing in the tool objected. This
    check makes that failure a hard, loud abort (see sanity_gate) so a collapsed
    snapshot can never again silently overwrite good published data.

    The three signals are deliberately overlapping (defence in depth): a genuine
    collapse trips all three, a partial truncation trips at least the floor.
    """
    problems: list[str] = []
    n = len(items)
    if n < min_items:
        problems.append(
            f"snapshot has {n} items (< floor {min_items}) — rd is likely empty or unsynced")
    gate_total = len(data["workstreams"])
    gate_present = sum(1 for w in data["workstreams"] if w["self_status"] != "missing")
    if gate_present == 0 and gate_total:
        problems.append(
            f"0/{gate_total} gate-epic nodes present — every workstream would render 'missing'")
    milestone_items = sum(m["rollup"]["total"] for m in data["milestones"])
    if milestone_items == 0:
        problems.append(
            "0 rel-* labelled items across all milestones — every milestone would render 0%")
    return problems


def sanity_gate(items: list[dict], data: dict, min_items: int, allow_empty: bool) -> None:
    """Refuse to emit anything from a collapsed snapshot (exit 2, write nothing)."""
    problems = collapse_problems(items, data, min_items)
    if problems and not allow_empty:
        sys.stderr.write(
            "reconcile.py: REFUSING TO PUBLISH — the rd snapshot looks collapsed:\n")
        for p in problems:
            sys.stderr.write(f"  - {p}\n")
        sys.stderr.write(
            "No files written; any existing published roadmap.json is left intact.\n"
            "Fix rd, then re-run. Most common cause: `rd list --all --json` returns an\n"
            "EMPTY board because this was run from a git worktree (worktrees lack the CEK\n"
            "epoch) — run reconcile.py from the primary repo checkout, not a .worktrees/ dir.\n"
            "Otherwise check rd is synced. For a genuinely empty board, pass --allow-empty.\n")
        raise SystemExit(2)


# --------------------------------------------------------------------------- #
#  release cascade verification                                                 #
# --------------------------------------------------------------------------- #

def _read(path: str) -> str | None:
    try:
        with open(path, encoding="utf-8") as fh:
            return fh.read()
    except OSError:
        return None


def check_cascade(site_dir: str) -> int:
    """Verify every downstream public surface reflects the latest release TAG.

    THE SOURCE OF TRUTH is the newest release-like git tag (same set the Atom
    feed is derived from — no new ledger). Given that tag, every public surface
    must either (a) show that version, or (b) — for the in-browser DEMO, whose
    image is expensively re-captured and can lag a cut when the capture races an
    install-reboot — carry an EXPLICIT, tracked pin acknowledging the lag. A demo
    that silently trails the latest release (the "demo shows V0.5 while V0.5-1
    shipped" drift) FAILS here loudly.

    Surfaces:
      - index.html            `data-ovmx-version` spans      == latest tag  (product edition)
      - docs/installation/…   `data-ovmx-version` spans      == latest tag  (manual "applies to")
      - atom.xml              newest <entry> <title>         == latest tag  (release feed)
      - boot/DEPLOYED_TAG     the image the demo actually boots
      - boot/qemu-worker.js   PAYLOAD_VER cache-buster       keyed to DEPLOYED_TAG (the demo payload)
      - index.html            `data-demo-version` span       == DEPLOYED_TAG (honest demo badge)
      - demo lag rule         DEPLOYED_TAG == latest, OR boot/DEMO_PIN.json
                              {pinned_to == DEPLOYED_TAG, blocked_from == latest}

    Returns 0 if every surface is current (an acknowledged demo pin counts as a
    pass, with a warning); 1 on any drift; 2 on a usage/environment error.
    """
    rels = git_releases()
    if not rels:
        print("check-cascade: no release-like git tags found (need the vms repo "
              "with tags fetched)", file=sys.stderr)
        return 2
    latest = rels[0]["tag"]
    print(f"latest release tag (source of truth): {latest}")

    fails: list[str] = []
    warns: list[str] = []

    def rule(name: str, ok: bool, shown, expected):
        mark = "OK  " if ok else "DRIFT"
        print(f"  [{mark}] {name}: shown={shown!r} expected={expected!r}")
        if not ok:
            fails.append(f"{name}: shown {shown!r}, expected {expected!r}")

    # ---- text/data surfaces that must always follow the latest tag ----
    for rel in ("index.html", os.path.join("docs", "installation", "index.html")):
        path = os.path.join(site_dir, rel)
        html = _read(path)
        if html is None:
            print(f"  [skip ] {rel}: not present")
            continue
        spans = re.findall(r"data-ovmx-version>([^<]*)</span>", html)
        if not spans:
            rule(f"{rel} data-ovmx-version", False, "(no span found)", latest)
        else:
            for v in dict.fromkeys(spans):  # unique, stable order
                rule(f"{rel} data-ovmx-version", v == latest, v, latest)

    atom = _read(os.path.join(site_dir, "atom.xml"))
    if atom is None:
        rule("atom.xml newest entry", False, "(missing)", latest)
    else:
        m = re.search(r"<entry>.*?<title>(.*?)</title>", atom, re.S)
        shown = m.group(1) if m else "(no entry)"
        rule("atom.xml newest entry", shown == latest, shown, latest)

    # ---- demo surfaces, keyed to what the demo image actually boots ----
    deployed = (_read(os.path.join(site_dir, "boot", "DEPLOYED_TAG")) or "").strip()
    if not deployed:
        rule("boot/DEPLOYED_TAG", False, "(missing/empty)", latest)
        deployed = None

    if deployed:
        # PAYLOAD_VER is a cache-buster for the demo PAYLOAD, so it tracks
        # DEPLOYED_TAG (the image on disk), not the product edition.
        worker = _read(os.path.join(site_dir, "boot", "qemu-worker.js"))
        if worker is None:
            rule("boot/qemu-worker.js PAYLOAD_VER", False, "(missing)", deployed)
        else:
            pm = re.search(r"PAYLOAD_VER\s*=\s*'([^']*)'", worker)
            pv = pm.group(1) if pm else "(not found)"
            ok = bool(pm) and re.fullmatch(re.escape(deployed) + r"(-\d+)?", pv) is not None
            rule("boot/qemu-worker.js PAYLOAD_VER (keyed to DEPLOYED_TAG)",
                 ok, pv, f"{deployed}[-<run>]")

        # The visible demo badge must state what the demo really boots.
        idx = _read(os.path.join(site_dir, "index.html")) or ""
        dspans = re.findall(r"data-demo-version>([^<]*)</span>", idx)
        if not dspans:
            rule("index.html data-demo-version (visible demo badge)",
                 False, "(no span found)", deployed)
        else:
            for v in dict.fromkeys(dspans):
                rule("index.html data-demo-version (visible demo badge)",
                     v == deployed, v, deployed)

        # The lag rule: current, or explicitly + trackably pinned.
        if deployed == latest:
            rule("demo tracks latest release", True, deployed, latest)
        else:
            pin_raw = _read(os.path.join(site_dir, "boot", "DEMO_PIN.json"))
            pin = None
            if pin_raw:
                try:
                    pin = json.loads(pin_raw)
                except json.JSONDecodeError as e:
                    print(f"  [DRIFT] boot/DEMO_PIN.json: invalid JSON ({e})")
                    fails.append("boot/DEMO_PIN.json is not valid JSON")
            if (pin and pin.get("pinned_to") == deployed
                    and pin.get("blocked_from") == latest):
                print(f"  [WARN ] demo is DELIBERATELY pinned to {deployed} "
                      f"(latest {latest} blocked): {pin.get('reason','(no reason)')} "
                      f"[tracking: {pin.get('tracking','(none)')}]")
                warns.append(f"demo pinned to {deployed}, latest is {latest}")
            else:
                print(f"  [DRIFT] demo boots {deployed} but latest release is "
                      f"{latest}, and boot/DEMO_PIN.json does not explicitly "
                      f"acknowledge it (need pinned_to={deployed}, "
                      f"blocked_from={latest})")
                fails.append(
                    f"demo silently trails: boots {deployed}, latest {latest}, "
                    f"no acknowledging DEMO_PIN.json")

    print()
    if fails:
        print(f"CASCADE DRIFT — {len(fails)} surface(s) do not reflect {latest}:")
        for f in fails:
            print(f"  - {f}")
        return 1
    if warns:
        print(f"cascade OK for {latest} (with {len(warns)} acknowledged pin(s)):")
        for w in warns:
            print(f"  - {w}")
        return 0
    print(f"cascade OK — every downstream surface reflects {latest}")
    return 0


# --------------------------------------------------------------------------- #

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rd-json", help="read snapshot from file instead of `rd list --all --json`")
    ap.add_argument("--as-of", help="date stamp YYYY-MM-DD (default: today UTC)")
    ap.add_argument("--site-dir", help="openvmx-site checkout: also write data/roadmap.json")
    ap.add_argument("--feed-only", action="store_true",
                    help="write ONLY the Atom release feed to --site-dir/atom.xml "
                         "(derived from git tags alone — needs no rd snapshot, so it "
                         "is safe to run in CI/the release train without corrupting "
                         "the rd-derived roadmap.json)")
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if regeneration would change any file (drift gate)")
    ap.add_argument("--check-cascade", action="store_true",
                    help="verify every public downstream surface in --site-dir "
                         "reflects the latest release tag (or, for the demo, "
                         "carries an explicit tracked pin). Exits 1 on drift. "
                         "Derived from git tags alone — needs no rd snapshot, so "
                         "it is safe to run in CI/the release train.")
    ap.add_argument("--print-gaps", action="store_true",
                    help="print the rd-labeling gap report and exit")
    ap.add_argument("--min-items", type=int, default=200,
                    help="collapse-guard floor: refuse to publish when the rd snapshot has "
                         "fewer items than this (default 200; a healthy board is ~1300). "
                         "Prevents an empty/unsynced snapshot from publishing all-0%%.")
    ap.add_argument("--allow-empty", action="store_true",
                    help="override the collapse-guard (only for a genuinely empty board)")
    ap.add_argument("--roadmap-doc", default=ROADMAP_DOC,
                    help="roadmap markdown to splice+write (default: the tracked doc; "
                         "tests redirect this)")
    args = ap.parse_args()

    as_of = args.as_of or _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%d")

    # Cascade check: verify the public surfaces reflect the latest release tag.
    # Derived from git tags alone (like --feed-only), so it needs no rd snapshot
    # and is safe on the release train / in CI.
    if args.check_cascade:
        if not args.site_dir:
            print("--check-cascade requires --site-dir", file=sys.stderr)
            return 2
        return check_cascade(args.site_dir)

    # Feed-only: the release feed is derived from git tags alone, so it needs no
    # rd snapshot and cannot touch the rd-derived roadmap.json. This is the mode
    # the release train runs (see openvmx-site .github/workflows/track-release.yml)
    # so a new tag refreshes the public feed without a live rd relay.
    if args.feed_only:
        if not args.site_dir:
            print("--feed-only requires --site-dir", file=sys.stderr)
            return 2
        changed_feed: list[str] = []
        site_atom = os.path.join(args.site_dir, "atom.xml")
        write_if_changed(site_atom, atom_feed({"releases": git_releases()}, as_of),
                         changed_feed, args.check)
        if args.check:
            if changed_feed:
                print("DRIFT — regeneration would change:")
                for p in changed_feed:
                    print(f"  {p}")
                return 1
            print("clean — atom.xml is up to date")
            return 0
        print("wrote atom.xml" if changed_feed else "no changes to atom.xml")
        return 0

    items = load_snapshot(args.rd_json)
    data = compute(items)
    # Kill the all-0% failure mode at the source: never emit from a collapsed snapshot.
    sanity_gate(items, data, args.min_items, args.allow_empty)

    if args.print_gaps:
        print_gaps(data)
        return 0

    changed: list[str] = []

    # 1. in-repo roadmap doc GENERATED block
    with open(args.roadmap_doc) as fh:
        doc = fh.read()
    block = render_doc_block(data, as_of)
    write_if_changed(args.roadmap_doc, splice_doc(doc, block), changed, args.check)

    # 2. canonical machine export
    write_if_changed(BUILD_JSON, canonical_json(data, as_of), changed, args.check)

    # 3. public site data
    if args.site_dir:
        site_data = os.path.join(args.site_dir, "data", "roadmap.json")
        write_if_changed(site_data, public_json(data, as_of), changed, args.check)
        # 3b. public Atom release feed (served at the site root)
        site_atom = os.path.join(args.site_dir, "atom.xml")
        write_if_changed(site_atom, atom_feed(data, as_of), changed, args.check)

    if args.check:
        if changed:
            print("DRIFT — regeneration would change:")
            for p in changed:
                print(f"  {p}")
            return 1
        print("clean — generated artifacts are up to date")
        return 0

    if changed:
        print(f"wrote (as of {as_of}):")
        for p in changed:
            print(f"  {p}")
    else:
        print(f"no changes (as of {as_of})")
    print_gaps(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
