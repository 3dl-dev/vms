#!/bin/bash
# run_tests.sh - Host-side: boot QEMU, capture serial output, parse results
#
# This runs inside the Docker container. It launches QEMU with the kernel
# and initramfs built during docker build, captures all serial output, and
# exits with 0 if all tests pass, 1 otherwise.

set -euo pipefail

# The verdict lives in a sourceable helper (rd vms-b1f) so that run_tests.sh
# and the controls that exercise it cannot drift, and so that the decision is
# testable without booting QEMU. It takes a FILE, deliberately: the defect it
# replaces was `echo "$OUTPUT" | grep -q ...` under pipefail, where grep exits
# on match without draining, echo takes SIGPIPE, and the pipeline reports 141 —
# inverting the verdict on any run with more than a pipe buffer of output after
# the matching line. Do not reintroduce a pipeline here.
. "$(dirname "$0")/lib/harness_verdict.sh"

# Whole-VM wall budget (rd vms-055). This is ONE QEMU boot that runs every
# executive suite (~76 as of this writing) sequentially -- there is no
# per-suite timeout, only this single wall. It was 120s, and the suite set
# outgrew it: measured under CI contention the guest was SIGTERM'd around suite
# ~58/76 (e.g. test_syssvc_procnam) with ZERO failing assertions -- a pure
# capacity red, not a suite defect, and load-dependent (it passed on 2 of 3
# recent main runs). The work-floor is ~120s * 76/58 ~= 157s.
#
# 300 -> 600 (rd vms-4003, extends vms-055). 300s fixed the common case (a
# normal GitHub runner does the full 76-suite run in ~90s), but GitHub's TCG
# runner quality varies wildly and a pathologically slow (~5x) runner reached
# only 60/76 in 300s -- extrapolating, the full set needs ~380s there. 600s
# covers that observed worst case (60 suites in 300s -> 76 ~= 380s) with ~1.5x
# margin, while the ci.yml kernel-executive job's own timeout-minutes: 60
# remains the OUTER bound so a genuine hang (not mere slowness) still surfaces.
# A wall overrun is detected (timeout rc=124) and reported legibly below, but
# STILL fails the gate -- see the verdict path.
TIMEOUT=600
KERNEL=/boot/vmlinuz
INITRD=/initramfs.cpio.gz
ARCH=$(uname -m)

# SUITE SHARDING (rd vms-ea7). This whole-VM wall (TIMEOUT above) is a fixed
# budget; a pathologically slow (~10x) GitHub TCG runner walled the full
# ~79-suite run at 60/79. The durable fix is to boot N of these VMs in
# parallel (a ci.yml matrix), each running ~1/N of the suites, so the SAME
# 600s wall trivially covers a ~13-suite subset regardless of runner speed --
# rather than chasing unbounded runner variance with an ever-bigger wall (Rule
# 8: sharding is the fix, not a bigger wall; the wall is unchanged at 600s).
#
# The coordinates are passed to the guest ON THE KERNEL COMMAND LINE
# (ovmx.shard / ovmx.shards); init.sh parses them and runs only its residue
# class by md5(name) mod N. The DEFAULT is shard 0 of 1 = run everything, so
# every existing caller (the negative-control image, a bare `docker run
# ovmx-ktest`) is byte-for-byte unaffected -- only a caller that sets these
# env vars shards. Set via `docker run -e SHARD_INDEX=I -e SHARD_TOTAL=N`.
SHARD_INDEX=${SHARD_INDEX:-0}
SHARD_TOTAL=${SHARD_TOTAL:-1}
case "$SHARD_TOTAL" in ''|*[!0-9]*|0) SHARD_TOTAL=1 ;; esac
case "$SHARD_INDEX" in ''|*[!0-9]*) SHARD_INDEX=0 ;; esac
KCMD_SHARD="ovmx.shard=$SHARD_INDEX ovmx.shards=$SHARD_TOTAL"

# ASSERTION TRANSCRIPT (vms-b5b round 2). ttyS0 (below) carries the boot
# banner, kernel printk and init.sh's own aggregate lines -- exactly what it
# always has. A SECOND serial port, ttyS1, is wired to a plain file so that
# init.sh's per-suite loop (which writes there via fd 4 -- see init.sh) has a
# wire kernel printk never touches, closing the printk-splice class the
# round-1 audit found (tee-to-console put suite text and printk on the SAME
# wire; see init.sh's header comment for the measured line). x86_64 only:
# CI's kernel-executive/negctl jobs run exclusively on x86_64 GitHub runners,
# so that is the only path this has been verified against a real /dev/vms;
# the aarch64 branch below is left on the single-console (round-1) shape,
# disclosed, not silently assumed fixed -- see the comment there.
ASSERT_TRANSCRIPT=$(mktemp) || { echo "run_tests.sh: mktemp failed" >&2; exit 2; }

