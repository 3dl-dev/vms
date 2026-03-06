#!/bin/bash
# TEST: HELP has comprehensive content for commands and lexical functions
# EXPECT: contains:COPY
# EXPECT: contains:DELETE
# EXPECT: contains:SHOW
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'HELP\n' | $VMSDCL 2>&1
