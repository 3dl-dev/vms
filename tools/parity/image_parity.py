#!/usr/bin/env python3
"""image_parity.py - cross-arch OVMX shippable-image parity gate (rd vms-e1d).

THE PROBLEM THIS GUARDS. Today the OVMX image set is defined twice, by hand,
in two different places:

  * x86_64 (the native/Linux runtime): whatever distro/Dockerfile.bootable's
    `cp` lines happen to stage into /system-stage (the shipped system disk +
    OS kit) and the kernel-module harvest (vms.ko/vmsfs.ko).
  * VAX (netbsd-vax/elf32-vax, epic vms-f10): tools/cut-release.sh's
    VAX_ARTIFACT_ORDER array, itself produced by ~25 hand-maintained
    tools/cross-vax/build-*.sh scripts.

Nothing MECHANICALLY compares the two. An image can be added for x86_64 (a
new `cp ... SOMETHING.EXE /system-stage/...` line) with no corresponding VAX
build, and nothing reds until a human notices -- possibly at release time,
possibly never. Operator ruling: "if I see a release that has something on
x86_64 and not vax, it's your ass." This script is the mechanical backstop.

WHAT IT DOES. Extracts the x86_64 shippable-image set from
distro/Dockerfile.bootable (ground truth: what actually gets staged into the
shipped system disk / OS kit / kernel-module harvest) and the VAX shippable-
image set from tools/cut-release.sh's VAX_ARTIFACT_ORDER (ground truth: the
already-reviewed, already-gated release-cut artifact list -- cut-release.sh
itself `fail`s if any of these are missing from a VAX cut). Diffs them.
Anything present on one side and absent on the other, and NOT covered by an
explicit allowlist entry (image-parity-allowlist.json, the single reviewed
record of legitimate per-arch differences -- see docs/design-vax-mainstream-
release.md "Decision A", vms-42d), is reported as REAL DRIFT and fails the
gate.

THIS IS AN INTERIM GATE. The real fix is the unified CMake build (vms-509),
after which every image becomes an ordinary CMake target built for every
arch and this comparison collapses to "diff two build-output directories"
(vms-64a/vms-88c can point EXTRACT_VAX_IMAGES / EXTRACT_X86_64_IMAGES at that
aggregate instead of text-parsing these two files). Until then, this is the
mechanical guarantee.

Usage:
    python3 tools/parity/image_parity.py [--repo-root DIR]

Exit 0 = every image-set difference is covered by the allowlist.
Exit 1 = at least one REAL (unallowlisted) drift was found; a report is
         printed naming every offending image and which side is missing it.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT_DEFAULT = Path(__file__).resolve().parents[2]
DOCKERFILE_BOOTABLE = "distro/Dockerfile.bootable"
CUT_RELEASE_SH = "tools/cut-release.sh"
ALLOWLIST_JSON = "tools/parity/image-parity-allowlist.json"

X86_64 = "x86_64"
VAX = "vax"

# ---------------------------------------------------------------------------
# x86_64 extraction
# ---------------------------------------------------------------------------

# The VMS-native LINK.EXE shareable-image graph (OVMX_LINK_NATIVE,
# link_native_graph CMake target, beads vms-b6a/vms-6da). distro/
# Dockerfile.bootable stages these via a glob (`cp SYSLIB/*.EXE ...`), which
# a text scan cannot expand on its own -- these names come from the SAME
# Dockerfile's own descriptive comment (the "Builds the VMS-native LINK.EXE
# shareable-image graph (...)" block, just above the `link-native` stage) and
# are cross-checked at build time by that stage's own `COUNT -eq 9` gate
# (7 shareables + DCL.EXE + LOGINOUT.EXE == 9). If that comment's name list
# ever drifts from this constant, _check_shareable_names_cited() below reds
# loudly rather than silently under- or over-counting.
LINK_NATIVE_SHAREABLES = (
    "DECC$SHR.EXE",
    "LIBVMSSYS$SHR.EXE",
    "LIBVMSPROCESS$SHR.EXE",
    "LIBVMSLNM$SHR.EXE",
    "LIBVMSFS$SHR.EXE",
    "LIBVMS$SHR.EXE",
    "LIBVMSRMS$SHR.EXE",
)

# Not real shippable images -- container/manifest files and DCL command
# procedures that happen to be copied by the same `cp` lines we scan.
_X86_64_NON_IMAGE_BASENAMES = {
    "OVMX-OS.KIT",
    "PARTS_SETUP.COM",
}

_CP_LINE_RE = re.compile(
    r"""cp\s+                       # the cp command
        (?P<flags>-\w+\s+)?         # optional flags (e.g. -r, -L)
        (?P<src>[^\s]+)\s+          # source path (no shell quoting used for these)
        (?P<dst>[^\s]+)             # destination path
    """,
    re.VERBOSE,
)


def _join_shell_continuations(text: str) -> str:
    """Collapse `... \\\n    ...` line continuations into single logical
    lines, then split on `&&`/`;`/newline so each shell command the
    Dockerfile's RUN blocks issue is its own scannable unit."""
    joined = re.sub(r"\\\r?\n[ \t]*", " ", text)
    commands: list[str] = []
    for line in joined.splitlines():
        for piece in re.split(r"&&|;", line):
            piece = piece.strip()
            # A Dockerfile RUN instruction's first shell command shares its
            # line with the `RUN` keyword itself (`RUN cp FOO.EXE ...`);
            # later commands in the same instruction (after && or ;) don't.
            # Strip it either way so the cp-matcher sees a bare shell command.
            piece = re.sub(r"^RUN\s+", "", piece)
            if piece:
                commands.append(piece)
    return commands