# Two virtio disks (vms-3e8). The executive enumerates the node's virtio block
# devices into DK units (DKA0: from vda, DKA100: from vdb) at module init --
# test_kmod_disk asserts that mapping against a real vms.ko, so the guest must
# actually HAVE two virtio disks. Cleaned up with the transcript below (ONE trap,
# all temp files -- see the trap note further down).
#
#   vda (DKA0:)   -- a GENUINE real-VAX ODS-2 volume: the SYSTEM DISK. The
#                    Files-11 ACP $MOUNT VALIDATES a real Files-11 structure (home
#                    block + SCB) against a real /dev/vms and records it
#                    executive-global; $ASSIGN of DKA0:/SYS$SYSDEVICE then reaches
#                    it (sys_assign.c routes the boot unit to the ACP). The
#                    fixture /ods2_real.img is staged by tests/qemu/Dockerfile
#                    (COPY tests/ods2/real_vax_ods2.dsk /ods2_real.img); it is
#                    copied to a writable temp so QEMU's raw block open never
#                    touches the staged image. (test_syssvc_acp_{channel,mount}.)
#   vdb (DKA100:) -- BLANK 16M: the non-ODS-2 media the ACP $MOUNT must REJECT
#                    fail-honest (its home block is all-zero, so ods2_home_parse
#                    fails the "DECFILE11B  " format check), AND the scratch unit
#                    test_syssvc_initialize.c formats. test_kmod_disk reads only
#                    the unit->backing-device mapping, not content or size.
OVMX_DISK0=$(mktemp) || { echo "run_tests.sh: mktemp failed" >&2; exit 2; }
OVMX_DISK1=$(mktemp) || { echo "run_tests.sh: mktemp failed" >&2; exit 2; }
OVMX_DISK2=$(mktemp) || { echo "run_tests.sh: mktemp failed" >&2; exit 2; }
truncate -s 16M "$OVMX_DISK1"
OVMX_ODS2_SRC=/ods2_real.img
if [ -f "$OVMX_ODS2_SRC" ]; then
    cp "$OVMX_ODS2_SRC" "$OVMX_DISK0"
else
    echo "run_tests.sh: FATAL: ODS-2 fixture $OVMX_ODS2_SRC missing -- the Files-11" >&2
    echo "                ACP mount test (vms-127) needs a genuine ODS-2 volume on DKA0: (vda)" >&2
    exit 2
fi
#   vdc (DKA200:) -- a GENERATED multi-version genuine ODS-2 volume (from
#                    /ods2_search.img == tests/qemu/mkimage_ods2_search.c). Its
#                    [SRCH] directory carries a name at several versions, which
#                    the real-VAX fixture on DKA0: does not; the IO$_ACPCONTROL
#                    wildcard directory search test (test_syssvc_acp_search,
#                    vms-a0b) $MOUNTs it and iterates the versions.
OVMX_ODS2_SEARCH_SRC=/ods2_search.img
if [ -f "$OVMX_ODS2_SEARCH_SRC" ]; then
    cp "$OVMX_ODS2_SEARCH_SRC" "$OVMX_DISK2"
else
    echo "run_tests.sh: FATAL: ODS-2 search fixture $OVMX_ODS2_SEARCH_SRC missing --" >&2
    echo "                the \$SEARCH test (vms-a0b) needs a multi-version ODS-2 volume on DKA200: (vdc)" >&2
    exit 2
fi
#   vdd (DKA300:) -- a GENERATED genuine ODS-2 SYSTEM-DISK tree (from
#                    /ods2_sysvol.img == tests/qemu/mkimage_ods2_sysvol.c). It
#                    carries [SYS0.SYSCOMMON.{SYSEXE,SYSMGR,SYSLIB}] with
#                    SYSUAF.DAT etc., so the directory-logical resolution test
#                    (test_syssvc_dirlogical_acp, vms-0044) can compose
#                    SYS$SYSTEM:SYSUAF.DAT and walk the ACP to the file FID.
OVMX_DISK3=$(mktemp) || { echo "run_tests.sh: mktemp failed" >&2; exit 2; }
OVMX_ODS2_SYSVOL_SRC=/ods2_sysvol.img
if [ -f "$OVMX_ODS2_SYSVOL_SRC" ]; then
    cp "$OVMX_ODS2_SYSVOL_SRC" "$OVMX_DISK3"
else
    echo "run_tests.sh: FATAL: ODS-2 sysvol fixture $OVMX_ODS2_SYSVOL_SRC missing --" >&2
    echo "                the directory-logical test (vms-0044) needs a system-disk ODS-2 volume on DKA300: (vdd)" >&2
    exit 2
fi
trap 'rm -f "$ASSERT_TRANSCRIPT" "$OVMX_DISK0" "$OVMX_DISK1" "$OVMX_DISK2" "$OVMX_DISK3"' EXIT

