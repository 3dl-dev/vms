#!/bin/bash
# test_release_acceptance_e2e.sh - generalized RELEASE-ACCEPTANCE gate
# (vms-a86f, epic vms-a84 RELEASE ENGINEERING).
#
# THE PROBLEM THIS REPLACES: OVMX's only release proof was
# tests/qemu/test_parts_demo_e2e.sh, which boots the DEV bootable image
# (distro/Dockerfile.bootable's own /boot/*, built straight from the working
# tree) -- never the actual cut release bundle tools/cut-release.sh (vms-d73)
# produces, and never checks that the shipped version matches what was cut.
# A dev build passing this gate proves nothing about what a release recipient
# would actually receive and boot.
#
# WHAT THIS GATE PROVES, GROUND-SOURCE:
#   1. It boots the ACTUAL cut artifacts -- $RELEASE_DIR/vmlinuz,
#      $RELEASE_DIR/initramfs-ovmx-slim.cpio.gz, $RELEASE_DIR/ovmx-distrib.img
#      -- bind-mounted in from a real tools/cut-release.sh run (see
#      run_release_acceptance_e2e.sh's header + .github/workflows/ci.yml's
#      release-acceptance-e2e job for how CI produces $RELEASE_DIR). This
#      script never builds anything; it only boots what it is handed.
#   2. It runs the SAME acceptance scenario as test_parts_demo_e2e.sh (the
#      0.2 "killer app" demo: login -> @SYS$UPDATE:PARTS_SETUP.COM ->
#      $ PARTS -> keyed RMS lookups -> zero Unix leaks -> independent
#      $ DIRECTORY corroboration) -- same DCL commands, same expected
#      %PARTS-*/%COPY-*/%RMS-* strings, so a regression here is the same
#      regression parts_demo_e2e would catch, now measured against the
#      RELEASE ARTIFACT instead of a dev build. Skippable via
#      SKIP_PARTS_SCENARIO=1 for a fast boot+version-only run (used by the
#      negative control below and by run_release_acceptance_e2e.sh's
#      version-mismatch demonstration, which does not need a ~25-minute RMS
#      load to prove a version string disagrees).
#   3. THE VERSION-MATCH ASSERTION: after login, a real
#      `$ PRODUCT SHOW PRODUCT` reports the OVMX_PRODUCT_VERSION baked into
#      the booted artifact (src/product/product.c's do_show(), reading
#      ovmx_product_version() -- itself single-sourced from
#      src/libvms/include/ovmx_identity.h, INV-1). This is compared against
#      $EXPECTED_PRODUCT_VERSION, which the CALLER (run_release_acceptance_
#      e2e.sh) derives from the SAME cut's release-manifest.json
#      "product_version" field -- an independent record of what was cut,
#      written by tools/cut-release.sh at cut time, not re-derived from the
#      artifact under test. A mismatch is a REAL gate failure (recorded via
#      bad(), not swallowed).
#   4. THE NEGATIVE CONTROL: the exact same comparison is run a second time
#      against the SAME real, freshly-captured PRODUCT SHOW PRODUCT output,
#      but against a deliberately corrupted expected value
#      ("$EXPECTED_PRODUCT_VERSION-NEGCTRL-MISMATCH", guaranteed to differ
#      from anything real). This proves the comparator can actually detect a
#      disagreement -- it is not a check that always reports "match" no
#      matter what it is given, which would make assertion #3 vacuous. See
#      run_release_acceptance_e2e.sh + the release-acceptance-e2e CI job for
#      the complementary, heavier demonstration: rerunning this WHOLE gate
#      with a genuinely wrong $EXPECTED_PRODUCT_VERSION and asserting the
#      gate script itself exits nonzero.
#
# WHAT WOULD MAKE THIS TEST FAIL HONESTLY: any of the parts_demo_e2e failure
# modes (see that file's header) against the release artifact, OR
# `$ PRODUCT SHOW PRODUCT` reporting a version other than
# $EXPECTED_PRODUCT_VERSION, OR the negative control's corrupted comparison
# failing to detect the deliberate mismatch (which would mean assertion #3
# cannot be trusted).
#
# Usage (run INSIDE the ovmx-boot image, which supplies qemu-system-* and
# general boot tooling only -- the booted kernel/initrd/disk come from the
# bind-mounted $RELEASE_DIR, never the image's own /boot/*):
#   docker run --rm \
#       -e EXPECTED_PRODUCT_VERSION=V0.2 \
#       -v $PWD/tests/qemu/test_release_acceptance_e2e.sh:/test.sh:ro \
#       -v /path/to/cut-release/bundle:/release:ro \
#       --entrypoint bash ovmx-boot /test.sh
# (see tests/qemu/run_release_acceptance_e2e.sh, which cuts/locates the
# bundle and invokes this the same way.)
#
# Env knobs:
#   EXPECTED_PRODUCT_VERSION  REQUIRED. The version the cut's own
#                             release-manifest.json recorded.
#   SKIP_PARTS_SCENARIO       "1" to skip the @PARTS_SETUP/$PARTS/DIRECTORY
#                             steps and go straight from login to the
#                             version check (default "0"; used for a fast
#                             version-only negative-control boot).
#   BOOT_TIMEOUT / SETUP_TIMEOUT / RUN_TIMEOUT   same meaning as
#                             test_parts_demo_e2e.sh.
#
# Exit 0 = the full transcript is correct end to end AND the shipped version
# matches the cut. Exit 1 = a real failure (see the printed transcript
# segment) -- including a version mismatch.

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
SETUP_TIMEOUT="${SETUP_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-1500}"
SKIP_PARTS_SCENARIO="${SKIP_PARTS_SCENARIO:-0}"
EXPECTED_PRODUCT_VERSION="${EXPECTED_PRODUCT_VERSION:-}"
KERNEL=/release/vmlinuz
INITRD=/release/initramfs-ovmx-slim.cpio.gz
DISTRIB_IMG=/release/ovmx-distrib.img
ARCH=$(uname -m)

