#!/bin/bash
# TEST: vms-5c1 DCL symbol evaluation — bare-symbol IF/assignment eval, P'N'
#       parameter indexing, ''SYM' substitution inside quoted strings, and
#       object-list accumulation (general DCL symbol-evaluation idioms, vms-62b)
# EXPECT: contains:idx ALPHA BETA GAMMA
# EXPECT: contains:objs= ALPHA.OBJ BETA.OBJ GAMMA.OBJ
# EXPECT: contains:assign-ok
# EXPECT: contains:ifbare-ok
# EXPECT: contains:quote HELLO.C
# EXPECT_NOT: contains:idx-fail
# EXPECT_NOT: contains:assign-fail
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
TDIR=$(mktemp -d)

# Sub-procedure: parameter indexing (SRC = P'N'), bare-symbol loop terminator
# (IF SRC .EQS. ""), object-list accumulation, ''SYM' inside quotes.
cat > "$TDIR/loop.com" <<'EOF'
$ OBJS = ""
$ IDX = ""
$ N = 1
$ LOOP:
$   SRC = P'N'
$   IF SRC .EQS. "" THEN GOTO DONE
$   IDX = IDX + " " + SRC
$   OBJS = OBJS + " " + SRC + ".OBJ"
$   N = N + 1
$   IF N .LE. 8 THEN GOTO LOOP
$ DONE:
$   WRITE SYS$OUTPUT "idx''IDX'"
$   WRITE SYS$OUTPUT "objs=''OBJS'"
EOF

# Bare-symbol assignment RHS (A = B assigns B's value) + bare-symbol IF compare.
cat > "$TDIR/eval.com" <<'EOF'
$ FILE = "HELLO"
$ COPY = FILE
$ IF COPY .EQS. "HELLO" THEN WRITE SYS$OUTPUT "assign-ok"
$ IF COPY .NES. "HELLO" THEN WRITE SYS$OUTPUT "assign-fail"
$ A = "one"
$ B = "one"
$ IF A .EQS. B THEN WRITE SYS$OUTPUT "ifbare-ok"
$ WRITE SYS$OUTPUT "quote ''FILE'.C"
EOF

printf 'DEFINE TESTDIR "%s"\nSET DEFAULT TESTDIR:[000000]\n@loop.com ALPHA BETA GAMMA\n@eval.com\n' "$TDIR" | $VMSDCL 2>&1
rm -rf "$TDIR"