# One virtio-net NIC (vms-9d2). Exactly as the two virtio disks above give the
# executive real block devices to enumerate into DK units, this gives it a real
# Ethernet net device to enumerate into the VMS device ETH0: -- test_kmod_devtab
# / test_syssvc_getdvi / test_syssvc_showdev assert ETH0: against a real vms.ko,
# so the guest must actually HAVE a NIC. Added to the QEMU line below as
# -netdev user (SLIRP, zero host config, unprivileged/CI-safe) + a
# virtio-net-pci device with romfile= to suppress the PXE ROM (no boot pause),
# the same pair distro/boot/run-qemu.sh ships (vms-7bd). The executive sources
# the device through the GENERIC netdev abstraction, so virtio-net here stands in
# for whatever NIC a real deployment has -- the code path is driver-agnostic.

# Select QEMU binary and machine config based on architecture
if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
    # NOT verified: mach-virt provides exactly one PL011 UART by default: a
    # second port needs an explicit -device pl011 at a free MMIO address,
    # which this repo has no aarch64 QEMU host to test against (tracked as
    # rd vms-c83). init.sh's fallback (fd 4 -> fd 1 when /dev/ttyS1 is
    # absent) keeps this path running exactly as round 1 left it -- flaky
    # under printk, LOUDLY disclosed on the console, not silently "fixed".
    SECOND_SERIAL=()
else
    QEMU=qemu-system-x86_64
    # KVM acceleration (vms-fb8): hardware virt when /dev/kvm is writable, else
    # TCG software emulation (identical behavior, ~10x slower). This is the path
    # the kernel-executive shards boot on every PR, so KVM here is the direct
    # per-PR-loop accelerant. See distro/boot/run-qemu.sh for the full rationale.
    if [ -w /dev/kvm ]; then
        MACHINE="-accel kvm -cpu host"
    else
        MACHINE="-accel tcg"
    fi
    CONSOLE="console=ttyS0"
    # The PC machine wires an Nth -serial to COM(N+1) automatically -- no
    # -device needed, the same convenience the existing ttyS0 stdio backend
    # already relies on. This is the path CI actually boots.
    SECOND_SERIAL=(-serial "file:$ASSERT_TRANSCRIPT")
fi

echo "=== OVMX QEMU Test Runner ==="
echo "Architecture: $ARCH"
echo "QEMU: $QEMU"
echo "Kernel: $KERNEL ($(ls -lh $KERNEL | awk '{print $5}'))"
echo "Initrd: $INITRD ($(ls -lh $INITRD | awk '{print $5}'))"
echo "Shard: $SHARD_INDEX of $SHARD_TOTAL (kernel cmdline: $KCMD_SHARD)"
echo ""

# Run QEMU with serial on stdio, capture all output.
# Capture timeout(1)'s exit code instead of discarding it: rc=124 means the
# whole-VM wall fired (SIGTERM on budget exhaustion), which we render legibly
# in the verdict path below (rd vms-055) rather than letting it masquerade as a
# suite assertion failure. `|| QEMU_RC=$?` keeps `set -e` from aborting here
# exactly as the old `|| true` did; there is no pipe, so pipefail is not in play.
QEMU_RC=0
OUTPUT=$(timeout "$TIMEOUT" $QEMU \
    $MACHINE \
    -kernel "$KERNEL" \
    -initrd "$INITRD" \
    -nographic \
    -append "$CONSOLE panic=-1 loglevel=4 $KCMD_SHARD" \
    -m 256M \
    -no-reboot \
    -smp 1 \
    -netdev user,id=net0 \
    -device virtio-net-pci,netdev=net0,romfile= \
    -nodefaults \
    -serial stdio \
    "${SECOND_SERIAL[@]}" \
    -drive if=none,id=ovmxdisk0,file="$OVMX_DISK0",format=raw \
    -device virtio-blk-pci,drive=ovmxdisk0 \
    -drive if=none,id=ovmxdisk1,file="$OVMX_DISK1",format=raw \
    -device virtio-blk-pci,drive=ovmxdisk1 \
    -drive if=none,id=ovmxdisk2,file="$OVMX_DISK2",format=raw \
    -device virtio-blk-pci,drive=ovmxdisk2 \
    -drive if=none,id=ovmxdisk3,file="$OVMX_DISK3",format=raw \
    -device virtio-blk-pci,drive=ovmxdisk3 \
    2>&1) || QEMU_RC=$?

