#!/bin/bash
# TEST: F$ lexical completeness (vms-fdf) — unknown-lexical %DCL error + new lexicals
#
# Two things this suite proves, each a fails-on-facade assertion against the
# pre-vms-fdf tree:
#
# 1. THE FACADE-KILL. dcl_eval_lexical's unknown-function path used to return a
#    silent empty string (INV-DCL's one remaining lexical-layer fake). It now
#    emits the AUTHENTIC VMS diagnostic. Grounded to the lab-2 VAX V7.3 oracle
#    (vaxlab-1, 11-AUG-2026): an undefined lexical answers, verbatim,
#        %DCL-W-IVFNAM, invalid lexical function name - check validity and spelling
#         \F$BOGUS(\
#    On the old tree F$BOGUS(...) prints nothing (empty string), so the IVFNAM
#    EXPECT below is red before the change and green after.
#
# 2. THE NEW LEXICALS. On the old tree each unknown F$xxx returned "" (the same
#    facade), so `A = F$CUNITS(1024)` left A = "" — SHOW SYMBOL would print
#    A = "". The exact-value EXPECTs below are therefore red before and green
#    after:
#      * F$CUNITS / F$DELTA_TIME — computational, VMS-exact output (VSI OpenVMS
#        DCL Dictionary; F$CUNITS examples 512KB / 1BLOCKS / 524288BYTES / 0GB
#        reproduced from the Dictionary/Wiki entry).
#      * F$CSID / F$MULTIPATH — honest non-clustered / no-multipath answer:
#        an EMPTY list, never a fabricated CSID or path (INV-6). ARGREQ on a
#        missing required argument, matching the VAX oracle.
#      * F$SETPRV — INV-6 executive property: with no /dev/vms (this userspace
#        harness, as for F$GETJPI CURPRIV) it fails HONESTLY and returns the
#        empty string via $STATUS — never a fabricated privilege-name string.
#        Its live prior-state string rides the executive-proven sys$setprv
#        (tests/qemu/test_syssvc_setprv.c / test_syssvc_setprv_dcl.c).
#
# --- facade-kill: unknown lexical is the authentic %DCL error, not empty ---
# EXPECT: contains:%DCL-W-IVFNAM, invalid lexical function name - check validity and spelling
# EXPECT: contains: \F$BOGUS(\
#
# --- F$CUNITS (VSI OpenVMS DCL Dictionary: block=512 bytes, binary KB/MB/...) ---
# EXPECT: contains:CU1 = "512KB"
# EXPECT: contains:CU2 = "1BLOCKS"
# EXPECT: contains:CU3 = "524288BYTES"
# EXPECT: contains:CU4 = "0GB"
#
# --- F$DELTA_TIME (VSI OpenVMS DCL Dictionary: DDD HH:MM:SS.CC; DCL fmt uses -) ---
# EXPECT: contains:DT1 = "1 02:24:18.92"
# EXPECT: contains:DT2 = "0-00:00:05.62"
#
# --- F$CSID: honest non-cluster empty list + ARGREQ on missing arg ---
# EXPECT: contains:CS1 = ""
# EXPECT: contains:%DCL-W-ARGREQ, missing argument - supply all required arguments
#
# --- F$MULTIPATH: honest no-multipath empty path name ---
# EXPECT: contains:MP1 = ""
#
# --- F$SETPRV: INV-6 — no fabricated privilege string when the executive is absent ---
# EXPECT: contains:SP1 = ""
# EXPECT_NOT: regex:SP1 = "(NO)?(OPER|GROUP|WORLD)
#
# --- the new lexicals themselves must NOT be reported as unknown ---
# EXPECT_NOT: contains:\F$CUNITS(\
# EXPECT_NOT: contains:\F$DELTA_TIME(\
# EXPECT_NOT: contains:\F$SETPRV(\

VMSDCL="${VMSDCL:-vmsdcl}"

printf '%s\n' \
  'X = F$BOGUS("abc")' \
  'CU1 = F$CUNITS(1024)' \
  'SHOW SYMBOL CU1' \
  'CU2 = F$CUNITS(512,"b","blocks")' \
  'SHOW SYMBOL CU2' \
  'CU3 = F$CUNITS(1024,"BLOCKS","BYTES")' \
  'SHOW SYMBOL CU3' \
  'CU4 = F$CUNITS(10000,"blocks","gb")' \
  'SHOW SYMBOL CU4' \
  'DT1 = F$DELTA_TIME("11-AUG-2026 08:00:00.00","12-AUG-2026 10:24:18.92")' \
  'SHOW SYMBOL DT1' \
  'DT2 = F$DELTA_TIME("11-AUG-2026 08:00:00.00","11-AUG-2026 08:00:05.62","DCL")' \
  'SHOW SYMBOL DT2' \
  'CTX = ""' \
  'CS1 = F$CSID(CTX)' \
  'SHOW SYMBOL CS1' \
  'BAD = F$CSID()' \
  'MC = 0' \
  'MP1 = F$MULTIPATH("DKA0","MP_PATHNAME",MC)' \
  'SHOW SYMBOL MP1' \
  'SP1 = F$SETPRV("NOOPER,GROUP")' \
  'SHOW SYMBOL SP1' \
  | $VMSDCL 2>&1