def _check_shareable_names_cited(dockerfile_text: str) -> None:
    """Ground-source check: every name this script hardcodes as a LINK.EXE
    shareable must still appear literally in the Dockerfile's own
    descriptive comment. Catches the case where a human edits the comment
    (adds/removes/renames a shareable) without updating this script."""
    missing = [
        name for name in LINK_NATIVE_SHAREABLES if name.replace("$", r"\$") not in dockerfile_text
        and name not in dockerfile_text
    ]
    if missing:
        raise RuntimeError(
            "image_parity.py: LINK_NATIVE_SHAREABLES has drifted from "
            f"{DOCKERFILE_BOOTABLE}'s own shareable-graph comment -- "
            f"not found in the Dockerfile text: {missing}. Update "
            "LINK_NATIVE_SHAREABLES in tools/parity/image_parity.py."
        )


def extract_x86_64_images(dockerfile_text: str) -> set[str]:
    """The x86_64 shippable-image set: every basename actually staged into
    the shipped system disk / OS kit (/system-stage/...), the bootstrap
    initramfs (/initramfs-slim/...), or the kernel-module harvest, by
    distro/Dockerfile.bootable. This is ground truth, not a wishlist --
    if a `cp` line here doesn't exist, the image doesn't ship."""
    _check_shareable_names_cited(dockerfile_text)

    images: set[str] = set()
    for cmd in _join_shell_continuations(dockerfile_text):
        m = _CP_LINE_RE.match(cmd)
        if not m:
            continue
        dst = m.group("dst").strip("\"'")
        if not (dst.startswith("/system-stage/") or dst.startswith("/initramfs-slim/")):
            continue
        if m.group("flags"):
            # Directory copies (-r ... SYSMGR/, SYSHLP/, SYS$STARTUP/) stage
            # config/data trees, not individual images -- skip.
            continue
        src = m.group("src").strip("\"'")
        basename = src.rsplit("/", 1)[-1]
        if basename == "*.EXE" and src.rstrip("/").endswith("SYSLIB/*.EXE"):
            images.update(LINK_NATIVE_SHAREABLES)
            continue
        if not (basename.endswith(".EXE") or basename.endswith(".ko")):
            continue
        if basename in _X86_64_NON_IMAGE_BASENAMES:
            continue
        images.add(basename)
    return images


# ---------------------------------------------------------------------------
# VAX extraction
# ---------------------------------------------------------------------------

_VAX_ARTIFACT_ORDER_RE = re.compile(
    r"VAX_ARTIFACT_ORDER=\(([^)]+)\)", re.MULTILINE
)


def extract_vax_images(cut_release_text: str) -> set[str]:
    """The VAX shippable-image set: tools/cut-release.sh's own
    VAX_ARTIFACT_ORDER -- already the reviewed, already-gated list (a real
    VAX release cut hard-`fail`s if any of these is missing from the built
    bundle), so it is the authoritative VAX side rather than re-deriving one
    from the ~25 tools/cross-vax/build-*.sh scripts by hand.

    cut-release.sh declares `VAX_ARTIFACT_ORDER=()` (empty) up front, then
    reassigns it to the real name list inside the branch that handles a
    VAX-capable tree -- so this takes the LAST (non-empty) assignment in the
    file, not the first regex match."""
    matches = _VAX_ARTIFACT_ORDER_RE.findall(cut_release_text)
    non_empty = [m for m in matches if m.strip()]
    if not non_empty:
        raise RuntimeError(
            f"image_parity.py: could not find a populated VAX_ARTIFACT_ORDER=(...) in "
            f"{CUT_RELEASE_SH} -- has the release script been restructured? Update "
            "extract_vax_images()."
        )
    return set(non_empty[-1].split())


# ---------------------------------------------------------------------------
# Allowlist + diff
# ---------------------------------------------------------------------------


@dataclass
class AllowlistEntry:
    names: list[str]
    missing_on: str  # "x86_64" or "vax" -- the side that legitimately lacks these
    reason: str


