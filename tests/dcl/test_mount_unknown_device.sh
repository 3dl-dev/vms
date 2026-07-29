#!/bin/bash
# TEST: MOUNT rejects devices that were never actually configured -- not just
#       devices whose CLASS syntax is unrecognized
# EXPECT: contains:ALL_REJECTED
# EXPECT_NOT: contains:%MOUNT-I-MOUNTED
# EXPECT_NOT: contains:REGRESSION
#
# Root cause (vms-b9f C3, docs/design-authenticity-roadmap.md §2.3): cmd_mount() used to
# accept ANY string >= 2 chars as a device name and always report success -- the item's own
# reproduction was literally "MOUNT DKA100: ... accepts anything". The first rework only
# rejected unrecognized CLASS syntax (its test used "ZZQ0:" -- "ZZ" isn't a disk/tape
# mnemonic, an input AND expected string both authored by the implementer to match the
# allowlist it had just written). DKA100:, DKA999:, MKB300: and $77$DGA4242: -- all
# syntactically valid classes with a NONZERO unit number -- still mounted successfully on
# that build. Verified: on the current binary, EVERY device below is now rejected.
#
# OVMX has no physical controllers, so it stands in exactly one autoconfigured unit -- unit 0
# -- per recognized class (dcl_is_known_device_class, dcl_builtin.c). This matches real VMS
# pinned live against the oracle (~/vax/cluster/, OpenVMS VAX 7.3 vax1, 2026-07-29): MOUNT
# DUA99: (valid class "DU", unit never configured on that system) returned the byte-identical
# "%MOUNT-F-NOSUCHDEV, no such device available" as the bogus class MOUNT ZZQ0: -- same
# facility, same severity (F, not the self-certified 'E'), same text, no device name echoed.
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"

FAIL=0

# ZZQ0: -- unrecognized device class (not a real VMS disk/tape mnemonic)
# DKA100:, DKA999:, MKB300: -- recognized classes, but a unit that was never configured
# $77$DGA4242: -- recognized class + allocation-class prefix, unit never configured
for spec in "ZZQ0: BOGUS" "DKA100: FOO" "DKA999: FOO" "MKB300: FOO" '$77$DGA4242: FOO'; do
    out=$(printf 'MOUNT %s\n' "$spec" | $VMSDCL 2>&1)
    echo "--- MOUNT $spec ---"
    echo "$out"
    if echo "$out" | grep -q '%MOUNT-I-MOUNTED'; then
        echo "REGRESSION: MOUNT $spec succeeded -- should have been rejected"
        FAIL=1
    fi
    if ! echo "$out" | grep -qF '%MOUNT-F-NOSUCHDEV, no such device available'; then
        echo "REGRESSION: MOUNT $spec did not produce the pinned %MOUNT-F-NOSUCHDEV text"
        FAIL=1
    fi
done

if [ "$FAIL" -eq 0 ]; then
    echo "ALL_REJECTED"
fi
