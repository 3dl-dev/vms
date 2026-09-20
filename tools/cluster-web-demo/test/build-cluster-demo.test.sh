#!/bin/sh
# build-cluster-demo.test.sh - ground-source proof (rd vms-f0f) that the
# per-release generator (1) actually injects Node A's real cluster identity
# onto a GENUINE ODS-2 fixture (reusing the same vmsfs_master fixture recipe
# as inject-ods2-config.test.sh -- no parallel ODS-2 tooling), (2) assembles a
# deployable bundle carrying the page + injected artifacts + manifest, and
# (3) is DETERMINISTIC: two runs against byte-identical inputs produce a
# byte-identical bundle (same file set, same file hashes).
#
# This does NOT exercise a real multi-GB release build or a real VAX/VMS
# image (that needs a >=48GB host and real release artifacts, per
# heavy-alpha-runtime-on-k3s-worker); it proves the orchestration + the real
# injector path against a small genuine fixture.
#
# Requires: cc, qemu-img, python3 (present on the dev host; no installs).

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
DEMO=$(cd "$HERE/.." && pwd)
REPO=$(cd "$DEMO/../.." && pwd)
ODS2_DIR="$REPO/src/vmsfs/ods2"
ODS2_INC="$REPO/src/vmsfs/include"
CC=${CC:-cc}
PYTHON=${PYTHON:-python3}

for t in "$CC" qemu-img "$PYTHON"; do
    command -v "$t" >/dev/null 2>&1 || { echo "SKIP: $t not on PATH"; exit 0; }
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/f0f-generator-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

echo "1. building the genuine ODS-2 fixture (vmsfs_master, same recipe as inject-ods2-config.test.sh)"
ODS2_SRCS="$ODS2_DIR/ods2_reader.c $ODS2_DIR/ods2_writer.c $ODS2_DIR/ods2_edit.c \
$ODS2_DIR/ods2_bdev.c $ODS2_DIR/ods2_path.c $ODS2_DIR/ods2_block_posix.c"
# shellcheck disable=SC2086
"$CC" -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -O2 -I "$ODS2_INC" \
    "$REPO/tools/vmsfs_master.c" $ODS2_SRCS -o "$WORK/vmsfs_master"
OVMX_MASTER_ODS2=1 "$WORK/vmsfs_master" --ods2 master \
    "$WORK/stock.raw" OVMXSYS "$REPO/distro/rootfs/vms" 128 >/dev/null
qemu-img convert -O qcow2 "$WORK/stock.raw" "$WORK/stock-sysdisk.qcow2"

echo "2. fabricating a minimal (fake, non-booting) initramfs + vmlinuz stand-in"
mkdir -p "$WORK/initramfs-root/etc/ovmx"
( cd "$WORK/initramfs-root" && find . | cpio -o -H newc --quiet | gzip -n ) > "$WORK/stock-initramfs.cpio.gz"
echo "not a real kernel -- generator orchestration fixture only" > "$WORK/stock-vmlinuz"

echo "3. fabricating a minimal site-dir stand-in (demo/cluster/{page files,boot/}, assets/site.css)"
SITE="$WORK/site"
mkdir -p "$SITE/demo/cluster/lib" "$SITE/demo/cluster/boot" "$SITE/assets"
for f in index.html node.html node-pcjs.html node-worker.js coi-serviceworker.js; do
    echo "<!-- fixture $f -->" > "$SITE/demo/cluster/$f"
done
echo "// fixture hub.mjs" > "$SITE/demo/cluster/lib/hub.mjs"
echo "fixture-wasm-bytes" > "$SITE/demo/cluster/boot/qemu-system-x86_64.wasm"
echo "// fixture out.js" > "$SITE/demo/cluster/boot/out.js"
echo "/* fixture shared site.css */" > "$SITE/assets/site.css"
mkdir -p "$SITE/demo/cluster/boot/assets"
echo "// fixture xterm.js" > "$SITE/demo/cluster/boot/assets/xterm.js"
echo "/* fixture xterm.css */" > "$SITE/demo/cluster/boot/assets/xterm.css"
echo "// fixture xterm-pty.js" > "$SITE/demo/cluster/boot/assets/xterm-pty.js"

