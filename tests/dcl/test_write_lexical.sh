#!/bin/bash
# TEST: WRITE evaluates lexical functions (F$xxx) and symbol substitution (vms-65f)
#
# Regression for vms-65f: WRITE SYS$OUTPUT F$GETSYI("VERSION") printed the
# LITERAL "F$GETSYIVERSION" instead of evaluating the lexical function, because
# cmd_write pushed the tokenized params out verbatim instead of routing the
# argument list through the shared DCL expression evaluator (the same one the
# `=` assignment RHS uses). The backend evaluator was always fine — only the
# WRITE / comma-expression-list path bypassed it.
#
# F$GETSYI("VERSION") in WRITE must evaluate to a version, not the literal name:
# EXPECT: regex:V[0-9]+\.[0-9]+
# '' VER '' substitution inside a quoted WRITE argument must resolve:
# EXPECT: regex:ver is V[0-9]+\.[0-9]+
# Plain string literal still written verbatim:
# EXPECT: contains:WLEX_PLAIN_OK
# Bare symbol WRITE resolves to its value:
# EXPECT: contains:WLEX_SYMVAL
# The core bug: WRITE must NOT emit the literal lexical-function name.
# EXPECT_NOT: contains:F$GETSYI
# EXPECT_NOT: contains:F$TRNLNM
# Substitution must resolve, not print the source apostrophes:
# EXPECT_NOT: contains:ver is ''VER
# EXPECT_NOT: contains:%DCL-
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"

"$VMSDCL" 2>&1 <<'DCLEOF'
VER = F$GETSYI("VERSION")
WRITE SYS$OUTPUT F$GETSYI("VERSION")
WRITE SYS$OUTPUT "ver is ''VER'"
WRITE SYS$OUTPUT F$TRNLNM("SYS$SYSDEVICE")
WRITE SYS$OUTPUT "WLEX_PLAIN_OK"
A = "WLEX_SYMVAL"
WRITE SYS$OUTPUT A
DCLEOF
