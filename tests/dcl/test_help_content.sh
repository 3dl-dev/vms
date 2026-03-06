#!/bin/bash
# TEST: HELP has comprehensive content for commands and lexical functions
# EXPECT: contains:COPY
# EXPECT: contains:Format:
# EXPECT: contains:DELETE
# EXPECT: contains:SHOW
# EXPECT: contains:F$TIME
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'HELP\nHELP COPY\nHELP SHOW\nHELP F$TIME\n' | $VMSDCL 2>&1
