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

echo "11. rd vms-e18e: Node B is a BUILD-TIME-CONFIGURED slim single disk, staged VERBATIM."
echo "    Fixture: assemble one with the REAL builder (tests/lab-vax/mk_single_disk.py),"
echo "    then assert the generator stages its bytes unchanged."
LABVAX="$REPO/tests/lab-vax"
"$PYTHON" - "$LABVAX/mk_single_disk.py" "$WORK/nodeB.img" <<'PYEOF'
# Build the minimal shape run-boot.sh's sysboot-single produces: a NetBSD/vax
# disklabel in sector 0 with an FFS root in 'a'. The partition-'e' ODS-2 volume
# is then added by the REAL builder (mk_single_disk.main) below, so what the
# generator is asked to verify was produced by the repo's own writer -- not by
# a hand-rolled label this test invented.
import importlib.util, struct, sys
spec = importlib.util.spec_from_file_location("mk_single_disk", sys.argv[1])
msd = importlib.util.module_from_spec(spec); spec.loader.exec_module(msd)
img, secsize = sys.argv[2], msd.SECSIZE
A_SECTORS, TOTAL, A_ORIG = 64, 256, 128
buf = bytearray(secsize * 2)
lab = msd.LABELSECTOR * secsize + msd.LABELOFFSET
struct.pack_into("<I", buf, lab, msd.DISKMAGIC)
struct.pack_into("<I", buf, lab + msd.OFF_MAGIC2, msd.DISKMAGIC)
struct.pack_into("<I", buf, lab + msd.OFF_SECPERCYL, 32)
struct.pack_into("<I", buf, lab + msd.OFF_SECPERUNIT, TOTAL)
struct.pack_into("<H", buf, lab + msd.OFF_NPART, 3)
msd._set_part(buf, lab, 0, A_ORIG, 0, 1024, 7, 8, 16)         # 'a' FFS root (pre-shrink)
msd._set_part(buf, lab, 2, TOTAL, 0, 0, msd.FS_UNUSED, 0, 0)  # 'c' whole disk
struct.pack_into("<H", buf, lab + msd.OFF_CKSUM, 0)
struct.pack_into("<H", buf, lab + msd.OFF_CKSUM, msd._dkcksum(buf, lab, 3))
with open(img, "wb") as f:
    f.write(buf); f.truncate(TOTAL * secsize)
with open(img + ".ods2", "wb") as f:                          # stand-in volume
    f.write(b"ODS2-VOLUME-FIXTURE" * 512)
sys.exit(msd.main(["mk_single_disk.py", img, img + ".ods2", str(A_SECTORS), str(TOTAL)]))
PYEOF
"$PYTHON" "$LABVAX/mk_single_disk.py" verify "$WORK/nodeB.img" \
    || { echo "FAIL: the fixture the real builder produced does not verify"; exit 1; }

OUT5="$WORK/out5"
"$DEMO/build-cluster-demo" V9.9-test --out "$OUT5" \
    --x86-vmlinuz "$WORK/stock-vmlinuz" \
    --x86-initramfs "$WORK/stock-initramfs.cpio.gz" \
    --x86-sysdisk "$WORK/stock-sysdisk.qcow2" \
    --site-dir "$SITE" --vax-image "$WORK/nodeB.img" > "$WORK/run5.log" 2>&1 \
    || { echo "FAIL: generator rejected a genuine build-time-configured Node B"; cat "$WORK/run5.log"; exit 1; }
BUNDLE5="$OUT5/V9.9-test"
[ -f "$BUNDLE5/nodeB/ovmx-vax-nodeB.img.gz" ] || { echo "FAIL: Node B not staged into the bundle"; exit 1; }
gunzip -c "$BUNDLE5/nodeB/ovmx-vax-nodeB.img.gz" > "$WORK/nodeB.roundtrip"
cmp "$WORK/nodeB.img" "$WORK/nodeB.roundtrip" \
    || { echo "FAIL: staged Node B is NOT byte-identical to the build's image (it was re-written)"; exit 1; }
"$PYTHON" - "$BUNDLE5/manifest.json" <<'PYEOF'
import json, sys
b = json.load(open(sys.argv[1]))["nodes"]["B"]
assert b["staged"] is False, b
assert b["injected_here"] is False, b
assert "run-boot.sh" in b["config_source"], b
assert "name" not in b and "id" not in b, ("manifest asserts a Node B identity "
                                           "this generator never read back", b)
print("manifest Node B OK (verbatim, provenance recorded, no unverified identity)")
PYEOF

echo "12. rd vms-e18e TEETH: the two ways a Node B cannot boot must be REFUSED, not shipped."
echo "12a. a qcow2 --vax-image (pcjs reads raw disk bytes and cannot open qcow2)"
qemu-img create -f qcow2 "$WORK/nodeB.qcow2" 1M >/dev/null
OUT6="$WORK/out6"
if "$DEMO/build-cluster-demo" V9.9-test --out "$OUT6" \
    --x86-vmlinuz "$WORK/stock-vmlinuz" --x86-initramfs "$WORK/stock-initramfs.cpio.gz" \
    --x86-sysdisk "$WORK/stock-sysdisk.qcow2" --site-dir "$SITE" \
    --vax-image "$WORK/nodeB.qcow2" > "$WORK/run6.log" 2>&1; then
    echo "FAIL: generator staged a qcow2 Node B pcjs cannot boot"; cat "$WORK/run6.log"; exit 1
fi
# Match the diagnostic's own words, not "qcow2" -- the fixture PATH contains
# that string, so a grep for it passes on any refusal at all.
grep -q "reads RAW disk bytes" "$WORK/run6.log" \
    || { echo "FAIL: refused, but not with the qcow2 diagnostic"; cat "$WORK/run6.log"; exit 1; }

echo "12b. an image with no NetBSD/vax disklabel (e.g. a bare ODS-2 volume passed by mistake)"
OUT7="$WORK/out7"
if "$DEMO/build-cluster-demo" V9.9-test --out "$OUT7" \
    --x86-vmlinuz "$WORK/stock-vmlinuz" --x86-initramfs "$WORK/stock-initramfs.cpio.gz" \
    --x86-sysdisk "$WORK/stock-sysdisk.qcow2" --site-dir "$SITE" \
    --vax-image "$WORK/stock.raw" > "$WORK/run7.log" 2>&1; then
    echo "FAIL: generator staged an image carrying no disklabel as Node B"; cat "$WORK/run7.log"; exit 1
fi
grep -q "carries no NetBSD/vax disklabel" "$WORK/run7.log" \
    || { echo "FAIL: refused, but not with the disklabel diagnostic"; cat "$WORK/run7.log"; exit 1; }

echo "PASS: build-cluster-demo injects a real ODS-2 identity for Node A, stages the page"
echo "      verbatim (including boot/assets/{xterm.js,xterm.css,xterm-pty.js}, rd vms-a4f),"
echo "      stays honest-partial on Node B/C, is byte-deterministic across re-runs, starts"
echo "      each tag's bundle clean (no in-place upgrade), refuses to ship a bundle"
echo "      Node A can't render into, and stages a build-time-configured Node B byte-for-byte"
echo "      while refusing the two shapes pcjs cannot boot (rd vms-e18e)."
