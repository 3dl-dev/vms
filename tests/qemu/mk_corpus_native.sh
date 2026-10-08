#!/bin/sh
# mk_corpus_native.sh - build the corpus programs listed in corpus_runtime_native.txt
# as VMS-native images (rd vms-db7): each source compiled by OVMX's tcc (TCC.EXE,
# which honours DEC C's #pragma extern_model), the objects linked by LINK.EXE
# --executable --use DECC$SHR. The image's PT_INTERP is IMGACT.EXE at its SYS$SYSTEM
# path (/vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE, staged in the initramfs), and IMGACT
# resolves DECC$SHR from SYS$LIBRARY on the mounted system disk.
#
# Output: the image <repo>/build-static/native/<NAME>.EXE, which the Dockerfile
# masters onto the corpus system disk as [SYS0.SYSCOMMON.SYSEXE]<NAME>.EXE and
# stages at /vms/SYS0/SYSCOMMON/SYSEXE/<NAME>.EXE (IMGACT reads a main image's
# bytes over the executive ACP from SYS$SYSDEVICE, never the POSIX copy), plus
# <repo>/build-static/bin/corpus_rt_<name> (+ .args): a two-line sh wrapper that
# execs that SYS$SYSTEM path, which the corpus staging glob copies into the
# guest's /tests/corpus_rt like every other entry.
#
# Usage: mk_corpus_native.sh <repo-root>
set -eu
REPO=${1:?usage: mk_corpus_native.sh <repo-root>}
BIN=$REPO/build-static/bin
TCC=$BIN/TCC.EXE
DECC="$BIN/DECC\$SHR.EXE"
LIST=$REPO/tests/qemu/corpus_runtime_native.txt
W=${WORK:-/tmp/corpus-native}
NATIVE=$REPO/build-static/native
rm -rf "$W" "$NATIVE"; mkdir -p "$W/vmsinc" "$NATIVE"
[ -x "$TCC" ] || { echo "mk_corpus_native: FATAL: no $TCC" >&2; exit 1; }
[ -f "$DECC" ] || { echo "mk_corpus_native: FATAL: no $DECC" >&2; exit 1; }

# LINK.EXE with its default PT_INTERP (the SYS$SYSTEM IMGACT.EXE path; the build-static
# LINK.EXE bakes the bootable /run/ovmx-boot path instead).
${CC:-gcc} -std=gnu11 -O2 -I"$REPO/src/vmslink/include" -o "$W/LINK.EXE" "$REPO/src/vmslink/link.c"
MUSL_INC=$(echo /usr/include/*-linux-musl)
# The two DEC C headers the corpus uses that musl does not have.
cp "$REPO/src/libvms/include/unixlib.h" "$REPO/src/libvms/include/reentrancy.h" "$W/vmsinc/"

n=0
grep -E '^[A-Za-z0-9_]+\|' "$LIST" | while IFS='|' read -r name dir defs srcs args; do
    objs=""
    dflags=""
    for d in $(echo "$defs" | tr ',' ' '); do dflags="$dflags -D$d"; done
    for s in $(echo "$srcs" | tr ',' ' '); do
        o="$W/$name-${s%.c}.o"
        # shellcheck disable=SC2086
        "$TCC" -nostdinc -I"$REPO/third-party/tcc/src/include" -I"$MUSL_INC" -I"$W/vmsinc" \
            $dflags -c -o "$o" "$REPO/tests/corpus/$dir/$s" \
            || { echo "mk_corpus_native: FATAL: TCC.EXE did not compile $dir/$s" >&2; exit 1; }
        objs="$objs $o"
    done
    # shellcheck disable=SC2086
    img=$(echo "$name" | tr 'a-z' 'A-Z').EXE
    "$W/LINK.EXE" --executable --use "$DECC" -o "$NATIVE/$img" $objs \
        || { echo "mk_corpus_native: FATAL: LINK.EXE did not link $name" >&2; exit 1; }
    chmod +x "$NATIVE/$img"
    printf '#!/bin/sh\nexec /vms/SYS0/SYSCOMMON/SYSEXE/%s "$@"\n' "$img" > "$BIN/corpus_rt_$name"
    chmod +x "$BIN/corpus_rt_$name"
    if [ -n "$args" ]; then printf "%s\n" "$args" > "$BIN/corpus_rt_$name.args"; fi
    echo "mk_corpus_native: built corpus_rt_$name ($srcs)"
done
