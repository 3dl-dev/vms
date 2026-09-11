#!/bin/sh
# vms-7b7 negctl: LINK.EXE must WARN (%LINK-W-CALLDEFER) when a deferred external
# under --allow-undefined is reached by a CALL-site reloc (rel32 CALL/JMP -> a
# latent activation crash: the call parks rel32=0), and stay SILENT for a
# never-called (data/reference-only) deferred external (a legitimate import).
# Enforcement bundles its proof: this proves the gate FIRES on the dangerous case
# AND ALLOWS the safe case (a gate that can't distinguish is a fake gate).
set -u
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd); LINKDIR=$(cd "$HERE/.." && pwd)
REPO=$(cd "$LINKDIR/../.." && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT

# Host LINK.EXE = the CMake `vmslink` target: add_executable(vmslink link.c),
# target_include_directories PRIVATE include, -DIMGACT_INTERP_PATH=... . link.c
# is a single TU (it #includes evax_read.c). Mirror those flags here.
"$CC" -O2 -DIMGACT_INTERP_PATH=/run/ovmx-boot/IMGACT.EXE \
  -I"$LINKDIR/include" -I"$REPO/src/libvms/include" \
  -o "$W/LINK.EXE" "$LINKDIR/link.c" \
  || { echo "FAIL: could not build host LINK.EXE from link.c"; exit 1; }

# Fixture 1: a CALL to an undefined function -> a PLT32/CALL-site reloc against
# an undefined external (becomes a CALLED deferred external under --allow-undef).
printf 'extern void ovmx_missing_call(void);\nvoid f(void){ ovmx_missing_call(); }\nvoid _start(void){ f(); }\n' > "$W/call.c"
"$CC" -c -o "$W/call.o" "$W/call.c" || { echo "FAIL: build call.o"; exit 1; }
# Fixture 2: only a DATA reference to an undefined symbol -> NOT a call reloc.
printf 'extern int ovmx_missing_data;\nint *ovmx_p = &ovmx_missing_data;\nvoid _start(void){}\n' > "$W/data.c"
"$CC" -c -o "$W/data.o" "$W/data.c" || { echo "FAIL: build data.o"; exit 1; }

pass=0; fail=0
chk(){ if eval "$2"; then echo "  PASS: $1"; pass=$((pass+1)); else echo "  FAIL: $1"; fail=$((fail+1)); fi; }

# Link as a --shareable (the minimal mode that reaches reloc-apply without a --use
# producer): --symbol-vector + --gsmatch, per link.c usage. The undefined external
# becomes a deferred import; reloc-apply then classifies call-vs-reference.
# CALLED-deferred fixture: expect %LINK-W-CALLDEFER naming the symbol.
"$W/LINK.EXE" --shareable --symbol-vector "f=PROCEDURE" --gsmatch LEQUAL,1,0 \
    --allow-undefined -o "$W/CALL.EXE" "$W/call.o" > "$W/call.out" 2>&1 || true
echo "--- CALLED-deferred link output (full) ---"; cat "$W/call.out"
chk "CALLED deferred external fires %LINK-W-CALLDEFER"      "grep -q 'CALLDEFER' '$W/call.out'"
chk "  ...and names the called symbol (ovmx_missing_call)"  "grep -q 'ovmx_missing_call' '$W/call.out'"

# never-called-deferred fixture (data reference only): expect NO CALLDEFER.
"$W/LINK.EXE" --shareable --symbol-vector "ovmx_p=DATA" --gsmatch LEQUAL,1,0 \
    --allow-undefined -o "$W/DATA.EXE" "$W/data.o" > "$W/data.out" 2>&1 || true
echo "--- never-called-deferred link output (full) ---"; cat "$W/data.out"
chk "never-called deferred ref does NOT fire CALLDEFER"     "! grep -q 'CALLDEFER' '$W/data.out'"

echo "=== calldefer gate: $pass passed, $fail failed ==="
[ "$fail" -eq 0 ]