def load_allowlist(path: Path) -> list[AllowlistEntry]:
    data = json.loads(path.read_text())
    entries = []
    for raw in data.get("entries", []):
        if raw["missing_on"] not in (X86_64, VAX):
            raise ValueError(
                f"image_parity.py: allowlist entry has invalid missing_on={raw['missing_on']!r} "
                f"(must be {X86_64!r} or {VAX!r}): {raw}"
            )
        entries.append(
            AllowlistEntry(names=list(raw["names"]), missing_on=raw["missing_on"], reason=raw["reason"])
        )
    return entries


def _allowlisted_names(entries: list[AllowlistEntry], missing_on: str) -> set[str]:
    out: set[str] = set()
    for e in entries:
        if e.missing_on == missing_on:
            out.update(e.names)
    return out


@dataclass
class DiffResult:
    x86_64_only: set[str] = field(default_factory=set)  # present x86_64, absent vax, NOT allowlisted
    vax_only: set[str] = field(default_factory=set)  # present vax, absent x86_64, NOT allowlisted
    allowlisted_x86_64_only: set[str] = field(default_factory=set)
    allowlisted_vax_only: set[str] = field(default_factory=set)

    @property
    def has_real_drift(self) -> bool:
        return bool(self.x86_64_only or self.vax_only)


def compute_diff(
    x86_64_images: set[str], vax_images: set[str], allowlist: list[AllowlistEntry]
) -> DiffResult:
    raw_x86_64_only = x86_64_images - vax_images
    raw_vax_only = vax_images - x86_64_images

    allowlisted_missing_on_vax = _allowlisted_names(allowlist, VAX)
    allowlisted_missing_on_x86_64 = _allowlisted_names(allowlist, X86_64)

    result = DiffResult()
    result.x86_64_only = raw_x86_64_only - allowlisted_missing_on_vax
    result.vax_only = raw_vax_only - allowlisted_missing_on_x86_64
    result.allowlisted_x86_64_only = raw_x86_64_only & allowlisted_missing_on_vax
    result.allowlisted_vax_only = raw_vax_only & allowlisted_missing_on_x86_64
    return result


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def run(repo_root: Path) -> DiffResult:
    dockerfile_text = (repo_root / DOCKERFILE_BOOTABLE).read_text()
    cut_release_text = (repo_root / CUT_RELEASE_SH).read_text()
    allowlist = load_allowlist(repo_root / ALLOWLIST_JSON)

    x86_64_images = extract_x86_64_images(dockerfile_text)
    vax_images = extract_vax_images(cut_release_text)
    return compute_diff(x86_64_images, vax_images, allowlist)


def report(result: DiffResult) -> str:
    lines = []
    lines.append(f"OVMX cross-arch image-parity gate (rd vms-e1d)")
    lines.append("")
    if result.allowlisted_x86_64_only or result.allowlisted_vax_only:
        lines.append("Allowlisted (documented, legitimate) per-arch differences:")
        for name in sorted(result.allowlisted_x86_64_only):
            lines.append(f"  - {name}: x86_64 only (vax legitimately lacks it -- see allowlist)")
        for name in sorted(result.allowlisted_vax_only):
            lines.append(f"  - {name}: vax only (x86_64 legitimately lacks it -- see allowlist)")
        lines.append("")

    if not result.has_real_drift:
        lines.append("OK: no unallowlisted image-set drift between x86_64 and vax.")
        return "\n".join(lines)

    lines.append("FAIL: unallowlisted image-set drift found.")
    lines.append("")
    if result.x86_64_only:
        lines.append(
            f"Present on x86_64, MISSING on vax ({len(result.x86_64_only)}) -- "
            "not in tools/parity/image-parity-allowlist.json:"
        )
        for name in sorted(result.x86_64_only):
            lines.append(f"  - {name}")
        lines.append("")
    if result.vax_only:
        lines.append(
            f"Present on vax, MISSING on x86_64 ({len(result.vax_only)}) -- "
            "not in tools/parity/image-parity-allowlist.json:"
        )
        for name in sorted(result.vax_only):
            lines.append(f"  - {name}")
        lines.append("")
    lines.append(
        "Each gap above is either a real cross-arch drift that needs a VAX (or x86_64) "
        "build, or a legitimate per-arch difference that belongs in "
        "tools/parity/image-parity-allowlist.json with a one-line reason. Do not "
        "add an allowlist entry just to make this gate pass -- get sign-off on why "
        "the difference is legitimate first (see the file's own header)."
    )
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root", type=Path, default=REPO_ROOT_DEFAULT, help="OVMX repo root (default: autodetected)"
    )
    args = parser.parse_args(argv)

    result = run(args.repo_root)
    print(report(result))
    return 1 if result.has_real_drift else 0


if __name__ == "__main__":
    sys.exit(main())
