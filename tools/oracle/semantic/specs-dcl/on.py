# Semantic-oracle DCL spec: ON CONDITIONS (the dcl-scripting$on-condition row).
# What a command procedure does when a command completes with each severity:
# the default action (ON ERROR THEN EXIT), ON WARNING / ERROR / SEVERE_ERROR
# thresholds, GOTO / CONTINUE / EXIT actions, the action being one-shot, SET
# NOON / SET ON, and the per-procedure-level scope of all of it (a CALLed
# level does not inherit its caller's ON or NOON). The DCL Dictionary and the
# User's Manual describe these; these cases measure them on the real system.
#
# KIND "CALL" (comgen.py): each case is the body of its own SUBROUTINE, run at a
# fresh procedure level with CALL from a SET NOON driver. A case records what
# it reached by appending letters to the global SP_T; the line printed is
#     <CASE-ID> t=<SP_T> st=<the CALL's $STATUS, hex>
# Severities come from CALL SP_RET <status>, a subroutine that EXITs with that
# status; bit 28 (STS$M_INHIB_MSG) is set in each so no message is printed and
# only the severity (the low three bits) differs.
KIND = "CALL"
SUBS = {
    "SP_RET": ["EXIT 'P1'"],
    # a nested level that takes a WARNING then an ERROR at its own level
    "SP_INNER": ['SP_T == SP_T + "i"', "CALL SP_RET %X10000000",
                 'SP_T == SP_T + "j"', "CALL SP_RET %X10000002",
                 'SP_T == SP_T + "k"'],
    # what a CALL parameter is: its type and value as the called level sees it
    "SP_PTYPE": ['SP_T == F$TYPE(P1) + "/" + P1'],
}
W, S, E, I, F = "%X10000000", "%X10000001", "%X10000002", "%X10000003", "%X10000004"


def run(sev):
    return "CALL SP_RET " + sev


def mark(c):
    return 'SP_T == SP_T + "%s"' % c


def thresh(cond, sev):
    return ["ON %s THEN GOTO H" % cond, mark("a"), run(sev), mark("b"), "EXIT",
            "H:", mark("h")]


CASES = [
    # the default action: ON ERROR THEN EXIT
    ("ON.DEFAULT.SUCCESS", [mark("a"), run(S), mark("b")]),
    ("ON.DEFAULT.INFO", [mark("a"), run(I), mark("b")]),
    ("ON.DEFAULT.WARNING", [mark("a"), run(W), mark("b")]),
    ("ON.DEFAULT.ERROR", [mark("a"), run(E), mark("b")]),
    ("ON.DEFAULT.SEVERE", [mark("a"), run(F), mark("b")]),
    # each threshold against each severity
    ("ON.WARNING.WARNING", thresh("WARNING", W)),
    ("ON.WARNING.ERROR", thresh("WARNING", E)),
    ("ON.WARNING.SEVERE", thresh("WARNING", F)),
    ("ON.WARNING.SUCCESS", thresh("WARNING", S)),
    ("ON.WARNING.INFO", thresh("WARNING", I)),
    ("ON.ERROR.WARNING", thresh("ERROR", W)),
    ("ON.ERROR.ERROR", thresh("ERROR", E)),
    ("ON.ERROR.SEVERE", thresh("ERROR", F)),
    ("ON.SEVERE.WARNING", thresh("SEVERE_ERROR", W)),
    ("ON.SEVERE.ERROR", thresh("SEVERE_ERROR", E)),
    ("ON.SEVERE.SEVERE", thresh("SEVERE_ERROR", F)),
    # the action fires once; then the default is back
    ("ON.ONESHOT", ["ON WARNING THEN GOTO H", mark("a"), run(W), mark("b"), "EXIT",
                    "H:", mark("h"), run(W), mark("i"), run(E), mark("j")]),
    ("ON.CONTINUE", ["ON ERROR THEN CONTINUE", mark("a"), run(E), mark("b"), run(E),
                     mark("c")]),
    ("ON.EXIT.WARNING", ["ON WARNING THEN EXIT", mark("a"), run(W), mark("b")]),
    ("ON.REARM", ["ON ERROR THEN GOTO H1", run(E), mark("x"), "H1:", mark("1"),
                  "ON ERROR THEN GOTO H2", run(F), mark("y"), "H2:", mark("2")]),
    # the handler sees the failing command's $STATUS and $SEVERITY
    ("ON.HANDLER.STATUS", ["ON ERROR THEN GOTO H", run(E), "EXIT", "H:",
                           'SP_T == F$STRING($STATUS) + "/" + F$STRING($SEVERITY)']),
    # SET NOON / SET ON
    ("ON.NOON", ["SET NOON", mark("a"), run(F), mark("b"), run(E), mark("c")]),
    ("ON.NOON.SETON", ["SET NOON", mark("a"), run(E), mark("b"), "SET ON", run(E),
                       mark("c")]),
    ("ON.NOON.KEEPS.ACTION", ["ON WARNING THEN GOTO H", "SET NOON", mark("a"), run(E),
                              mark("b"), "SET ON", run(W), mark("c"), "EXIT", "H:",
                              mark("h")]),
    ("ON.AFTER.NOON", ["SET NOON", "ON WARNING THEN GOTO H", mark("a"), run(W),
                       mark("b"), "EXIT", "H:", mark("h")]),
    # per-level scope: a CALLed level starts with the default, whatever its caller set
    ("ON.LEVEL.DEFAULT", [mark("a"), "CALL SP_INNER", mark("b")]),
    ("ON.LEVEL.CALLER.WARNING", ["ON WARNING THEN GOTO H", mark("a"), "CALL SP_INNER",
                                 mark("b"), "EXIT", "H:", mark("h")]),
    ("ON.LEVEL.CALLER.NOON", ["SET NOON", mark("a"), "CALL SP_INNER", mark("b")]),
    ("ON.LEVEL.CALLER.CONTINUE", ["ON ERROR THEN CONTINUE", mark("a"), "CALL SP_INNER",
                                  mark("b"), run(E), mark("c")]),
    # the status EXIT returns is an expression (the cases above depend on it)
    ("ON.EXIT.SYMBOL", ["SP_X = 44", "EXIT SP_X"]),
    ("ON.EXIT.EXPR", ["EXIT 40 + 4"]),
    ("ON.EXIT.HEX", ["EXIT %X2C"]),
    ("ON.CALL.PARAM", ["CALL SP_PTYPE 44"]),
]
