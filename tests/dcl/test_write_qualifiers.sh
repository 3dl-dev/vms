#!/bin/bash
# TEST: WRITE validates its qualifiers; /SYMBOL writes the symbol's value (vms-097).
# EXPECT: contains:long-record-value
# EXPECT: contains:%DCL-W-UNDSYM
# EXPECT: contains: \ZQBOGUS\
# EXPECT: contains: \UPDATE\
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SYM = "long-record-value"\nWRITE/SYMBOL SYS$OUTPUT SYM\nWRITE/SYMBOL SYS$OUTPUT NOSUCHSYM\nWRITE/ZQBOGUS SYS$OUTPUT "hi"\nWRITE/UPDATE SYS$OUTPUT "hi"\n' | $VMSDCL 2>&1
