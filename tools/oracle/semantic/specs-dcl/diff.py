# Semantic-oracle DCL spec: DIFFERENCES (the utilities$differences row).
# What a user sees first when comparing two text files: identical files, a
# changed / inserted / deleted line, two sections, a change at the end, an empty
# file, the next lower version (one parameter), /PARALLEL, /MAXIMUM_DIFFERENCES,
# /IGNORE=TRAILING_SPACES and /IGNORE=BLANK_LINES, /MATCH, /MERGED, a missing
# file, and the $STATUS DIFFERENCES leaves after each.
#
# KIND "CALL" (comgen.py): each case is the body of its own SUBROUTINE. It
# builds its files, runs DIFFERENCES/OUTPUT=SP_D.LIS, records the $STATUS in
# SP_T, and SP_SHOW prints the listing one record per line:
#     <CASE-ID>.Lnn |<record>|
#     <CASE-ID>.N n=<records in the listing>
#     <CASE-ID> t=<$STATUS>[/<F$MESSAGE of it>] st=<the CALL's $STATUS, hex>
# The files live in SYS$SYSDEVICE:[SPDIFF], so the file specifications the
# listing prints are the same on every system; SETUP makes it the default
# directory and TEARDOWN restores the caller's.
KIND = "CALL"
LISTING = True     # capture.py keeps the <CASE-ID>.Lnn and .N lines
SETUP = ['SP_OLDDEF = F$ENVIRONMENT("DEFAULT")',
         'IF F$SEARCH("SYS$SYSDEVICE:[000000]SPDIFF.DIR") .EQS. "" THEN '
         'CREATE/DIRECTORY SYS$SYSDEVICE:[SPDIFF]',
         "SET DEFAULT SYS$SYSDEVICE:[SPDIFF]"]
TEARDOWN = ["SET DEFAULT 'SP_OLDDEF'"]
SUBS = {
    # P1 = case id, P2 = the listing file
    "SP_SHOW": ["SET NOON", "SP_N = 0",
                'IF F$SEARCH(P2) .EQS. "" THEN GOTO NONE',
                "OPEN/READ SP_R 'P2'",
                "LOOP:",
                "READ/END_OF_FILE=DONE SP_R SP_L",
                "SP_N = SP_N + 1",
                'WRITE SYS$OUTPUT P1, ".L", F$FAO("!2ZL", SP_N), " |", SP_L, "|"',
                "GOTO LOOP",
                "DONE:",
                "CLOSE SP_R",
                "NONE:",
                'WRITE SYS$OUTPUT P1, ".N n=", SP_N',
                "EXIT 1"],
}


# every case ends by deleting its files, then an assignment: the CALL's
# status is that of the case's last command, and an assignment's is fixed
CLEAN = ["DELETE SP_*.TMP;*,SP_D.LIS;*", "SP_Z = 0"]


def make(name, lines):
    return (["OPEN/WRITE SP_F " + name] + ['WRITE SP_F "%s"' % l for l in lines] +
            ["CLOSE SP_F"])


def st():
    return 'SP_T == SP_T + F$STRING(SP_S)'


def msg():
    return 'SP_T == SP_T + "/" + F$MESSAGE(SP_S)'


def run(cid, cmd, a, b, show=True):
    """make SP_A.TMP and SP_B.TMP, run DIFFERENCES, record and show the result"""
    body = ["SET NOON"]
    if a is not None:
        body += make("SP_A.TMP", a)
    if b is not None:
        body += make("SP_B.TMP", b)
    body += [cmd, "SP_S = $STATUS", st()]
    if show:
        body.append('CALL SP_SHOW "%s" SP_D.LIS' % cid)
    body += CLEAN
    return (cid, body)


ABCD = ["alpha", "bravo", "charlie", "delta"]
D = "DIFFERENCES/OUTPUT=SP_D.LIS "
AB = " SP_A.TMP SP_B.TMP"
LONG = ["l1", "l2", "l3", "l4", "l5", "l6", "l7", "l8", "l9", "l10"]
LONG2 = ["l1", "X2", "l3", "l4", "l5", "l6", "l7", "X8", "l9", "l10"]

