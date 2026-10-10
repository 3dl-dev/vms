#!/bin/bash
# TEST: READ/PROMPT at end of file with no /END_OF_FILE label reports %RMS-E-EOF (VAX V7.3, keystroke RD.EOF; rd vms-bc5)
# EXPECT: contains:%RMS-E-EOF, end of file detected
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'READ/PROMPT="Z: " SYS$COMMAND KSRDEOF\n' | timeout 5 $VMSDCL 2>&1