echo "4. running build-cluster-demo (run 1)"
OUT1="$WORK/out1"
"$DEMO/build-cluster-demo" V9.9-test --out "$OUT1" \
    --x86-vmlinuz "$WORK/stock-vmlinuz" \
    --x86-initramfs "$WORK/stock-initramfs.cpio.gz" \
    --x86-sysdisk "$WORK/stock-sysdisk.qcow2" \
    --site-dir "$SITE" > "$WORK/run1.log" 2>&1 \
    || { echo "FAIL: generator run 1 exited non-zero"; cat "$WORK/run1.log"; exit 1; }
grep -q "sysdisk_injected=yes" "$WORK/run1.log" \
    || { echo "FAIL: run 1 did not report a real Node A sysdisk injection"; cat "$WORK/run1.log"; exit 1; }

BUNDLE1="$OUT1/V9.9-test"
[ -f "$BUNDLE1/manifest.json" ] || { echo "FAIL: no manifest.json"; exit 1; }
[ -f "$BUNDLE1/initramfs-ovmx-nodeA.cpio.gz" ] || { echo "FAIL: no injected initramfs in bundle"; exit 1; }
[ -f "$BUNDLE1/sysdisk-nodeA.qcow2.gz" ] || { echo "FAIL: no injected sysdisk in bundle"; exit 1; }
[ -f "$BUNDLE1/index.html" ] || { echo "FAIL: page not staged into bundle"; exit 1; }
[ -f "$BUNDLE1/assets/site.css" ] || { echo "FAIL: shared site.css not staged into bundle"; exit 1; }
[ -d "$BUNDLE1/nodeB" ] && { echo "FAIL: Node B directory present without --vax-image (must stay staged/absent)"; exit 1; }
[ -d "$BUNDLE1/nodeC" ] && { echo "FAIL: Node C directory present without --vms-image (must stay staged/absent)"; exit 1; }
echo "4b. asserting boot/assets/{xterm.js,xterm.css,xterm-pty.js} staged (rd vms-a4f: node.html throws"
echo "    'Terminal is not defined' and Node A never boots without them)"
for a in xterm.js xterm.css xterm-pty.js; do
    [ -f "$BUNDLE1/boot/assets/$a" ] || { echo "FAIL: boot/assets/$a missing from bundle"; exit 1; }
done

echo "5. asserting the injected sysdisk actually carries VAXCLUSTER=2 SCSNODE=OVMXA (real ODS-2 read-back)"
gunzip -c "$BUNDLE1/sysdisk-nodeA.qcow2.gz" > "$WORK/check.qcow2"
qemu-img convert -O raw "$WORK/check.qcow2" "$WORK/check.raw"
"$WORK/vmsfs_master" --ods2 list "$WORK/check.raw" | grep -qi '\]OVMXVMSSYS.PAR;2' \
    || { echo "FAIL: bundle sysdisk has no injected OVMXVMSSYS.PAR;2"; exit 1; }

echo "6. asserting manifest.json declares Node A live, Node B/C staged"
"$PYTHON" - "$BUNDLE1/manifest.json" <<'PYEOF'
import json, sys
m = json.load(open(sys.argv[1]))
assert m["tag"] == "V9.9-test", m
assert m["nodes"]["A"]["name"] == "OVMXA", m
assert m["nodes"]["A"]["staged"] is False, m
assert m["nodes"]["A"]["sysdisk_injected"] is True, m
assert m["nodes"]["B"]["staged"] is True, m
assert m["nodes"]["C"]["staged"] is True, m
assert "index.html" in m["files"], m
assert "sysdisk-nodeA.qcow2.gz" in m["files"], m
print("manifest OK")
PYEOF

