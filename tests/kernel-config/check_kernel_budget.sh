#!/usr/bin/env bash
# OVMX kernel size budget + real-hardware coverage gate (rd vms-c5f, epic vms-19e).
#
# The curated kernel config (distro/kernel/ovmx-diet.config, vms-fee) shrinks the
# kernel WITHOUT abandoning real hardware. This is the honesty check on that
# diet, in both directions:
#   * budgets   vmlinuz / module tree / (optionally) initramfs sizes are recorded
#               and compared with the checked-in budgets
#               (distro/kernel/budgets-<arch>.conf). A MAX catches silent
#               regression (bloat); a MIN catches silent over-stripping.
#   * coverage  every module named in distro/kernel/hw-coverage-<arch>.txt (common
#               bare-metal storage / NIC / USB / HID / wifi / GPU / sound /
#               filesystem drivers) must be in the module tree (modules.dep) or
#               built into vmlinuz (modules.builtin).
#   * config    the diet's pruned categories stay pruned, its compression stays on.
#
# Usage: check_kernel_budget.sh <x86_64|arm64> <kernel-out-dir> [initramfs-file]
#   <kernel-out-dir> is the `--target kernel-out` output of distro/Dockerfile.bootable
#   (vmlinuz, config, modules/lib/modules/<ver>/). Exit 0 = all gates pass.
# Env: BUDGETS=<file> and COVERAGE=<file> override the checked-in files (selftest).
set -u
ARCH=${1:?usage: $0 <x86_64|arm64> <kernel-out-dir> [initramfs-file]}
KOUT=${2:?kernel-out dir}
INITRAMFS=${3:-}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUDGETS=${BUDGETS:-$ROOT/distro/kernel/budgets-$ARCH.conf}
COVERAGE=${COVERAGE:-$ROOT/distro/kernel/hw-coverage-$ARCH.txt}

fail=0
bad() { echo "FAIL: $*"; fail=1; }
size() { stat -c %s "$1"; }

[ -f "$BUDGETS" ] || { echo "FAIL: no budget file $BUDGETS"; exit 1; }
[ -f "$COVERAGE" ] || { echo "FAIL: no coverage list $COVERAGE"; exit 1; }
[ -f "$KOUT/vmlinuz" ] || { echo "FAIL: $KOUT/vmlinuz missing"; exit 1; }
[ -f "$KOUT/config" ] || { echo "FAIL: $KOUT/config missing (kernel-out stage must export the final .config)"; exit 1; }
MODROOT=$(ls -d "$KOUT"/modules/lib/modules/*/ 2>/dev/null | head -1)
[ -n "$MODROOT" ] && [ -f "$MODROOT/modules.dep" ] || { echo "FAIL: no module tree with modules.dep under $KOUT/modules/lib/modules"; exit 1; }

# shellcheck disable=SC1090
. "$BUDGETS"
V=$(size "$KOUT/vmlinuz")
M=$(find "$MODROOT" -type f \( -name '*.ko' -o -name '*.ko.zst' \) -printf '%s\n' | awk '{s+=$1} END{print s+0}')
N=$(find "$MODROOT" -type f \( -name '*.ko' -o -name '*.ko.zst' \) | wc -l)
echo "METRIC arch=$ARCH vmlinuz_bytes=$V modules_bytes=$M modules_count=$N"
[ -z "$INITRAMFS" ] || { I=$(size "$INITRAMFS"); echo "METRIC arch=$ARCH initramfs_bytes=$I"; }

chk_max() { [ "$2" -le "$3" ] || bad "$1 = $2 bytes exceeds budget $3 (distro/kernel/budgets-$ARCH.conf)"; }
chk_min() { [ "$2" -ge "$3" ] || bad "$1 = $2 is below floor $3 -- the diet over-stripped (budgets-$ARCH.conf)"; }
chk_max vmlinuz "$V" "$VMLINUZ_MAX"
chk_min vmlinuz "$V" "$VMLINUZ_MIN"
chk_max module-tree "$M" "$MODULES_MAX_BYTES"
chk_min module-tree "$M" "$MODULES_MIN_BYTES"
chk_min module-count "$N" "$MODULES_MIN_COUNT"
[ -z "$INITRAMFS" ] || chk_max initramfs "$I" "$INITRAMFS_MAX"

# Coverage: module names are matched with '-' == '_', as the kernel does.
norm() { tr '-' '_'; }
have=$( { sed -n 's/^\(.*\.ko\)\(\.zst\)\{0,1\}:.*/\1/p' "$MODROOT/modules.dep"; cat "$MODROOT/modules.builtin"; } \
        | sed 's#.*/##; s/\.ko$//' | norm | sort -u)
missing=0; total=0
while read -r mod _; do
    case "$mod" in ''|'#'*) continue ;; esac
    total=$((total + 1))
    if ! grep -qx "$(printf '%s' "$mod" | norm)" <<<"$have"; then
        bad "hardware module '$mod' is neither in the module tree nor built in (coverage-$ARCH list)"
        missing=$((missing + 1))
    fi
done < "$COVERAGE"
echo "METRIC arch=$ARCH hw_modules_required=$total hw_modules_missing=$missing"

# Config: the diet's pruned categories stay pruned, its compression stays on.
for opt in CONFIG_STAGING CONFIG_MEDIA_SUPPORT CONFIG_HAMRADIO CONFIG_KASAN CONFIG_PROVE_LOCKING CONFIG_DEBUG_INFO_BTF CONFIG_JFFS2_FS CONFIG_ISDN; do
    grep -qE "^${opt}=[ym]$" "$KOUT/config" && bad "$opt is enabled -- a pruned category came back"
done
for opt in CONFIG_MODULE_COMPRESS_ZSTD CONFIG_DEBUG_INFO_NONE CONFIG_MODULE_SIG; do
    grep -q "^${opt}=y$" "$KOUT/config" || bad "$opt is not =y"
done
if [ "$ARCH" = x86_64 ]; then
    grep -q '^CONFIG_KERNEL_ZSTD=y$' "$KOUT/config" || bad "CONFIG_KERNEL_ZSTD is not =y"
fi
# The shipped module tree really is zstd-compressed.
[ "$(find "$MODROOT" -type f -name '*.ko' | wc -l)" -eq 0 ] || bad "uncompressed .ko files in the module tree (MODULE_COMPRESS_ZSTD)"

if [ "$fail" -eq 0 ]; then echo "PASS: kernel budgets + real-hardware coverage ($ARCH)"; else echo "RED: kernel budget / coverage gate ($ARCH)"; fi
exit "$fail"
