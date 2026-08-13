#!/bin/sh
# mk_tcc_static.sh -- build TCC.EXE as a PLAIN STATIC executable that DCL can
# activate as a foreign command by fork+execve (self-host spine #6, bead
# vms-d1b).
#
# WHY A SECOND TCC BUILD RECIPE (distinct from src/vmslink/mk_tcc.sh).
# mk_tcc.sh links tinycc with LINK.EXE --executable into an IMGACT-packaged OVMX
# image (PT_INTERP=IMGACT.EXE, .vms$sv symbol vector) whose primary-source read
# and object write route through OVMX RMS (OVMX_RMS_IO). That is the right shape
# for the in-OVMX self-host FIXPOINT proof (run_tcc_selfhost.sh), but it is NOT
# how the MMK-driven build reaches the compiler in QEMU: MMK spawns a persistent
# DCL over mailboxes and streams `TCC ...` command lines into it, and DCL
# activates a foreign command by fork()+execve() of the image path
# (dcl_cmd_process.c dcl_exec_foreign_command -> dcl_activate_image) -- exactly
# as it already does for the staged MMK.EXE/DCL.EXE. A plain static binary is
# therefore all that path needs (no IMGACT/shareable staging), identically to
# how MMK.EXE itself is staged into the QEMU initramfs.
#
# This recipe builds tinycc with its OWN unmodified build system (configure +
# `make tcc`), statically linked, with NO OVMX_RMS_IO seam: tcc reads its .c and
# writes its .OBJ through plain musl file I/O against the guest's tmpfs work
# directory. That is correct for THIS proof -- the authenticity crux (Rule 9 /
# INV-6) is that MMK DRIVES the toolchain over a real executive's mailboxes, not
# that tcc's private file reads go through RMS (the file the driven build touches
# is a scratch object on the SYSDISK, not an executive facility).
#
# CLEAN-ROOM (Rule 8): tinycc is vendored LGPL upstream source (third-party/tcc,
# VENDOR-REV) -- not a VMS format. No VMS byte layout is involved here.
#
# Usage:  mk_tcc_static.sh <out-TCC.EXE> [tcc-src-dir]
# Env:    CC (default gcc). The QEMU Dockerfile passes CC=musl-gcc so TCC.EXE is
#         a static-musl binary, the same libc mode as the staged MMK.EXE/DCL.EXE.
set -e

OUT=${1:?usage: mk_tcc_static.sh <out-TCC.EXE> [tcc-src-dir]}
HERE=$(cd "$(dirname "$0")" && pwd)                       # tests/toolchain
REPO=$(cd "$HERE/../.." && pwd)                           # repo root
TCC_SRC=${2:-$REPO/third-party/tcc/src}
CC=${CC:-gcc}

[ -d "$TCC_SRC" ] || { echo "mk_tcc_static: tcc src dir not found: $TCC_SRC"; exit 1; }
[ -f "$TCC_SRC/configure" ] || { echo "mk_tcc_static: tinycc configure not found in $TCC_SRC"; exit 1; }

WORK=${WORK:-/tmp/mk-tcc-static}
rm -rf "$WORK"
mkdir -p "$WORK"

echo "mk_tcc_static: configure (host tool; writes config.mak/config.h to CWD, so cd \$WORK)"
( cd "$WORK" && sh "$TCC_SRC/configure" --source-path="$TCC_SRC" --cc="$CC" ) \
    >"$WORK/configure.log" 2>&1 \
    || { echo "mk_tcc_static: configure failed:"; tail -20 "$WORK/configure.log"; exit 1; }
[ -f "$WORK/config.h" ] || { echo "mk_tcc_static: configure produced no config.h"; exit 1; }

# `make tcc` builds JUST the compiler driver (tinycc's own native, multi-TU,
# non-ONE_SOURCE shape); LDFLAGS=-static makes it a plain static binary. We do
# NOT `make` the whole tree: lib/libtcc1.a's optional bounds-checker object does
# not build against modern musl (VENDOR-REV documents this) and is not needed --
# `tcc -c` compiles without it.
echo "mk_tcc_static: make tcc LDFLAGS=-static (CC=$CC)"
( cd "$WORK" && make tcc LDFLAGS="-static" ) >"$WORK/make.log" 2>&1 \
    || { echo "mk_tcc_static: make tcc failed:"; tail -30 "$WORK/make.log"; exit 1; }
[ -x "$WORK/tcc" ] || { echo "mk_tcc_static: make produced no tcc binary"; exit 1; }

# Prove it is genuinely static -- a dynamic TCC.EXE would fail to activate in the
# initramfs (no dynamic linker present), silently, exactly as the comment on the
# staged MMK.EXE/DCL.EXE warns.
if command -v file >/dev/null 2>&1; then
    file "$WORK/tcc" | grep -q "statically linked" \
        || { echo "mk_tcc_static: FAIL: tcc is not statically linked:"; file "$WORK/tcc"; exit 1; }
fi
# `ldd` on a static binary prints "not a dynamic executable" (or errors) -- either
# way it must NOT list shared-object dependencies.
if command -v ldd >/dev/null 2>&1; then
    if ldd "$WORK/tcc" 2>/dev/null | grep -qE '=>|\.so'; then
        echo "mk_tcc_static: FAIL: tcc has dynamic dependencies:"; ldd "$WORK/tcc"; exit 1
    fi
fi

mkdir -p "$(dirname "$OUT")"
cp "$WORK/tcc" "$OUT"
chmod +x "$OUT"
echo "mk_tcc_static: created $OUT ($(wc -c < "$OUT") bytes, static)"
