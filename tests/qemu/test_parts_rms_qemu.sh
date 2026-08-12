#!/bin/bash
# test_parts_rms_qemu.sh - end-to-end proof that PARTS (the OVMX 0.2 RMS
# indexed-file demo, beads vms-e97 / vms-f20) builds through the VMS-native
# toolchain and RUNS under the real QEMU runtime, producing correct keyed
# lookups.
#
# What it proves (the real DoD - "runs and outputs under QEMU", not just links):
#   1. PARTS.EXE is built by cc -> LINK.EXE (.vms$sv symbol vector,
#      PT_INTERP=IMGACT.EXE, zero DT_NEEDED/DT_HASH) - the same native toolchain
#      DCL.EXE uses (mk_parts.sh, modelled on mk_dcl.sh).
#   2. Staged into the fat initramfs SYS$SYSTEM: and booted under QEMU with the
#      real vms.ko executive, a non-root SYSTEM DCL session activates it via
#      IMGACT and $ RUN PARTS creates an RMS indexed file, loads records, and
#      does keyed random sys$get lookups that print the correct records.
#
# It does NOT touch distro mastering (bead vms-cde owns shipping PARTS in the
# image): it repacks the ALREADY-BUILT initramfs at run time to add PARTS.EXE,
# exactly the pattern tests/qemu/inject_and_run.sh uses.
#
# Runs on the HOST (needs docker + qemu-system-x86_64 + cpio + gzip). CI runs
# the same way the other QEMU gates do.
#
# Env knobs:
#   OVMX_BOOT_IMAGE   full bootable image tag   (default: ovmx-boot, built if absent)
#   OVMX_LN_IMAGE     link-native stage tag     (default: ovmx-parts-ln, built if absent)
#   PARTS_QEMU_COUNT  records the demo loads     (default: 1000 - a speed knob for
#                     the gate; the shipped $ RUN PARTS default is larger)
#   KEEP_BUILD=1      reuse existing image tags without rebuilding
#
# Exit 0 = PARTS ran under QEMU and printed correct keyed lookups.
set -u

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BOOT_IMAGE="${OVMX_BOOT_IMAGE:-ovmx-boot}"
LN_IMAGE="${OVMX_LN_IMAGE:-ovmx-parts-ln}"
COUNT="${PARTS_QEMU_COUNT:-1000}"
DOCKERFILE="distro/Dockerfile.bootable"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-180}"

ARCH=$(uname -m)
if [ "$ARCH" != "x86_64" ]; then
    echo "SKIP: this gate is x86_64-only (LINK.EXE native graph is built for x86_64 here); arch=$ARCH"
    exit 0
fi
for tool in docker qemu-system-x86_64 cpio gzip; do
    command -v "$tool" >/dev/null 2>&1 || { echo "SKIP: $tool not available"; exit 0; }
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/parts-qemu.XXXXXX") || exit 1
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

echo "=== PARTS RMS indexed-file: native build + RUN under QEMU (vms-e97/vms-f20) ==="
echo "repo=$REPO_ROOT  count=$COUNT"

have_image() { docker image inspect "$1" >/dev/null 2>&1; }

if [ "${KEEP_BUILD:-0}" = "1" ] && have_image "$BOOT_IMAGE" && have_image "$LN_IMAGE"; then
    echo "--- reusing existing images ($BOOT_IMAGE, $LN_IMAGE) ---"
else
    echo "--- building bootable image ($BOOT_IMAGE) ---"
    docker build -t "$BOOT_IMAGE" -f "$REPO_ROOT/$DOCKERFILE" "$REPO_ROOT" >"$WORK/boot-build.log" 2>&1 \
        || { echo "FAIL: bootable image build"; tail -30 "$WORK/boot-build.log"; exit 1; }
    echo "--- building link-native stage ($LN_IMAGE) ---"
    docker build --target link-native -t "$LN_IMAGE" -f "$REPO_ROOT/$DOCKERFILE" "$REPO_ROOT" >"$WORK/ln-build.log" 2>&1 \
        || { echo "FAIL: link-native build"; tail -30 "$WORK/ln-build.log"; exit 1; }
fi

