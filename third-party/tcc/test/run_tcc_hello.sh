#!/bin/sh
# run_tcc_hello.sh — S2 FOUNDATION proof (bead vms-4ba.1, epic vms-4ba/S2):
# vendor a stock, unmodified tinycc (pinned rev — see ../VENDOR-REV), build a
# Linux-hosted tcc binary with tinycc's own aarch64 backend, and prove it
# actually compiles a real C source file to an aarch64 ET_REL object AND that
# the object is usable (gcc-linked executable runs and prints).
#
# Mirrors src/imgact/test/run_dcl_native.sh's pattern (build-to-/tmp, no repo
# writes, hard FAIL on any DONE-condition miss). NO OVMX integration here —
# this only proves we have a working aarch64 tcc to build vms-4ba.2+ on.
#
# DONE conditions (both proven by a REAL run, nothing mocked):
#   1. `tcc -c hello.c -o hello.o` -> ELF64 / REL (Relocatable) / AArch64
#   2. gcc-linked hello.o -> runs, prints its message, exits 0
#
# Native aarch64 build only (tinycc's ./configure auto-selects the arm64
# backend from `uname -m`) — run inside the project's arm64 musl Alpine
# container (see epic vms-4ba / CLAUDE.md build-environment section), never
# on the bare host.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)      # third-party/tcc/test
TCC_DIR=$(cd "$HERE/.." && pwd)          # third-party/tcc
CC=${CC:-gcc}
WORK=${WORK:-/tmp/tcc-hello}
rm -rf "$WORK"; mkdir -p "$WORK"

case "$(uname -m)" in
    aarch64) ;;
    *) echo "SKIP-FAIL: run_tcc_hello.sh needs a native aarch64 host (got $(uname -m)) — tinycc's configure auto-selects TCC_TARGET_ARM64 from uname -m. Run in the arm64 musl Alpine container per CLAUDE.md."; exit 1 ;;
esac

echo "== build stock tcc from the vendored source (third-party/tcc/src) =="
"${MAKE:-make}" -C "$TCC_DIR" CC="$CC" BUILD="$WORK/build" clean >/dev/null 2>&1 || true
"${MAKE:-make}" -C "$TCC_DIR" CC="$CC" BUILD="$WORK/build"
TCC="$WORK/build/tcc"
[ -x "$TCC" ] || { echo "FAIL: tcc binary not produced at $TCC"; exit 1; }
"$TCC" -v

echo
echo "== tcc -c hello.c -o hello.o (stock arm64 backend) =="
"$TCC" -c "$HERE/hello.c" -o "$WORK/hello.o"
[ -f "$WORK/hello.o" ] || { echo "FAIL: hello.o not produced"; exit 1; }

echo
echo "== readelf -h hello.o (DONE condition 1) =="
readelf -h "$WORK/hello.o" | tee "$WORK/hello.readelf"
grep -q 'Class:.*ELF64' "$WORK/hello.readelf" || { echo "FAIL: hello.o is not ELF64"; exit 1; }
grep -q 'Type:.*REL (Relocatable file)' "$WORK/hello.readelf" || { echo "FAIL: hello.o is not ET_REL"; exit 1; }
grep -q 'Machine:.*AArch64' "$WORK/hello.readelf" || { echo "FAIL: hello.o is not EM_AARCH64"; exit 1; }
echo "-- hello.o confirmed ELF64 / REL / AArch64 --"

echo
echo "== gcc-link hello.o and run it (DONE condition 2) =="
$CC -o "$WORK/hello" "$WORK/hello.o"
set +e
"$WORK/hello" > "$WORK/hello.out" 2>&1
RC=$?
set -e
echo "-- program output: --"; sed 's/^/   /' "$WORK/hello.out"
echo "exit code = $RC"
grep -q 'hello from tcc-built aarch64' "$WORK/hello.out" || { echo "FAIL: hello did not print the expected message"; exit 1; }
[ "$RC" -eq 0 ] || { echo "FAIL: gcc-linked hello did not exit clean (got $RC)"; exit 1; }

echo
echo "S2 FOUNDATION (vms-4ba.1): stock vendored tcc ($(cat "$TCC_DIR/VENDOR-REV" | sed -n 's/^Pinned commit:\s*//p')) builds"
echo "on Linux with its own aarch64 backend, compiles hello.c to a real ELF64/REL/"
echo "AArch64 object, and that object links (via gcc) and runs correctly. No OVMX"
echo "integration yet — this is the base for vms-4ba.2 (reloc-gap analysis)."
