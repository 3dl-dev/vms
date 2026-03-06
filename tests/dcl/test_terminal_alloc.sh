#!/bin/bash
# TEST: F$ENVIRONMENT("TERMINAL") returns a VMS terminal device name
# EXPECT: regex:_[A-Z]{2,3}[0-9]+:
# EXPECT_NOT: contains:/dev/
VMSDCL="${VMSDCL:-vmsdcl}"
# Use symbol assignment since WRITE does not inline-evaluate lexicals
cat <<'EOF' | $VMSDCL 2>&1
$ TERM = F$ENVIRONMENT("TERMINAL")
$ SHOW SYMBOL TERM
EOF
