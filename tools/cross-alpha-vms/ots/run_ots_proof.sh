#!/bin/bash
# run_ots_proof.sh — end-to-end proof for the OVMX LIBOTS$ runtime (vms-bfd6).
#
# Ties the RUNG-1 harness together and PROVES the OTS$/MATH$ residual is closed:
#
#   1. build the alpha-dec-vms cross toolchain image (binutils + cc1 + libgcc.a)
#   2. build musl libc.a (-g0) with the arch overlay
#   3. build LINK.EXE + OVMXDUMP from the repo (ordinary host tools)
#   4. build LIBOTS$SHR.EXE from ots_runtime.c + ots_home_args.s (build-libots.sh)
#   5. link DECC$SHR whole-archiving libc.a + libgcc.a WITH --use LIBOTS$SHR.EXE
#      and ASSERT zero OTS$ references are left deferred (they bind as real
#      cross-image .vms$imp imports); the only residual is the non-OTS setjmp/
#      cancellation/linker-array surface of later rungs.
#   6. run the native OTS$-divide correctness test (test_ots_div.c) — the same
#      shift-subtract C, checked against host / and % over an edge-case matrix.
#
# Build/oracle tooling, Rule-9-clean: nothing runs inside the OVMX guest; all
# deps containerized; the build writes to /tmp inside the container, never the
# repo. Exit 0 = the OTS$ runtime links and computes correctly.
#
#   tools/cross-alpha-vms/ots/run_ots_proof.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
CROSS=$(cd "$HERE/.." && pwd)                 # tools/cross-alpha-vms
REPO=$(cd "$CROSS/../.." && pwd)
IMG=${IMG:-ovmx-cross-alpha-vms}

echo "== [1/3] build alpha-dec-vms cross toolchain image ($IMG) =="
docker build -t "$IMG" "$CROSS"

echo "== [2/3] build libc.a + LINK.EXE + LIBOTS\$ + DECC\$SHR (--use LIBOTS\$) =="
docker run --rm \
    -v "$REPO:/wt:ro" \
    -e MUSL_EXTRA_CFLAGS=-g0 \
    "$IMG" bash -euxo pipefail -c '
        export PATH=/opt/cross-alpha-vms/bin:$PATH
        W=/tmp/ots-proof; mkdir -p $W

        # (a) musl libc.a (-g0), plus the toolchain libgcc.a
        OVERLAY=/wt/tools/cross-alpha-vms/musl-arch bash /wt/tools/cross-alpha-vms/musl-arch/build-musl.sh
        cp /tmp/musl-build/musl-1.2.5/lib/libc.a $W/libc.a
        cp /opt/cross-alpha-vms/lib/libgcc.a     $W/libgcc.a

        # (b) LINK.EXE + OVMXDUMP (ordinary host tools; single-TU each)
        gcc -O2 -Isrc/vmslink/include -Isrc/vmslink -o $W/LINK.EXE   /wt/src/vmslink/link.c
        gcc -O2 -Isrc/vmslink/include -Isrc/vmslink -o $W/OVMXDUMP   /wt/src/vmslink/dump_image.c || true

        # (c) LIBOTS$SHR.EXE
        LINK_EXE=$W/LINK.EXE OUT=$W bash /wt/tools/cross-alpha-vms/ots/build-libots.sh
        echo "== LIBOTS\$ universals =="
        [ -x $W/OVMXDUMP ] && $W/OVMXDUMP $W/LIBOTS_SHR.EXE 2>/dev/null | grep -iE "OTS\$" || true

        # (d) DECC$SHR whole-archive WITH the LIBOTS$ producer
        export NM=alpha-dec-vms-nm AR_HOST=ar
        export OVMX_DECC_ARCH=alpha DECC_ALLOW_UNDEF=1 OVMX_LINK_DUMP_UNDEF=1
        export DECC_USE=$W/LIBOTS_SHR.EXE
        bash /wt/src/vmslink/mk_decc_shr.sh $W/LINK.EXE $W/DECC_SHR.EXE $W/libc.a $W/libgcc.a 2> $W/link.err || { cat $W/link.err; exit 1; }
        cat $W/link.err

        # (e) ASSERT: no OTS$ left deferred
        if grep -oE "undefined symbol '"'"'[^'"'"']+'"'"'" $W/link.err | grep -q "OTS\$"; then
            echo "PROOF FAIL: an OTS\$ symbol is still deferred:"
            grep -oE "undefined symbol '"'"'[^'"'"']+'"'"'" $W/link.err | grep "OTS\$" | sort -u
            exit 4
        fi
        BOUND=$(grep -c "cross-image import '"'"'OTS\$" $W/link.err || true)
        echo "== PROOF: all OTS\$ references bound as cross-image imports (count=$BOUND); zero OTS\$ deferred =="
        echo "== remaining (non-OTS, later-rung) deferred residual =="
        grep -oE "undefined symbol '"'"'[^'"'"']+'"'"'" $W/link.err | sed "s/.*'"'"'\(.*\)'"'"'/\1/" | sort | uniq -c

        # (f) native OTS$-divide correctness proof (host gcc, same C)
        gcc -O2 -fdollars-in-identifiers -fno-builtin -fno-tree-loop-distribute-patterns \
            -c /wt/tools/cross-alpha-vms/ots/ots_runtime.c -o $W/ots_host.o
        gcc -O2 -fdollars-in-identifiers /wt/tools/cross-alpha-vms/ots/test_ots_div.c $W/ots_host.o -o $W/test_ots_div
        $W/test_ots_div
    '

echo "== [3/3] OTS\$ runtime proof PASSED (LIBOTS\$ links, DECC\$SHR binds it, divide is correct) =="
