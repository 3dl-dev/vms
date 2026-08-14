#!/bin/bash
# test_sysboot_cluster_params_e2e.sh - vms-46c (gap #2, conversational boot):
# cluster parameters are AUTHORED INTERACTIVELY at the SYSBOOT> prompt, before
# the banner, and the booted system reflects them.
#
# The conversational-boot MECHANISM itself (SYSBOOT> halts pre-banner on the
# boot flag; SHOW/SET/USE/WRITE/CONTINUE against SYS$SYSTEM:OVMXVMSSYS.PAR)
# landed with vms-b81 and is proven, for a STRING parameter and the
# pre-banner/persistence semantics, by tests/qemu/test_sysboot_conversational.sh.
# THIS gate proves the part that test does not: a NUMERIC cluster-identity
# parameter driven through the same prompt, and BOTH the string node name AND
# the numeric parameter observably in effect in the booted, logged-in system --
# the whole point of conversational boot for clustering (docs/design-boot-
# faithful.md §2.2/§4.2: "This is WHERE cluster params get authored
# interactively before boot").
#
# =============================================================================
# WHAT IS PROVEN, AND THE SEMANTICS BEHIND EACH LEG
# =============================================================================
# CASE 1 (positive, conversational): boot with ovmx.flags=0,1.
#   - SYSBOOT> appears (pre-banner; the emptiness-before-prompt proof is
#     test_sysboot_conversational.sh's job, not re-litigated here).
#   - SHOW SCSSYSTEMID exercises the NUMERIC SHOW row (sysboot.c
#     show_numeric_row) -- the string row is all test_sysboot_conversational.sh
#     covers.
#   - SET SCSNODE CLUX          (string cluster node name)
#     SET SCSSYSTEMID 1027      (numeric cluster system id -- an SCSSYSTEMID
#                                from the lab's own 1025/1026/1027 range)
#     WRITE                     (persists a REAL new vmsfs version ;2 over the
#                                seed's ;1 -- the ONLY command that touches the
#                                file; SET alone is in-memory, VMS persistence
#                                semantics)
#     CONTINUE                  (resume the boot with these params in effect)
#   - The boot console itself announces the authored NODE NAME
#     (%OVMX-I-SCSNODE ... CLUX): read_boot_parameters() applies the in-memory
#     conversational SCSNODE to the real hostname this boot.
#   - In the logged-in session:
#       F$GETSYI("NODENAME")    -> CLUX  (the real Linux hostname, set by
#                                         sethostname() from the SYSBOOT> SET)
#       F$GETSYI("SCSSYSTEMID") -> 1027  (dcl_lexical.c reads the OVMXVMSSYS.PAR
#                                         store fresh; it sees the ;2 the
#                                         SYSBOOT> WRITE just minted -- genuine
#                                         adoption of the authored numeric
#                                         value, not a fake)
#
# CASE 2 (bracket, flagless, FRESH disk): the same mastered image, never
#   touched by a SYSBOOT> session, boots normally and shows the SEEDED
#   DEFAULTS -- F$GETSYI("SCSSYSTEMID") is 0 and F$GETSYI("NODENAME") is OVMX,
#   and no SYSBOOT> prompt ever appears. This is what proves CASE 1's values
#   came from the conversational authoring, not from the seed.
#
# =============================================================================
# HARNESS NOTES (inherited from test_boot_scsnode_hostname_e2e.sh -- read its
# header for the full rationale)
# =============================================================================
#   - Every QEMU launch is INLINE (no boot_qemu() helper -- the function+$(...)
#     shape wedges) and wrapped in `timeout -k 15` (a plain `timeout N` only
#     SIGTERMs and then waits unbounded on an unresponsive child).
#   - The console is driven over a mkfifo, one `send` (adds \r) at a time with
#     small settle sleeps between sends, exactly as that gate does. At SYSBOOT>
#     the same fifo carries the SET/WRITE/CONTINUE lines; after CONTINUE it
#     carries the login and the DCL F$GETSYI lines.
#   - CASE 1 needs NO writeback settle / second QEMU process: the SYSBOOT>
#     WRITE and the F$GETSYI read that observes it happen in the SAME running
#     guest, against the SAME mounted volume -- no cross-power-cycle
#     persistence is being tested here (that is test_boot_scsnode_hostname_
#     e2e.sh's job). CASE 2 is a wholly independent disk copy.
#
# Exit 0 = every assertion passed against the real mounted volume.
# Exit 1 = a real failure (the relevant transcript segment is printed).

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
# PRE-INSTALLED distribution disk (vms-8ab): PID 1 does not install on a blank
# disk (vms-2f0), and SYSBOOT needs SYS$SYSTEM:OVMXVMSSYS.PAR to already exist
# to author against it. The mastered image is where distro/rootfs's seeded
# OVMXVMSSYS.PAR;1 (SCSNODE=OVMX, SCSSYSTEMID=0, the full 30-param set) lands.
DISTRIB_IMG=/boot/ovmx-distrib.img
ARCH=$(uname -m)

