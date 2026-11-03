#!/usr/bin/env bash
# Negative-control selftest for check_kernel_budget.sh (rd vms-c5f): a synthetic
# kernel-out tree must PASS, and each regression class must turn the gate RED.
# Pure shell, no containers, seconds.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
CHK=$HERE/check_kernel_budget.sh
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
rc=0

mk() {   # mk <dir>: a good synthetic tree
    local d=$1 mr=$1/modules/lib/modules/6.12.0-ovmx
    mkdir -p "$mr/kernel/drivers/net"
    head -c 5000 /dev/zero > "$d/vmlinuz"
    printf '%s\n' CONFIG_MODULE_COMPRESS_ZSTD=y CONFIG_DEBUG_INFO_NONE=y CONFIG_MODULE_SIG=y \
        CONFIG_KERNEL_ZSTD=y '# CONFIG_STAGING is not set' > "$d/config"
    : > "$mr/modules.dep"; : > "$mr/modules.builtin"
    for m in igb e1000e nvme; do
        head -c 3000 /dev/zero > "$mr/kernel/drivers/net/$m.ko.zst"
        echo "kernel/drivers/net/$m.ko.zst:" >> "$mr/modules.dep"
    done
    echo "kernel/drivers/usb/host/xhci-pci.ko" >> "$mr/modules.builtin"
}
cat > "$T/budgets" <<'B'
VMLINUZ_MAX=6000
VMLINUZ_MIN=1000
MODULES_MAX_BYTES=10000
MODULES_MIN_BYTES=5000
MODULES_MIN_COUNT=3
INITRAMFS_MAX=100
B
printf '%s\n' igb e1000e nvme xhci_pci > "$T/cov"
run() { BUDGETS=$T/budgets COVERAGE=$T/cov "$CHK" x86_64 "$@" >"$T/out" 2>&1; }
case_() {   # case_ <pass|red> <label> <dir> [extra args]
    local want=$1 label=$2; shift 2
    run "$@"; local r=$?
    if { [ "$want" = pass ] && [ $r -eq 0 ]; } || { [ "$want" = red ] && [ $r -ne 0 ]; }; then
        echo "ok   $label"
    else
        echo "NOT OK $label (wanted $want, exit $r)"; cat "$T/out"; rc=1
    fi
}

mk "$T/good";                                            case_ pass "good tree passes"            "$T/good"
cp -r "$T/good" "$T/big";   head -c 7000 /dev/zero > "$T/big/vmlinuz";      case_ red  "oversize vmlinuz is red"   "$T/big"
cp -r "$T/good" "$T/tiny";  head -c 10 /dev/zero > "$T/tiny/vmlinuz";       case_ red  "undersized vmlinuz is red" "$T/tiny"
cp -r "$T/good" "$T/nomod"; rm -f "$T"/nomod/modules/lib/modules/*/kernel/drivers/net/igb.ko.zst
sed -i '/igb/d' "$T"/nomod/modules/lib/modules/*/modules.dep;               case_ red  "dropped hardware module is red" "$T/nomod"
cp -r "$T/good" "$T/cfg";   echo CONFIG_STAGING=y >> "$T/cfg/config";       case_ red  "pruned category back is red" "$T/cfg"
cp -r "$T/good" "$T/ird";   head -c 500 /dev/zero > "$T/ird.img";           case_ red  "oversize initramfs is red" "$T/ird" "$T/ird.img"
cp -r "$T/good" "$T/raw";   head -c 100 /dev/zero > "$T/raw/modules/lib/modules/6.12.0-ovmx/kernel/drivers/net/extra.ko"
                                                                            case_ red  "uncompressed .ko is red" "$T/raw"
[ $rc -eq 0 ] && echo "PASS: check_kernel_budget.sh selftest" || echo "FAIL: check_kernel_budget.sh selftest"
exit $rc
