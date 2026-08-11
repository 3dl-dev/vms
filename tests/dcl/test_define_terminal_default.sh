#!/bin/bash
# TEST: vms-240 create-side default is NON-terminal; /TRANSLATION_ATTRIBUTES=TERMINAL is terminal
# EXPECT: contains:"ITERNDB" = "ITERNDC"
# EXPECT: contains:"ITERTDA" = "ITERTDB"
# EXPECT_NOT: contains:"ITERTDB" = "ITERTDC"
#
# THE FINDING THIS GATES (vms-240; docs/design-vms-parity-map.md, Engine B
# composition depth). cmd_define()/cmd_assign() (src/vmsdcl/dcl_cmd_io.c) used
# to stamp LNM_ATTR_TERMINAL on EVERY logical they created -- the OPPOSITE of
# the VMS default, which associates NO translation attributes with a plain
# DEFINE (VSI OpenVMS DCL Dictionary, DEFINE /TRANSLATION_ATTRIBUTES). That
# always-terminal default was harmless ONLY because both iterative translators
# ignored the TERMINAL flag; vms-240 makes them honor it, so the default MUST
# become non-terminal or every DEFINE chain breaks.
#
# SHOW TRANSLATION now honors LNM$M_TERMINAL (it stops the displayed chain at a
# terminal translation, per the VSI OpenVMS User's Manual iterative-translation
# rule), which is what makes the create-side attribute observable:
#
#   - A PLAIN DEFINE is non-terminal, so a chain composes:
#       DEFINE ITERNDA ITERNDB ; DEFINE ITERNDB ITERNDC
#       SHOW TRANSLATION ITERNDA  ->  "ITERNDA" = "ITERNDB"
#                                     "ITERNDB" = "ITERNDC"   <-- proves non-terminal
#   - DEFINE/TRANSLATION_ATTRIBUTES=TERMINAL is terminal, so the chain STOPS:
#       DEFINE/TRANSLATION_ATTRIBUTES=TERMINAL ITERTDA ITERTDB ; DEFINE ITERTDB ITERTDC
#       SHOW TRANSLATION ITERTDA  ->  "ITERTDA" = "ITERTDB"   (and STOPS)
#     "ITERTDB" = "ITERTDC" is NEVER displayed for ITERTDA.
#
# THIS IS THE TRIPWIRE, not a tautology:
#   - Restore cmd_define()'s old unconditional LNM_ATTR_TERMINAL default and the
#     plain chain goes terminal: SHOW TRANSLATION ITERNDA stops at ITERNDB, so
#     the first EXPECT ("ITERNDB" = "ITERNDC") goes RED.
#   - Drop the /TRANSLATION_ATTRIBUTES parsing (or SHOW TRANSLATION's TERMINAL
#     honoring) and ITERTDA chains through to ITERTDC, so the EXPECT_NOT
#     ("ITERTDB" = "ITERTDC") goes RED.
#   - On the fully pre-vms-240 code /TRANSLATION_ATTRIBUTES is an unknown
#     qualifier (%DCL-W-IVQUAL) so ITERTDA is never created and the second
#     EXPECT ("ITERTDA" = "ITERTDB") goes RED.
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'DEFINE ITERNDA ITERNDB\nDEFINE ITERNDB ITERNDC\nDEFINE/TRANSLATION_ATTRIBUTES=TERMINAL ITERTDA ITERTDB\nDEFINE ITERTDB ITERTDC\nSHOW TRANSLATION ITERNDA\nSHOW TRANSLATION ITERTDA\n' | $VMSDCL 2>&1