# --- 1. Build PARTS.EXE via the native toolchain (cc -> LINK.EXE) -------------
echo "--- building PARTS.EXE (cc -> LINK.EXE, .vms\$sv, IMGACT interp) ---"
# The current worktree's PARTS sources are bind-mounted over the copy baked into
# the link-native image, so the gate always builds the tree under test (and dev
# iteration does not require rebuilding the whole native graph).
docker run --rm -v "$WORK":/out -v "$REPO_ROOT/src/apps/parts":/parts-src:ro "$LN_IMAGE" sh -c '
    set -e
    SYSLIB=/tmp/build/link-native/SYSLIB
    CFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mtls-dialect=gnu2 -DPARTS_DEFAULT_COUNT='"$COUNT"'" \
      sh /parts-src/mk_parts.sh /tmp/build/bin/LINK.EXE /out/PARTS.EXE \
      "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" /parts-src /src/src
' >"$WORK/mk.log" 2>&1 || { echo "FAIL: PARTS.EXE native build"; cat "$WORK/mk.log"; exit 1; }
[ -s "$WORK/PARTS.EXE" ] || { echo "FAIL: PARTS.EXE not produced"; cat "$WORK/mk.log"; exit 1; }
grep -F "%LINK-S-CREATED" "$WORK/mk.log" | sed 's/^/    /'

# --- 1b. Ground-source: PARTS.EXE must be a VMS-native image -----------------
readelf -h "$WORK/PARTS.EXE" | grep -q "Machine:.*X86-64" || { echo "FAIL: PARTS.EXE not EM_X86_64"; exit 1; }
readelf -l "$WORK/PARTS.EXE" | grep -q INTERP        || { echo "FAIL: PARTS.EXE has no PT_INTERP (IMGACT)"; exit 1; }
N=$(readelf -d "$WORK/PARTS.EXE" 2>/dev/null | grep -c NEEDED)
H=$(readelf -d "$WORK/PARTS.EXE" 2>/dev/null | grep -c HASH)
[ "$N" -eq 0 ] && [ "$H" -eq 0 ] || { echo "FAIL: PARTS.EXE has DT_NEEDED/DT_HASH (ld-linked, not VMS-native)"; exit 1; }
readelf -S "$WORK/PARTS.EXE" | grep -q 'vms\$sv'     || { echo "FAIL: PARTS.EXE has no .vms\$sv-related section"; exit 1; }
echo "    OK: PARTS.EXE is VMS-native (EM_X86_64, PT_INTERP=IMGACT, zero DT_NEEDED/DT_HASH, .vms\$ sections)"

# --- 2. Extract kernel + fat initramfs from the bootable image ---------------
echo "--- extracting kernel + fat initramfs ---"
CID=$(docker create "$BOOT_IMAGE") || { echo "FAIL: docker create"; exit 1; }
docker cp "$CID":/boot/vmlinuz "$WORK/vmlinuz" >/dev/null 2>&1
docker cp "$CID":/boot/initramfs-ovmx.cpio.gz "$WORK/initramfs.cpio.gz" >/dev/null 2>&1
docker rm "$CID" >/dev/null 2>&1
[ -s "$WORK/vmlinuz" ] && [ -s "$WORK/initramfs.cpio.gz" ] || { echo "FAIL: could not extract boot artifacts"; exit 1; }

# --- 3. Repack the initramfs with PARTS.EXE in SYS$SYSTEM: --------------------
# Same run-time repack pattern as tests/qemu/inject_and_run.sh - the distro
# mastering (Dockerfile.bootable) is NOT modified.
echo "--- staging PARTS.EXE into initramfs SYS\$SYSTEM: and repacking ---"
IRD="$WORK/ird"; mkdir -p "$IRD"
( cd "$IRD" && gzip -dc "$WORK/initramfs.cpio.gz" | cpio -idm >/dev/null 2>&1 ) \
    || { echo "FAIL: unpack initramfs"; exit 1; }
SYSEXE_DIR="$IRD/vms/SYS0/SYSCOMMON/SYSEXE"
[ -d "$SYSEXE_DIR" ] || { echo "FAIL: SYSEXE dir not found in initramfs"; exit 1; }
[ -f "$SYSEXE_DIR/DCL.EXE" ] || { echo "FAIL: DCL.EXE not in initramfs (unexpected layout)"; exit 1; }
install -m 0755 "$WORK/PARTS.EXE" "$SYSEXE_DIR/PARTS.EXE"
( cd "$IRD" && find . | cpio -o -H newc 2>/dev/null | gzip > "$WORK/initramfs-parts.cpio.gz" ) \
    || { echo "FAIL: repack initramfs"; exit 1; }