# Splice the assertion transcript (ttyS1, if this arch has one) back into
# $OUTPUT right after init.sh's own banner announcing it took that channel.
# init.sh's loop prints NOTHING ELSE to ttyS0 for the whole loop's duration
# (every per-suite line went to fd 4 instead, see init.sh) -- so there is no
# "gap" content on ttyS0 to remove, only a single insertion point to fill.
# When the banner is absent (fallback: /dev/ttyS1 was never wired, e.g. the
# aarch64 branch above), $OUTPUT already carries everything inline exactly
# as round 1 left it, and this is a no-op.
ASSERT_BANNER='--- assertion channel: /dev/ttyS1 (separate from console) ---'
if printf '%s\n' "$OUTPUT" | grep -qF -- "$ASSERT_BANNER"; then
    OUTPUT=$(printf '%s\n' "$OUTPUT" | awk -v bannerfile="$ASSERT_TRANSCRIPT" -v banner="$ASSERT_BANNER" '
        { print }
        index($0, banner) == 1 {
            while ((getline line < bannerfile) > 0) print line
            close(bannerfile)
        }
    ')
fi

# Print full output
echo "$OUTPUT"
echo ""

# Parse final results line.
#
# THE ZERO MUST BE A WHOLE NUMBER. This was `FINAL RESULTS:.*0 suites failed`,
# which is satisfied by the trailing 0 of ANY count ending in zero -- so
# "10 suites failed" reported ALL KERNEL MODULE TESTS PASSED and exited 0.
# Latent since the harness was written; it first fires on the vms-a35 merge
# product, where vms-8019's test_kmod_procnam, vms-0ff's test_kmod_pin and
# vms-d0b's test_kmod_devtab together push the executive-absent failure count
# into double digits. Two ways it lies, both fatal to the barrier:
#   - the positive kernel-executive job goes green with 10 (or 20) real
#     failures, which is the whole facility silently untested;
#   - the negative-control job asserts a NONZERO exit and gets 0, so it fails
#     with "expected the harness to fail, but it exited 0".
# [^0-9] before the 0 requires the zero to stand alone.
OUTPUT_FILE=$(mktemp) || { echo "run_tests.sh: mktemp failed" >&2; exit 2; }
# ONE trap, both temp files: a second `trap ... EXIT` REPLACES the first
# rather than stacking with it, so registering ASSERT_TRANSCRIPT's cleanup
# separately above would have silently dropped it the moment this line ran.
trap 'rm -f "$OUTPUT_FILE" "$ASSERT_TRANSCRIPT" "$OVMX_DISK0" "$OVMX_DISK1" "$OVMX_DISK2" "$OVMX_DISK3"' EXIT
printf '%s\n' "$OUTPUT" > "$OUTPUT_FILE"

if harness_verdict_zero_failures "$OUTPUT_FILE"; then
    echo "=========================================="
    echo "  ALL KERNEL MODULE TESTS PASSED"
    echo "=========================================="
    exit 0
else
    # Make a wall-timeout LEGIBLE rather than silent (rd vms-055). timeout(1)
    # returns 124 when it SIGTERMs the guest on budget exhaustion. Before this,
    # a budget overrun exited nonzero indistinguishably from a real suite
    # failure -- the transcript was simply truncated before FINAL RESULTS, so
    # the verdict helper reported failure with no hint that the cause was the
    # clock, not an assertion. We name it here. Crucially this does NOT swallow
    # the failure: a genuine overrun must still redden the gate, just legibly,
    # so we fall through to the same exit 1 below. Every other nonzero exit
    # (QEMU_RC != 124, or a real FAIL line) stays a genuine suite failure.
    if [ "$QEMU_RC" -eq 124 ]; then
        echo "=========================================="
        echo "  KE VM WALL BUDGET (${TIMEOUT}s) EXCEEDED"
        echo "=========================================="
        echo "run_tests.sh: the whole-VM wall timeout fired (timeout rc=124) before"
        echo "the suite run reached its own FINAL RESULTS accounting. This is a HARNESS"
        echo "CAPACITY issue (rd vms-055), NOT a suite assertion failure -- assertions"
        echo "that DID run may all have passed. The gate still fails (a real overrun"
        echo "must not be swallowed). If the suite set has genuinely outgrown ${TIMEOUT}s,"
        echo "raise TIMEOUT in tests/qemu/run_tests.sh; do not drop or skip a suite."
        echo "If ${TIMEOUT}s ever proves insufficient, the SCALABLE fix (not implemented"
        echo "here, rd vms-4003) is to SHARD the ~76 suites across N parallel VM jobs --"
        echo "each shard boots its own QEMU over a suite subset, so its wall trivially"
        echo "covers that subset regardless of runner speed; raising a single whole-VM"
        echo "wall only buys linear headroom against an unbounded-slow TCG runner."
        echo ""
    fi
    echo "=========================================="
    echo "  KERNEL MODULE TESTS FAILED"
    echo "=========================================="
    # Show individual test results for easy diagnosis
    echo ""
    echo "Individual test results:"
    grep -E "(PASS|FAIL):" "$OUTPUT_FILE" || true
    exit 1
fi
