#!/bin/bash
# TEST: SHOW QUOTA reports the oracle-faithful %SYSTEM-F-QFNOTACT, never a
#       fabricated disk-quota entry (vms-73c4 / vms-050 / INV-6).
#
# WHAT IT USED TO BE. SHOW QUOTA printf'd a hardcoded
#     User [200,1] has 0 blocks used, 0 available
# -- a fabricated UIC (it ignored the real process entirely) and a fabricated
# measurement. That is the exact fabrication class INV-6 exists to kill: a
# command reporting invented state that passes a smoke test while telling the
# operator nothing true.
#
# WHAT IT IS NOW. Disk quotas live in a volume's QUOTA.SYS, charged per-UIC by
# the Files-11 ACP. OVMX has no disk-quota facility -- the ODS-2 volumes it
# mounts carry no QUOTA.SYS, i.e. disk quotas are NOT ENABLED on the volume --
# so the honest answer is the error a real VAX actually returns for that case:
# %SYSTEM-F-QFNOTACT, "disk quotas not enabled on this volume". Oracle-triggered
# verbatim on live OpenVMS VAX V7.3 (lab-2 vaxlab-2, Rule 8). This CORRECTS the
# earlier draft which named NODISKQUOTA -- a DIFFERENT condition (no quota ENTRY
# for a UIC on a quotas-ENABLED volume) that OVMX never hits.
#
# This path takes no $GETJPI and has no /dev/vms dependency, so unlike SHOW
# PROCESS/QUOTAS it is a full POSITIVE gate on a host with no executive: the
# honest message MUST appear, and the fabricated UIC/measurement MUST NOT.
#
# EXPECT: contains:%SYSTEM-F-QFNOTACT
# EXPECT: regex:disk quotas not enabled
# EXPECT_NOT: contains:[200,1]
# EXPECT_NOT: contains:blocks used
# EXPECT_NOT: regex:0 available
# EXPECT_NOT: contains:NODISKQUOTA
VMSDCL="${VMSDCL:-vmsdcl}"
echo 'SHOW QUOTA' | $VMSDCL 2>&1