if [ ! -f "$DISTRIB_IMG" ]; then
    echo "FATAL: $DISTRIB_IMG missing - the mastering stage did not run"
    exit 1
fi

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
fi

PASS=0
FAIL=0
record() {
    local desc="$1" rc="$2"
    if [ "$rc" -eq 0 ]; then echo "  PASS: $desc"; PASS=$((PASS + 1))
    else echo "  FAIL: $desc"; FAIL=$((FAIL + 1)); fi
}
check() {
    local desc="$1" log="$2" pattern="$3" expect="${4:-present}"
    # "--" is load-bearing: some patterns start with "-" (VMS continuation
    # lines, e.g. "-OVMX-I-...") and grep would parse them as options.
    if grep -qaF -- "$pattern" "$log" 2>/dev/null; then
        if [ "$expect" = "present" ]; then record "$desc" 0; else record "$desc" 1; fi
    else
        if [ "$expect" = "absent" ]; then record "$desc" 0; else record "$desc" 1; fi
    fi
}
waitfor() {  # pattern  limit-seconds  log
    local pat="$1" lim="${2:-60}" log="$3" w=0
    while [ $w -lt $((lim * 4)) ]; do
        grep -qaF "$pat" "$log" 2>/dev/null && return 0
        kill -0 "$qp" 2>/dev/null || return 1
        sleep 0.25; w=$((w + 1))
    done
    return 1
}

echo "=== SYSBOOT> authors cluster params -> booted system reflects them (vms-46c gap #2) ==="
echo "arch=$ARCH qemu=$QEMU"

# =============================================================================
# CASE 1: POSITIVE - author SCSNODE (string) + SCSSYSTEMID (numeric) at SYSBOOT>
# =============================================================================
echo ""
echo "--- CASE 1 (positive): conversational boot, SET at SYSBOOT>, verify in-guest ---"

POS_DISK=/tmp/sysboot-clu-pos.img
POS_LOG=/tmp/sysboot-clu-pos.log
POS_FIFO=/tmp/sysboot-clu-pos.in
rm -f "$POS_DISK" "$POS_LOG" "$POS_FIFO"
cp "$DISTRIB_IMG" "$POS_DISK"
mkfifo "$POS_FIFO"

# shellcheck disable=SC2086
timeout -k 15 "$BOOT_TIMEOUT" $QEMU $MACHINE \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet ovmx.flags=0,1" \
    -m 256M -smp 1 -nic none -nodefaults -serial stdio \
    -drive file="$POS_DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$POS_FIFO" >"$POS_LOG" 2>&1 &
qp=$!
exec 4>"$POS_FIFO"
send() { printf '%s\r' "$1" >&4; }
wake_login() {  # feed a CR/second until Username: appears (OPA0: waits for RETURN)
    local logf="$1" w=0
    until grep -qaF 'Username:' "$logf" 2>/dev/null || [ "$w" -ge 120 ]; do
        send ''; sleep 1; w=$((w + 1))
    done
}

# The prompt halts the boot pre-banner; wait for it, THEN author the params.
if waitfor 'SYSBOOT> ' 90 "$POS_LOG"; then rc=0; else rc=1; fi
record "boot: SYSBOOT> prompt reached on ovmx.flags=0,1 (conversational)" "$rc"

