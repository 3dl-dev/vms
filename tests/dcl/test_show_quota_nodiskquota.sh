#!/bin/bash
# TEST: SHOW QUOTA reports the honest %SYSTEM-F-NODISKQUOTA, never a fabricated
#       disk-quota entry (vms-73c4 / vms-050 / INV-6).
#
# WHAT IT USED TO BE. SHOW QUOTA printf'd a hardcoded
#     User [200,1] has 0 blocks used, 0 available
# -- a fabricated UIC (it ignored the real process entirely) and a fabricated
# measurement. That is the exact fabrication class INV-6 exists to kill: a
# command reporting invented state that passes a smoke test while telling the
# operator nothing true.
#
# WHAT IT IS NOW. A disk quota entry lives in a volume's QUOTA.SYS, charged
# per-UIC by the Files-11 ACP. OVMX has no disk-quota facility -- the ODS-2
# volumes it mounts carry no QUOTA.SYS -- so there is no real entry to report,
# and the honest answer is the error real VMS returns when the running UIC has
# no disk quota entry: %SYSTEM-F-NODISKQUOTA. Never an invented one (Rule 10).
#
# This path takes no $GETJPI and has no /dev/vms dependency, so unlike SHOW
# PROCESS/QUOTAS it is a full POSITIVE gate on a host with no executive: the
# honest message MUST appear, and the fabricated UIC/measurement MUST NOT.
#
# EXPECT: contains:%SYSTEM-F-NODISKQUOTA
# EXPECT: regex:no disk quota entry
# EXPECT_NOT: contains:[200,1]
# EXPECT_NOT: contains:blocks used
# EXPECT_NOT: regex:0 available
VMSDCL="${VMSDCL:-vmsdcl}"
echo 'SHOW QUOTA' | $VMSDCL 2>&1
