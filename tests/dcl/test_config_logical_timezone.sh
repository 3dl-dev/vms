#!/bin/bash
# TEST: vms-f89 - SYS$TIMEZONE_* are live system logicals seeded from the system
#       TZ config (config from logicals, parent vms-704). F$TRNLNM returns the
#       zone name, changeover rule and differential; a DEFINE overrides live.
#
# --- The differential is defined and numeric (seconds from UTC / TDF). SHOW
#     SYMBOL displays an all-digit value as an INTEGER (no quotes). On
#     origin/main SYS$TIMEZONE_DIFFERENTIAL is ABSENT -> F$TRNLNM returns "" ->
#     DIFF = "" (a quoted empty string, no digit), so this fails.
# EXPECT: regex:DIFF = -?[0-9]+
# --- The zone name is defined (non-empty string, shown quoted). Absent (empty)
#     on origin/main -> DIFF/NAME = "" has no non-quote char, so this fails.
# EXPECT: regex:NAME = "[^"]+"
# --- The changeover rule is defined (non-empty). Absent on origin/main.
# EXPECT: regex:RULE = "[^"]+"
# --- A DEFINE takes effect at the next F$TRNLNM (live override). Integer value,
#     so shown unquoted.
# EXPECT: contains:D2 = 54321
#
# Doc pin (VSI OpenVMS System Manager's Manual, Vol. 1, "Managing the System
# Time"): SYS$TIMEZONE_NAME (zone name), SYS$TIMEZONE_RULE (std/daylight
# changeover rule), SYS$TIMEZONE_DIFFERENTIAL (TDF, seconds from UTC). The
# companion test_config_logical_timezone.c proves the FORMATTERS read the
# differential live.
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'DIFF = F$TRNLNM("SYS$TIMEZONE_DIFFERENTIAL")\nSHOW SYMBOL DIFF\nNAME = F$TRNLNM("SYS$TIMEZONE_NAME")\nSHOW SYMBOL NAME\nRULE = F$TRNLNM("SYS$TIMEZONE_RULE")\nSHOW SYMBOL RULE\nDEFINE SYS$TIMEZONE_DIFFERENTIAL 54321\nD2 = F$TRNLNM("SYS$TIMEZONE_DIFFERENTIAL")\nSHOW SYMBOL D2\n' | $VMSDCL 2>&1
