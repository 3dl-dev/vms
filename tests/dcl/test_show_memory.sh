#!/bin/bash
# TEST: SHOW MEMORY with no executive prints the report banner and omits the Physical Memory Usage section -- it never reads the host's /proc instead
# EXPECT: contains:System Memory Resources on
# EXPECT: contains:SHOW_MEMORY_NOEXEC_OK
# EXPECT_NOT: contains:SHOW_MEMORY_NOEXEC_FAIL
# EXPECT_NOT: contains:MemTotal
# EXPECT_NOT: contains:Modified List Size
# EXPECT_NOT: contains:Paging File Usage
#
# rd vms-b14e4: SHOW MEMORY's figures come ONLY from the executive's $GETSYI
# memory reply (vms_kif_getsyi_memory -- si_meminfo in Linux vms.ko, uvm on
# NetBSD). This host test runs DCL with no /dev/vms, so the honest result is
# the banner with no Physical Memory Usage section. The old path read
# /proc/meminfo and /proc/swaps here; a regression back to it prints a Main
# Memory row on the host and fails below.
#
# The real figures are proven where the executive exists: the cross-check of
# the Total column against the guest's own memory size (2% tolerance, vms-fe21)
# lives in tests/qemu/test_syssvc_dcl_show_memory.c, run against a real
# /dev/vms.
#
# vms-31a (still enforced): no "Modified List Size" line (the retired Linux
# buffers+cached masquerade).
VMSDCL="${VMSDCL:-vmsdcl}"

if [ -e /dev/vms ]; then
    echo "SHOW_MEMORY_NOEXEC_FAIL (this host has /dev/vms; the no-executive case cannot be asserted here)"
    exit 1
fi

output=$(echo "SHOW MEMORY" | $VMSDCL 2>&1)
echo "$output"

if echo "$output" | grep -qE 'Physical Memory Usage|Main Memory'; then
    echo "SHOW_MEMORY_NOEXEC_FAIL (a Physical Memory section was printed with no executive -- it was sourced from the host)"
    exit 1
fi
echo "SHOW_MEMORY_NOEXEC_OK"