echo "    staged PARTS.EXE ($(stat -c%s "$WORK/PARTS.EXE") bytes); repacked initramfs $(stat -c%s "$WORK/initramfs-parts.cpio.gz") bytes"

# --- 4. Boot QEMU and drive a real SYSTEM DCL session ------------------------
echo "--- booting QEMU (fat initramfs + PARTS.EXE), driving SYSTEM session ---"
DISK="$WORK/sysdisk.img"; truncate -s 64M "$DISK"
LOG="$WORK/console.log"; FIFO="$WORK/console.in"; mkfifo "$FIFO"

timeout "$QEMU_TIMEOUT" qemu-system-x86_64 \
    -kernel "$WORK/vmlinuz" \
    -initrd "$WORK/initramfs-parts.cpio.gz" \
    -nographic \
    -append "console=ttyS0 loglevel=3 quiet" \
    -m 512M -smp 2 -nic none -nodefaults \
    -serial stdio \
    -drive file="$DISK",format=raw,if=virtio \
    -no-reboot \
    <"$FIFO" >"$LOG" 2>&1 &
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

PASS=0; FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if wait_for '%OVMX-I-EXEC' 60; then ok "executive attached (real vms.ko)"; else bad "executive never attached"; fi
send ''  # vms-2213: wake OPA0: — LOGINOUT waits for RETURN before Username:
if wait_for 'Username:' 60; then ok "login prompt"; else bad "no login prompt"; fi
send 'SYSTEM'
wait_for 'Password:' 30 && send 'MANAGER'
if wait_for 'Welcome to OpenVMX' 30; then ok "SYSTEM login (LOGINOUT.EXE activated)"; else bad "SYSTEM login failed"; fi
wait_for '$' 20

RUN_OFF=$(wc -c <"$LOG")
send 'RUN SYS$SYSTEM:PARTS'
# Wait for the demo to finish (or fail visibly). Generous - the load loop runs
# under emulation.
if wait_for '%PARTS-S-DONE' "$QEMU_TIMEOUT" "$RUN_OFF"; then
    ok "PARTS ran to completion under QEMU (%PARTS-S-DONE)"
else
    bad "PARTS did not reach %PARTS-S-DONE"
fi

SEG=$(tail -c "+$((RUN_OFF + 1))" "$LOG" 2>/dev/null | tr -d '\r')

check_seg() { if printf '%s\n' "$SEG" | grep -qF "$1"; then ok "$2"; else bad "$2"; fi; }
check_seg '%PARTS-I-CREATE' "created an RMS indexed file"
check_seg '%PARTS-I-LOADED' "loaded records into the indexed file"
check_seg '%PARTS-S-FOUND' "keyed random sys\$get returned a record"
check_seg 'PN000001' "printed the first part's record"

# Correctness / honesty guards.
if printf '%s\n' "$SEG" | grep -qF '%PARTS-E-VERIFY'; then bad "a keyed lookup returned the WRONG record"; else ok "no wrong-record (%PARTS-E-VERIFY absent)"; fi
if printf '%s\n' "$SEG" | grep -qF '%PARTS-F-'; then bad "PARTS reported a fatal error"; else ok "no PARTS fatal error"; fi
# vms-17f9: if cmd_run surfaced a crash/nonzero exit, the RUN did not really work.
if printf '%s\n' "$SEG" | grep -qiE '%DCL-[EF]-ABORT|terminated abnormally'; then bad "DCL RUN surfaced an abnormal image termination"; else ok "image activated and exited cleanly (no DCL abort)"; fi

kill "$QPID" 2>/dev/null; QPID=""

echo ""
echo "=== transcript (RUN PARTS segment) ==="
printf '%s\n' "$SEG" | grep -E '%PARTS|PN00|%DCL' | sed 's/^/  /'
echo "======================================"
echo "RESULT: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && { echo "ALL PARTS QEMU CHECKS PASSED"; exit 0; }
echo ""
echo "--- full console log ---"; cat "$LOG"
exit 1