if [ "$rc" -eq 0 ]; then
    # Numeric SHOW row (sysboot.c show_numeric_row) -- the header is pinned to
    # the oracle (§3.1); the numeric row itself is exercised here (a string row
    # is all test_sysboot_conversational.sh covers).
    send 'SHOW SCSSYSTEMID'; sleep 1
    check "SYSBOOT>: SHOW SCSSYSTEMID prints the pinned parameter-table header" \
        "$POS_LOG" 'Parameter Name            Current    Default     Min.       Max.   Unit  Dynamic'
    # The numeric row (sysboot.c show_numeric_row) rendered: SCSSYSTEMID's max
    # column is 65535 (its distinctive Max, unique among the numeric params in
    # the seeded set) and the row is tagged "Dec".
    check "SYSBOOT>: SHOW SCSSYSTEMID renders its numeric Max (65535)" "$POS_LOG" '65535'
    check "SYSBOOT>: SHOW SCSSYSTEMID row is tagged numeric (Dec)" "$POS_LOG" '  Dec'

    # Author the cluster identity: a string node name and a numeric system id.
    send 'SET SCSNODE CLUX'; sleep 1
    check "SYSBOOT>: SET SCSNODE (string) changed OVMX -> CLUX" "$POS_LOG" \
        '%SYSGEN-I-SETPARAM, SCSNODE changed from OVMX to CLUX'

    send 'SET SCSSYSTEMID 1027'; sleep 1
    check "SYSBOOT>: SET SCSSYSTEMID (numeric) changed 0 -> 1027" "$POS_LOG" \
        '%SYSGEN-I-SETPARAM, SCSSYSTEMID changed from 0 to 1027'

    # Persist: a REAL new vmsfs version ;2 over the seed's ;1 (same primitive
    # SYSGEN WRITE CURRENT uses). 30 params, matching the seeded set count.
    send 'WRITE'; sleep 1
    if waitfor '%SYSGEN-I-WRITTEN, 30 parameters written to SYS$SYSTEM:OVMXVMSSYS.PAR;2' 20 "$POS_LOG"; then
        rc=0; else rc=1; fi
    record "SYSBOOT>: WRITE minted OVMXVMSSYS.PAR;2 (real vmsfs version)" "$rc"

    # Resume the boot with the authored params in effect.
    send 'CONTINUE'; sleep 1

    # The boot console announces the authored NODE NAME before any DCL runs --
    # read_boot_parameters() applied the in-memory conversational SCSNODE to
    # the real hostname this boot.
    check "boot: %OVMX-I-SCSNODE console line names the authored CLUX" "$POS_LOG" \
        "%OVMX-I-SCSNODE, node name CLUX set from SYS\$SYSTEM:OVMXVMSSYS.PAR"

    wake_login "$POS_LOG"
    if waitfor 'Username:' 120 "$POS_LOG"; then rc=0; else rc=1; fi
    record "boot: reaches the login prompt after CONTINUE" "$rc"
fi

if [ "$rc" -eq 0 ]; then
    send 'SYSTEM'; sleep 1
    send 'MANAGER'; sleep 1
    if waitfor 'Welcome to OpenVMX' 30 "$POS_LOG"; then rc=0; else rc=1; fi
    record "login: SYSTEM logs in" "$rc"

    # STRING param in effect: the real Linux hostname follows the SYSBOOT> SET.
    send 'HOSTP = F$GETSYI("NODENAME")'; sleep 1
    send 'SHOW SYMBOL HOSTP'; sleep 1
    check "in-guest: F\$GETSYI(NODENAME) reads the authored CLUX (real hostname)" \
        "$POS_LOG" 'HOSTP = "CLUX"'

    # NUMERIC param in effect: dcl_lexical.c reads the OVMXVMSSYS.PAR store
    # fresh and sees the ;2 the SYSBOOT> WRITE minted. F$GETSYI returns an
    # INTEGER for a numeric item (unlike NODENAME, a string), so the DCL symbol
    # is integer-typed and SHOW SYMBOL renders it UNQUOTED with the Hex/Octal
    # columns: "SIDP = 1027   Hex = 00000403  Octal = ...". Anchor on the value
    # AND its hex (0x403 == 1027) so a wrong value cannot pass.
    send 'SIDP = F$GETSYI("SCSSYSTEMID")'; sleep 1
    send 'SHOW SYMBOL SIDP'; sleep 1
    check "in-guest: F\$GETSYI(SCSSYSTEMID) reads the authored 1027" \
        "$POS_LOG" 'SIDP = 1027   Hex = 00000403'
