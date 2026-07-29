#!/bin/bash
# TEST: The MOUNT/DISMOUNT examples OVMX SHIPS in its own HELP library must actually work
#       against the product's own MOUNT device inventory
# EXPECT: contains:ALL_HELP_EXAMPLES_WORKED
# EXPECT_NOT: contains:HELP_EXAMPLE_BROKEN
# EXPECT_NOT: contains:%MOUNT-F-NOSUCHDEV
#
# Root cause (vms-b9f, round 3 rework -- a REGRESSION, not a pre-existing bug): vms-b9f R2
# narrowed cmd_mount()'s accepted devices from "any recognized VMS disk class" down to a
# small, fixed, exactly-named inventory (dcl_is_configured_device, dcl_builtin.c: DKA0,
# DUA0, DJA0). That narrowing was correct -- DKA100:/DKA200: are exactly the kind of
# "syntactically plausible but never configured" device the item was raised to make MOUNT
# reject (see tests/dcl/test_mount_unknown_device.sh, which asserts MOUNT DKA100: is
# rejected) -- but distro/rootfs/vms/SYS0/SYSCOMMON/SYSHLP/HELPLIB.HLP's own MOUNT and
# DISMOUNT "2 Examples" sections still told users to run exactly those commands
# ($ MOUNT DKA100: USERDATA, $ MOUNT/SYSTEM DKA200: SHARED, $ DISMOUNT DKA100:). Verified
# live against the pre-fix binary: every one of those three shipped examples returned
# "%MOUNT-F-NOSUCHDEV, no such device available" instead of succeeding. OVMX's own
# documentation was silently invalidated by the narrowing, and the design-change cascade's
# documentation leg was never run for it.
#
# Fix: HELPLIB.HLP's examples were updated to devices OVMX actually has (DUA0:, DJA0:).
# This test extracts the exact command lines from the SHIPPED HELPLIB.HLP "2 Examples"
# sections under "1 MOUNT" and "1 DISMOUNT" (not hardcoded literals chosen to match the
# inventory) and executes them through vmsdcl, so it fails again the moment either side
# (the inventory or the shipped docs) drifts from the other -- regardless of which one
# changes.
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
HELPLIB="$REPO_ROOT/distro/rootfs/vms/SYS0/SYSCOMMON/SYSHLP/HELPLIB.HLP"

if [ ! -f "$HELPLIB" ]; then
    echo "HELP_EXAMPLE_BROKEN: shipped HELPLIB.HLP not found at $HELPLIB"
    exit 1
fi

# Pull the "2 Examples" block that immediately follows the "1 MOUNT" topic header, up to
# the next "1 "-level topic, and keep only the "$ " command lines in it.
mount_examples=$(awk '/^1 MOUNT$/{f=1; next} /^1 /{f=0} f' "$HELPLIB" | grep '^ \$ ' | sed 's/^ \$ //')
# Same for "1 DISMOUNT".
dismount_examples=$(awk '/^1 DISMOUNT$/{f=1; next} /^1 /{f=0} f' "$HELPLIB" | grep '^ \$ ' | sed 's/^ \$ //')

echo "--- MOUNT examples from shipped HELPLIB.HLP ---"
echo "$mount_examples"
echo "--- DISMOUNT examples from shipped HELPLIB.HLP ---"
echo "$dismount_examples"

if [ -z "$mount_examples" ]; then
    echo "HELP_EXAMPLE_BROKEN: no MOUNT examples found under '1 MOUNT' in shipped HELPLIB.HLP"
    exit 1
fi
if [ -z "$dismount_examples" ]; then
    echo "HELP_EXAMPLE_BROKEN: no DISMOUNT examples found under '1 DISMOUNT' in shipped HELPLIB.HLP"
    exit 1
fi

# Run the MOUNT examples verbatim, then dismount each device the examples used so the
# DISMOUNT example(s) below start from a clean, mounted state (matching how a real user
# would follow the doc top to bottom -- MOUNT first, DISMOUNT second).
script=""
mount_devices=""
while IFS= read -r line; do
    [ -z "$line" ] && continue
    script="${script}${line}
"
    # First whitespace-delimited token of a MOUNT/MOUNT-qualified example is the device.
    dev=$(echo "$line" | sed -E 's/^MOUNT(\/[A-Z]+)?[[:space:]]+([^[:space:]]+).*/\2/')
    mount_devices="${mount_devices}${dev}
"
done <<< "$mount_examples"

while IFS= read -r dev; do
    [ -z "$dev" ] && continue
    script="${script}DISMOUNT ${dev}
"
done <<< "$mount_devices"

# Then run the shipped DISMOUNT example(s) verbatim too, after re-mounting the same device
# so it is actually mounted when the example runs (otherwise it fails with the unrelated
# DEVNOTMNT, not the NOSUCHDEV this test guards against).
while IFS= read -r line; do
    [ -z "$line" ] && continue
    dev=$(echo "$line" | sed -E 's/^DISMOUNT[[:space:]]+([^[:space:]]+).*/\1/')
    script="${script}MOUNT ${dev} REMOUNT
${line}
"
done <<< "$dismount_examples"

output=$(printf '%s' "$script" | $VMSDCL 2>&1)
echo "--- Session output ---"
echo "$output"

FAIL=0
if echo "$output" | grep -q '%MOUNT-F-NOSUCHDEV'; then
    echo "HELP_EXAMPLE_BROKEN: a shipped MOUNT/DISMOUNT example targets a device OVMX's MOUNT does not recognize"
    FAIL=1
fi
if ! echo "$output" | grep -q '%MOUNT-I-MOUNTED'; then
    echo "HELP_EXAMPLE_BROKEN: no shipped MOUNT example actually succeeded"
    FAIL=1
fi
if ! echo "$output" | grep -q '%DISMOUNT-I-DISMOUNTED'; then
    echo "HELP_EXAMPLE_BROKEN: no shipped DISMOUNT example actually succeeded"
    FAIL=1
fi

if [ "$FAIL" -eq 0 ]; then
    echo "ALL_HELP_EXAMPLES_WORKED"
fi