echo "7. running build-cluster-demo AGAIN (run 2, byte-identical inputs) -- determinism"
OUT2="$WORK/out2"
"$DEMO/build-cluster-demo" V9.9-test --out "$OUT2" \
    --x86-vmlinuz "$WORK/stock-vmlinuz" \
    --x86-initramfs "$WORK/stock-initramfs.cpio.gz" \
    --x86-sysdisk "$WORK/stock-sysdisk.qcow2" \
    --site-dir "$SITE" > "$WORK/run2.log" 2>&1 \
    || { echo "FAIL: generator run 2 exited non-zero"; cat "$WORK/run2.log"; exit 1; }
BUNDLE2="$OUT2/V9.9-test"

echo "8. asserting run1/run2 manifests list the SAME file hashes (byte-identical bundle)"
"$PYTHON" - "$BUNDLE1/manifest.json" "$BUNDLE2/manifest.json" <<'PYEOF'
import json, sys
m1 = json.load(open(sys.argv[1]))
m2 = json.load(open(sys.argv[2]))
f1, f2 = m1["files"], m2["files"]
assert set(f1) == set(f2), (set(f1) ^ set(f2))
diffs = {k: (f1[k], f2[k]) for k in f1 if f1[k] != f2[k]}
assert not diffs, diffs
print("PASS: %d files byte-identical across two runs" % len(f1))
PYEOF

echo "9. asserting a FRESH cluster per release -- a second run of the SAME tag does not accrete stale files"
touch "$OUT1/V9.9-test/stale-marker-from-a-hand-edit"
"$DEMO/build-cluster-demo" V9.9-test --out "$OUT1" \
    --x86-vmlinuz "$WORK/stock-vmlinuz" \
    --x86-initramfs "$WORK/stock-initramfs.cpio.gz" \
    --x86-sysdisk "$WORK/stock-sysdisk.qcow2" \
    --site-dir "$SITE" > "$WORK/run3.log" 2>&1
[ -f "$OUT1/V9.9-test/stale-marker-from-a-hand-edit" ] \
    && { echo "FAIL: generator did not start clean -- stale file survived a re-run"; exit 1; }

echo "10. rd vms-a4f NEGATIVE case: a --boot-dir missing boot/assets/ must FAIL LOUDLY,"
echo "    not silently ship a bundle whose node.html throws 'Terminal is not defined'"
BADBOOT="$WORK/bad-boot-dir"
mkdir -p "$BADBOOT"
cp "$SITE/demo/cluster/boot/qemu-system-x86_64.wasm" "$SITE/demo/cluster/boot/out.js" "$BADBOOT/"
# deliberately no assets/ subdir under $BADBOOT
OUT4="$WORK/out4"
if "$DEMO/build-cluster-demo" V9.9-test --out "$OUT4" \
    --x86-vmlinuz "$WORK/stock-vmlinuz" \
    --x86-initramfs "$WORK/stock-initramfs.cpio.gz" \
    --x86-sysdisk "$WORK/stock-sysdisk.qcow2" \
    --site-dir "$SITE" --boot-dir "$BADBOOT" > "$WORK/run4.log" 2>&1; then
    echo "FAIL: generator exited 0 with a --boot-dir missing assets/ (vms-a4f regression)"; cat "$WORK/run4.log"; exit 1
fi
grep -q "vms-a4f" "$WORK/run4.log" \
    || { echo "FAIL: generator failed but not with the expected vms-a4f diagnostic"; cat "$WORK/run4.log"; exit 1; }

echo "PASS: build-cluster-demo injects a real ODS-2 identity for Node A, stages the page"
echo "      verbatim (including boot/assets/{xterm.js,xterm.css,xterm-pty.js}, rd vms-a4f),"
echo "      stays honest-partial on Node B/C, is byte-deterministic across re-runs, starts"
echo "      each tag's bundle clean (no in-place upgrade), and refuses to ship a bundle"
echo "      Node A can't render into."