fi
kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; exec 4>&- 2>/dev/null
rm -f "$POS_FIFO"

if [ "$FAIL" -ne 0 ]; then
    echo "--- CASE 1 console log ---"; cat "$POS_LOG"
fi

# =============================================================================
# CASE 2: BRACKET - a fresh, flagless boot shows the SEEDED DEFAULTS
# =============================================================================
echo ""
echo "--- CASE 2 (bracket): flagless boot on a fresh disk shows defaults ---"

DEF_DISK=/tmp/sysboot-clu-def.img
DEF_LOG=/tmp/sysboot-clu-def.log
DEF_FIFO=/tmp/sysboot-clu-def.in
rm -f "$DEF_DISK" "$DEF_LOG" "$DEF_FIFO"
cp "$DISTRIB_IMG" "$DEF_DISK"
mkfifo "$DEF_FIFO"

# shellcheck disable=SC2086
timeout -k 15 "$BOOT_TIMEOUT" $QEMU $MACHINE \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 256M -smp 1 -nic none -nodefaults -serial stdio \
    -drive file="$DEF_DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$DEF_FIFO" >"$DEF_LOG" 2>&1 &
qp=$!
exec 4>"$DEF_FIFO"

check "bracket: no SYSBOOT> prompt on a flagless boot" "$DEF_LOG" 'SYSBOOT>' absent

wake_login "$DEF_LOG"
if waitfor 'Username:' 120 "$DEF_LOG"; then rc=0; else rc=1; fi
record "bracket: flagless boot reaches the login prompt" "$rc"

# It never went through SYSBOOT> (the check above ran before login could not
# have printed it either way, but assert again after boot completed for a
# stable transcript).
check "bracket: SYSBOOT> never appeared across the whole flagless boot" "$DEF_LOG" 'SYSBOOT>' absent
check "bracket: the authored CLUX from CASE 1 never appears (separate disk)" "$DEF_LOG" 'CLUX' absent

if [ "$rc" -eq 0 ]; then
    send 'SYSTEM'; sleep 1
    send 'MANAGER'; sleep 1
    if waitfor 'Welcome to OpenVMX' 30 "$DEF_LOG"; then rc=0; else rc=1; fi
    record "bracket: SYSTEM logs in" "$rc"

    send 'HOSTD = F$GETSYI("NODENAME")'; sleep 1
    send 'SHOW SYMBOL HOSTD'; sleep 1
    check "bracket: F\$GETSYI(NODENAME) is the seeded default OVMX" \
        "$DEF_LOG" 'HOSTD = "OVMX"'

    # Numeric item -> integer symbol, rendered unquoted with Hex/Octal (see the
    # CASE 1 SCSSYSTEMID note); the seeded default is 0 (0x0).
    send 'SIDD = F$GETSYI("SCSSYSTEMID")'; sleep 1
    send 'SHOW SYMBOL SIDD'; sleep 1
    check "bracket: F\$GETSYI(SCSSYSTEMID) is the seeded default 0" \
        "$DEF_LOG" 'SIDD = 0   Hex = 00000000'
fi
kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; exec 4>&- 2>/dev/null
rm -f "$DEF_FIFO"

if [ "$FAIL" -ne 0 ]; then
    echo "--- CASE 2 console log ---"; cat "$DEF_LOG"
fi

echo ""
echo "=========================================="
echo "  RESULTS: $PASS/$((PASS + FAIL)) checks passed, $FAIL failed"
echo "=========================================="
if [ "$FAIL" -eq 0 ]; then
    echo "  ALL SYSBOOT CLUSTER-PARAM CHECKS PASSED -- REAL BOOT, REAL PARAMETER FILE, NOT A MOCK"
    exit 0
fi
exit 1
