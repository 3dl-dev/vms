#!/bin/bash
# test_ovmx_ld.sh -- the argument mapping of ovmx-ld (collect2's ld -> OVMX
# LINK.EXE), checked against a stand-in LINK.EXE that records its argv.
# Host-only. Exit 0 = success.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d)
S="$W/sysroot"; mkdir -p "$S"
cat > "$S/LINK.EXE" <<'SH'
#!/bin/bash
printf '%s\n' "$@" > "$OVMX_LD_TEST_ARGS"
SH
chmod +x "$S/LINK.EXE"
touch "$S/DECC\$SHR.EXE" "$S/LIBOTS_SHR.EXE" "$S/STARLET.a" "$W/a.o" "$W/libcommon.a" "$W/b.obj" "$W/libm.a"
export OVMX_ALPHA_SYSROOT="$S" OVMX_LD_TEST_ARGS="$W/args"
fails=0
check() { if eval "$1"; then echo "  PASS $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

bash "$HERE/ovmx-ld" -o "$W/out.exe" "$W/a.o" "$W/libcommon.a" "$W/b.obj" -L"$W" -lm
mapfile -t A < "$W/args"
pos() { local i; for i in "${!A[@]}"; do [ "${A[$i]}" = "$1" ] && { echo "$i"; return; }; done; echo -1; }
pa=$(pos "$W/a.o"); pl=$(pos "$W/libcommon.a"); pb=$(pos "$W/b.obj")
check '[ "$pl" -gt 0 ] && [ "${A[$((pl-1))]}" = "--library" ]' "a positional archive is passed as --library (searched, not whole-archived)"
check '[ "$pa" -lt "$pl" ] && [ "$pl" -lt "$pb" ]' "it keeps its command-line position between the objects"
pm=$(pos "$W/libm.a")
check '[ "$pm" -gt 0 ] && [ "${A[$((pm-1))]}" = "--library" ]' "-lm found on -L is searched too"
check '[ "${A[$(( ${#A[@]} - 1 ))]}" = "$S/STARLET.a" ]' "STARLET is searched last"
rm -rf "$W"
[ "$fails" -eq 0 ] && echo "ALL OVMX-LD ARGUMENT-MAPPING CHECKS PASSED"
