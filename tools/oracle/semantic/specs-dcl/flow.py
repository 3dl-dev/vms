# Semantic-oracle DCL spec: CONTROL FLOW (the dcl-scripting$if-then-goto and
# dcl-scripting$call-subroutine rows). What a command procedure does with GOTO
# (forward, backward, a label in another case, a label named by a symbol, a
# missing label), GOSUB / RETURN (nesting, the status RETURN leaves, the symbols
# a GOSUB shares with its level, EXIT inside one), block IF / THEN / ELSE /
# ENDIF (nesting, THEN at the end of the IF line), CALL parameters and the
# locals a CALLed level sees, and the status each leaves.
#
# KIND "CALL" (comgen.py): each case is the body of its own SUBROUTINE, run at a
# fresh procedure level with CALL from a SET NOON driver. A case records what
# it reached by appending letters to the global SP_T; the line printed is
#     <CASE-ID> t=<SP_T> st=<the CALL's $STATUS, hex>
# "x" marks a line that must be skipped.
KIND = "CALL"
SUBS = {
    "SP_RET": ["EXIT 'P1'"],
    "SP_PARMS": ['SP_T == SP_T + P1 + "/" + P2 + "/" + P3 + "/" + P4 + "/"'],
    "SP_SETA": ["SP_A = 2", 'SP_T == SP_T + F$STRING(SP_A)'],
    "SP_READA": ['SP_T == SP_T + F$STRING(SP_A)'],
}
E, I = "%X10000002", "%X10000003"


def mark(c):
    return 'SP_T == SP_T + "%s"' % c


CASES = [
    # GOTO
    ("FLOW.GOTO.FWD", [mark("a"), "GOTO L", mark("x"), "L:", mark("b")]),
    ("FLOW.GOTO.BACK", ["SP_N = 0", "L:", mark("a"), "SP_N = SP_N + 1",
                        "IF SP_N .LT. 3 THEN GOTO L", mark("b")]),
    ("FLOW.GOTO.CASE", ["GOTO lAbEl", mark("x"), "LABEL:", mark("b")]),
    ("FLOW.GOTO.SYMBOL", ['SP_L = "L2"', "GOTO 'SP_L'", mark("x"), "L2:", mark("b")]),
    ("FLOW.GOTO.MISSING", [mark("a"), "GOTO NOWHERE", mark("b")]),
    ("FLOW.GOTO.MISSING.NOON", ["SET NOON", mark("a"), "GOTO NOWHERE", mark("b")]),
    ("FLOW.LABEL.SAMELINE", ["GOTO L", mark("x"), "L: " + mark("a"), mark("b")]),
    # GOSUB / RETURN
    ("FLOW.GOSUB.BASIC", [mark("a"), "GOSUB G", mark("c"), "EXIT", "G:", mark("b"),
                          "RETURN"]),
    ("FLOW.GOSUB.NESTED", ["GOSUB G1", mark("e"), "EXIT", "G1:", mark("a"), "GOSUB G2",
                           mark("c"), "RETURN", "G2:", mark("b"), "RETURN"]),
    ("FLOW.GOSUB.RETURN.STATUS", ["GOSUB G", 'SP_T == SP_T + F$STRING($STATUS)', "EXIT",
                                  "G:", "RETURN " + I]),
    ("FLOW.GOSUB.RETURN.PLAIN", ["GOSUB G", 'SP_T == SP_T + F$STRING($STATUS)', "EXIT",
                                 "G:", "SP_Z = 1", "RETURN"]),
    ("FLOW.GOSUB.LOCALS", ["SP_A = 1", "GOSUB G", 'SP_T == SP_T + F$STRING(SP_A)', "EXIT",
                           "G:", "SP_A = 2", "RETURN"]),
    ("FLOW.GOSUB.EXIT", ["GOSUB G", mark("x"), "EXIT", "G:", mark("a"), "EXIT " + I]),
    ("FLOW.GOSUB.MISSING", [mark("a"), "GOSUB NOPE", mark("b")]),
    ("FLOW.RETURN.NOGOSUB", ["SET NOON", mark("a"), "RETURN", mark("b")]),
    # block IF
    ("FLOW.IF.THEN", ["IF 1", "THEN", mark("a"), "ELSE", mark("x"), "ENDIF", mark("c")]),
    ("FLOW.IF.ELSE", ["IF 0", "THEN", mark("x"), "ELSE", mark("b"), "ENDIF", mark("c")]),
    ("FLOW.IF.NOELSE", ["IF 0", "THEN", mark("x"), "ENDIF", mark("b")]),
    ("FLOW.IF.NESTED", ["IF 1", "THEN", "IF 0", "THEN", mark("x"), "ELSE", mark("b"),
                        "ENDIF", mark("c"), "ELSE", mark("x"), "ENDIF", mark("d")]),
    ("FLOW.IF.NESTED.SKIPPED", ["IF 0", "THEN", "IF 1", "THEN", mark("x"), "ELSE",
                                mark("x"), "ENDIF", mark("x"), "ELSE", mark("b"), "ENDIF",
                                mark("c")]),
    ("FLOW.IF.THEN.EOL", ["IF 1 THEN", mark("a"), "ELSE", mark("x"), "ENDIF", mark("c")]),
    ("FLOW.IF.GOTO.OUT", ["IF 1", "THEN", mark("a"), "GOTO L", mark("x"), "ENDIF",
                          mark("x"), "L:", mark("b")]),
    ("FLOW.IF.STRING.YES", ['IF "YES" THEN ' + mark("a"), 'IF "NO" THEN ' + mark("x"),
                            'IF "abc" THEN ' + mark("x"), mark("b")]),
    ("FLOW.IF.STATUS", ["SET NOON", "CALL SP_RET " + E, "IF .NOT. $STATUS THEN " + mark("n"),
                        "IF $SEVERITY .EQ. 2 THEN " + mark("s")]),
    # CALL
    ("FLOW.CALL.PARAMS", ['CALL SP_PARMS abc "b c" 3']),
    ("FLOW.CALL.QUOTED", ['CALL SP_PARMS "Mixed Case" lower']),
    ("FLOW.CALL.LOCALS", ["SP_A = 1", "CALL SP_SETA", 'SP_T == SP_T + F$STRING(SP_A)']),
    ("FLOW.CALL.READ.OUTER", ["SP_A = 7", "CALL SP_READA"]),
    ("FLOW.CALL.STATUS", ["CALL SP_RET " + I, 'SP_T == SP_T + F$STRING($STATUS)']),
    ("FLOW.CALL.MISSING", ["SET NOON", mark("a"), "CALL NOPE", mark("b")]),
]