if [ -z "$EXPECTED_PRODUCT_VERSION" ]; then
    echo "FATAL: EXPECTED_PRODUCT_VERSION is required (the cut's release-manifest.json product_version) -- see header" >&2
    exit 1
fi

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
fi

for f in "$KERNEL" "$INITRD" "$DISTRIB_IMG"; do
    [ -f "$f" ] || { echo "FATAL: $f not found - run this inside the ovmx-boot image with a cut-release.sh bundle bind-mounted at /release (see header)"; exit 1; }
done
command -v "$QEMU" >/dev/null 2>&1 || { echo "FATAL: $QEMU not available"; exit 1; }

PASS=0
FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== OVMX release-acceptance e2e: boot the CUT artifact -> login -> [PARTS demo] -> verify shipped version (vms-a86f) ==="
echo "arch=$ARCH qemu=$QEMU kernel=$KERNEL initrd=$INITRD distrib=$DISTRIB_IMG"
echo "expected product version (from the cut's release-manifest.json): $EXPECTED_PRODUCT_VERSION"
echo "skip_parts_scenario=$SKIP_PARTS_SCENARIO"

DISK=/tmp/release-acceptance-e2e.img
LOG=/tmp/release-acceptance-e2e-console.log
FIFO=/tmp/release-acceptance-e2e-console.in
rm -f "$DISK" "$LOG" "$FIFO"
cp "$DISTRIB_IMG" "$DISK"
mkfifo "$FIFO"

cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; }
trap cleanup EXIT

# shellcheck disable=SC2086
timeout "$((BOOT_TIMEOUT + SETUP_TIMEOUT + RUN_TIMEOUT + 120))" $QEMU $MACHINE \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 512M -smp 2 -nic none -nodefaults -serial stdio \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$FIFO" >"$LOG" 2>&1 &
QPID=$!
exec 4>"$FIFO"

send() { printf '%s\r' "$1" >&4; }
wait_for() {  # pattern  limit-seconds  since-byte
    local pat="$1" limit="${2:-30}" since="${3:-0}" waited=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if tail -c "+$((since + 1))" "$LOG" 2>/dev/null | grep -qF "$pat"; then return 0; fi
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 0.25; waited=$((waited + 1))
    done
    return 1
}
segment_since() { tail -c "+$(($1 + 1))" "$LOG" 2>/dev/null | tr -d '\r'; }
dump_and_die() {
    echo ""
    echo "=== FATAL: $1 ==="
    echo "--- full console log ---"
    cat "$LOG"
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
    exit 1
}

# --- 1. Boot the CUT artifact: distrib.img -> STARTUP.EXE -> login prompt --
if wait_for '%OVMX-I-EXEC' 60; then ok "executive attached (real vms.ko, cut artifact)"; else bad "executive never attached"; fi
send ''  # vms-2213: wake OPA0: — LOGINOUT waits for RETURN before Username:
if wait_for 'Username:' "$BOOT_TIMEOUT"; then
    ok "the CUT ovmx-distrib.img boots to the login prompt"
else
    dump_and_die "boot never reached Username: within ${BOOT_TIMEOUT}s"
fi