CASES = [
    run("DIFF.SAME", D + AB, ABCD, ABCD),
    run("DIFF.CHANGE", D + AB, ABCD, ["alpha", "BRAVO", "charlie", "delta"]),
    run("DIFF.INSERT", D + AB, ABCD, ["alpha", "bravo", "new", "charlie", "delta"]),
    run("DIFF.DELETE", D + AB, ABCD, ["alpha", "charlie", "delta"]),
    run("DIFF.FIRST", D + AB, ABCD, ["ALPHA", "bravo", "charlie", "delta"]),
    run("DIFF.LAST", D + AB, ABCD, ["alpha", "bravo", "charlie", "DELTA"]),
    run("DIFF.APPEND", D + AB, ABCD, ABCD + ["echo"]),
    run("DIFF.TWO", D + AB, LONG, LONG2),
    run("DIFF.EMPTY", D + AB, ABCD, []),
    # three records replaced by two; a change on a two-digit line number
    run("DIFF.REPLACE", D + AB, ["a", "b", "c", "d", "e", "f"], ["a", "X", "Y", "e", "f"]),
    run("DIFF.LINE10", D + AB, LONG + ["l11"], LONG[:9] + ["X10", "l11"]),
    run("DIFF.PARALLEL", "DIFFERENCES/PARALLEL/OUTPUT=SP_D.LIS" + AB, ABCD,
        ["alpha", "BRAVO", "charlie", "delta"]),
    run("DIFF.PARALLEL.WIDTH", "DIFFERENCES/PARALLEL/WIDTH=80/OUTPUT=SP_D.LIS" + AB, LONG,
        LONG2),
    run("DIFF.MAXDIFF", "DIFFERENCES/MAXIMUM_DIFFERENCES=1/OUTPUT=SP_D.LIS" + AB, LONG,
        LONG2),
    run("DIFF.TRAIL", D + AB, ["alpha", "bravo  ", "charlie"], ["alpha", "bravo", "charlie"]),
    run("DIFF.TRAIL.IGNORE", "DIFFERENCES/IGNORE=TRAILING_SPACES/OUTPUT=SP_D.LIS" + AB,
        ["alpha", "bravo  ", "charlie"], ["alpha", "bravo", "charlie"]),
    run("DIFF.BLANK", D + AB, ["alpha", "", "bravo", "charlie"], ["alpha", "bravo", "charlie"]),
    run("DIFF.BLANK.IGNORE", "DIFFERENCES/IGNORE=BLANK_LINES/OUTPUT=SP_D.LIS" + AB,
        ["alpha", "", "bravo", "charlie"], ["alpha", "bravo", "charlie"]),
    run("DIFF.IGNORE.BOTH", "DIFFERENCES/IGNORE=(TRAILING_SPACES,BLANK_LINES)/OUTPUT=SP_D.LIS" + AB,
        ["alpha", "", "bravo  ", "charlie"], ["alpha", "bravo", "charlie"]),
    run("DIFF.MERGED0", "DIFFERENCES/MERGED=0/OUTPUT=SP_D.LIS" + AB, ABCD,
        ["alpha", "BRAVO", "charlie", "delta"]),
    run("DIFF.MATCH1", "DIFFERENCES/MATCH=1/OUTPUT=SP_D.LIS" + AB, LONG, LONG2),
    # one parameter: the next lower version of the same file
    ("DIFF.VERSIONS", ["SET NOON"] + make("SP_A.TMP", ABCD) +
     make("SP_A.TMP", ["alpha", "bravo", "CHARLIE", "delta"]) +
     ["DIFFERENCES/OUTPUT=SP_D.LIS SP_A.TMP", "SP_S = $STATUS", st(),
      'CALL SP_SHOW "DIFF.VERSIONS" SP_D.LIS'] + CLEAN),
    ("DIFF.NOLOWER", ["SET NOON"] + make("SP_A.TMP", ABCD) +
     ["DIFFERENCES/OUTPUT=SP_D.LIS SP_A.TMP", "SP_S = $STATUS", st(), msg(),
      'CALL SP_SHOW "DIFF.NOLOWER" SP_D.LIS'] + CLEAN),
    ("DIFF.MISSING2", ["SET NOON"] + make("SP_A.TMP", ABCD) +
     ["DIFFERENCES/OUTPUT=SP_D.LIS SP_A.TMP SP_NOSUCH.TMP", "SP_S = $STATUS", st(), msg(),
      'CALL SP_SHOW "DIFF.MISSING2" SP_D.LIS'] + CLEAN),
    ("DIFF.MISSING1", ["SET NOON"] + make("SP_B.TMP", ABCD) +
     ["DIFFERENCES/OUTPUT=SP_D.LIS SP_NOSUCH.TMP SP_B.TMP", "SP_S = $STATUS", st(), msg(),
      'CALL SP_SHOW "DIFF.MISSING1" SP_D.LIS'] + CLEAN),
    # the status of a run that found differences, and of one that did not
    ("DIFF.STATUS", ["SET NOON"] + make("SP_A.TMP", ABCD) + make("SP_B.TMP", ["x"]) +
     ["DIFFERENCES/OUTPUT=SP_D.LIS SP_A.TMP SP_B.TMP", "SP_S = $STATUS", st(), msg(),
      'SP_T == SP_T + "/"', "DIFFERENCES/OUTPUT=SP_D.LIS SP_A.TMP SP_A.TMP",
      "SP_S = $STATUS", st(), msg()] + CLEAN),
]