# --- 2. Log in as SYSTEM -----------------------------------------------------
LOGIN_OFF=$(wc -c <"$LOG")
send 'SYSTEM'
wait_for 'Password:' 30 "$LOGIN_OFF" && send 'MANAGER'
if wait_for 'Welcome to OpenVMX' 30 "$LOGIN_OFF"; then
    ok "SYSTEM logs in (LOGINOUT.EXE activated, cut artifact)"
else
    dump_and_die "SYSTEM login failed"
fi
wait_for '$' 20 "$LOGIN_OFF"

if [ "$SKIP_PARTS_SCENARIO" != "1" ]; then
    # --- 3. The acceptance scenario: @SYS$UPDATE:PARTS_SETUP.COM ------------
    # Same commands/assertions as tests/qemu/test_parts_demo_e2e.sh -- reused
    # here as the release-acceptance scenario, now against the cut artifact.
    SETUP_OFF=$(wc -c <"$LOG")
    send '@SYS$UPDATE:PARTS_SETUP.COM'
    if wait_for '%PARTS-I-DONE' "$SETUP_TIMEOUT" "$SETUP_OFF"; then
        ok "PARTS_SETUP.COM ran to completion (%PARTS-I-DONE)"
    else
        dump_and_die "PARTS_SETUP.COM did not reach %PARTS-I-DONE within ${SETUP_TIMEOUT}s"
    fi
    SETUP_SEG=$(segment_since "$SETUP_OFF")
    check_setup() { if printf '%s\n' "$SETUP_SEG" | grep -qF "$1"; then ok "$2"; else bad "$2"; fi; }
    check_setup '%PARTS-I-SETUP' "install procedure announced itself"
    check_setup '%PARTS-I-COPY'  "install procedure staged the COPY step"
    check_setup '%PARTS-I-DEFINE, defining PARTS :== $SYS$SYSTEM:PARTS.EXE' \
                "install procedure defined the PARTS foreign command"
    if printf '%s\n' "$SETUP_SEG" | grep -qiE '%COPY-[EF]-|%RMS-[EF]-|%DCL-[EF]-'; then
        bad "PARTS_SETUP.COM's COPY step reported no fatal/error condition"
    else
        ok "PARTS_SETUP.COM's COPY step reported no fatal/error condition"
    fi

    # --- 4. The demo itself: the bare foreign command $ PARTS ----------------
    RUN_OFF=$(wc -c <"$LOG")
    send 'PARTS'
    if wait_for '%PARTS-S-DONE' "$RUN_TIMEOUT" "$RUN_OFF"; then
        ok "PARTS ran to completion under QEMU (%PARTS-S-DONE, cut artifact)"
    else
        dump_and_die "\$ PARTS did not reach %PARTS-S-DONE within ${RUN_TIMEOUT}s"
    fi
    RUN_SEG=$(segment_since "$RUN_OFF")
    check_run() { if printf '%s\n' "$RUN_SEG" | grep -qF "$1"; then ok "$2"; else bad "$2"; fi; }
    check_run '%PARTS-I-BEGIN' "\$ PARTS activated (foreign-command dispatch)"
    check_run '%PARTS-I-CREATE, created indexed file SYS$SCRATCH:PARTS.DAT' \
        "created the RMS indexed file at SYS\$SCRATCH:PARTS.DAT (a VMS filespec, first candidate, no fallback)"
    if printf '%s\n' "$RUN_SEG" | grep -qF '%PARTS-I-TRYCRE'; then
        bad "no fallback attempts (%PARTS-I-TRYCRE absent) -- SYS\$SCRATCH: worked on the first try"
    else
        ok "no fallback attempts (%PARTS-I-TRYCRE absent) -- SYS\$SCRATCH: worked on the first try"
    fi
    if printf '%s\n' "$RUN_SEG" | grep -qF '/tmp/'; then
        bad "no Unix path (/tmp/) anywhere in the PARTS transcript"
    else
        ok "no Unix path (/tmp/) anywhere in the PARTS transcript"
    fi
    check_run '%PARTS-I-LOADED' "loaded records into the indexed file"
    check_run '%PARTS-I-LOOKUP' "ran keyed random lookups (proving indexed, not sequential, access)"
    check_run '%PARTS-S-FOUND'  "a keyed random sys\$get returned a record"
    check_run 'PN000001'        "the first loaded part (PN000001) was found by keyed lookup"
    check_run '%PARTS-W-NOTFOUND, no part with key PN999999' \
        "a key that was never loaded (PN999999) correctly reported NOTFOUND (RMS\$_RNF)"
    if printf '%s\n' "$RUN_SEG" | grep -qF '%PARTS-E-VERIFY'; then
        bad "no wrong-record from a keyed lookup (%PARTS-E-VERIFY absent)"
    else
        ok "no wrong-record from a keyed lookup (%PARTS-E-VERIFY absent)"
    fi

    # --- 5. Independent corroboration: the VMS layer itself sees the file ---
    DIR_OFF=$(wc -c <"$LOG")
    DIR_CMD='DIRECTORY SYS$SCRATCH:PARTS.DAT'
    send "$DIR_CMD"
    wait_for 'Total of' 20 "$DIR_OFF"
    DIR_SEG=$(segment_since "$DIR_OFF")
    DIR_BODY=$(printf '%s\n' "$DIR_SEG" | grep -vF "$DIR_CMD")
    if printf '%s\n' "$DIR_BODY" | grep -qE 'Total of [1-9][0-9]* files?, [0-9]+ blocks' \
        && printf '%s\n' "$DIR_BODY" | grep -qF 'PARTS.DAT'; then
        ok "\$ DIRECTORY SYS\$SCRATCH:PARTS.DAT independently confirms the file exists"
    else
        bad "\$ DIRECTORY SYS\$SCRATCH:PARTS.DAT independently confirms the file exists"
    fi
else
    echo "  (SKIP_PARTS_SCENARIO=1 -- skipping @PARTS_SETUP/\$ PARTS/DIRECTORY, version check only)"
fi

# --- 6. THE VERSION-MATCH ASSERTION -----------------------------------------
# `$ PRODUCT SHOW PRODUCT` against the DEFAULT (currently-running) system
# reports "OVMX <version>" via src/product/product.c's do_show(), which reads
# ovmx_product_version() (src/libvms/include/ovmx_identity.h, INV-1
# single-source). This is the artifact's OWN claim about its own version.
VER_OFF=$(wc -c <"$LOG")
send 'PRODUCT SHOW PRODUCT'
if wait_for 'product found' "$SETUP_TIMEOUT" "$VER_OFF"; then
    ok "PRODUCT SHOW PRODUCT completes"
else
    dump_and_die "PRODUCT SHOW PRODUCT did not report a product count within ${SETUP_TIMEOUT}s"
fi
VER_SEG=$(segment_since "$VER_OFF")
REPORTED_VERSION=$(printf '%s\n' "$VER_SEG" | awk '/^OVMX /{print $2; exit}')

if [ -n "$REPORTED_VERSION" ]; then
    ok "PRODUCT SHOW PRODUCT names a version (parsed: $REPORTED_VERSION)"
else
    dump_and_die "could not parse a version out of PRODUCT SHOW PRODUCT's output: $VER_SEG"
fi

# THE GATE: the shipped artifact's own reported version must equal what the
# cut recorded in release-manifest.json. A mismatch here is exactly the bug
# class this gate exists to catch -- do NOT relax this to a substring/prefix
# match; a mismatch is a real, enforced failure.
if [ "$REPORTED_VERSION" = "$EXPECTED_PRODUCT_VERSION" ]; then
    ok "shipped version ($REPORTED_VERSION) matches the cut's release-manifest.json ($EXPECTED_PRODUCT_VERSION)"
else
    bad "VERSION MISMATCH: PRODUCT SHOW PRODUCT reports '$REPORTED_VERSION' but the cut recorded '$EXPECTED_PRODUCT_VERSION' in release-manifest.json"
fi

# --- 7. NEGATIVE CONTROL: the same comparison, deliberately corrupted ------
# Proves the comparator above is not vacuously true. Uses the SAME real,
# freshly-captured $REPORTED_VERSION -- no synthetic data -- compared
# against a value guaranteed to differ from anything real.
BOGUS_VERSION="${EXPECTED_PRODUCT_VERSION}-NEGCTRL-MISMATCH"
if [ "$REPORTED_VERSION" = "$BOGUS_VERSION" ]; then
    bad "NEGATIVE CONTROL FAILED: the comparator reported a match against a deliberately bogus version ($BOGUS_VERSION) -- the version-match assertion above cannot be trusted"
else
    ok "NEGATIVE CONTROL: the comparator correctly reports a mismatch against a deliberately bogus version ($BOGUS_VERSION), proving assertion #6 can actually go red"
fi

kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""

echo ""
echo "=== transcript: PRODUCT SHOW PRODUCT ==="
printf '%s\n' "$VER_SEG" | sed 's/^/  /'
echo "===================================="
echo "RESULT: $PASS passed, $FAIL failed"
if [ "$FAIL" -eq 0 ]; then
    echo "ALL RELEASE-ACCEPTANCE E2E CHECKS PASSED"
    exit 0
fi
echo ""
echo "--- full console log ---"
cat "$LOG"
exit 1
