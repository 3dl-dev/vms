# Semantic-oracle DCL spec: FILE I/O FROM DCL (OPEN / READ / WRITE / CLOSE).
# What a command procedure sees when it writes and reads a sequential file:
# the $STATUS each step leaves, READ at end of file with and without
# /END_OF_FILE, OPEN of a missing file with and without /ERROR, WRITE of a
# list, WRITE/SYMBOL, OPEN/APPEND, the logical name OPEN defines and CLOSE
# removes, and the errors for a file that is not open or opened for reading.
#
# KIND "CALL" (comgen.py): each case is the body of its own SUBROUTINE, run at
# a fresh procedure level. Each records what it reached, and the $STATUS of a
# step, in the global SP_T; the line printed is
#     <CASE-ID> t=<SP_T> st=<the CALL's $STATUS, hex>
# Files are created in the default directory and deleted by the case.
KIND = "CALL"
SUBS = {}
F = "SP_IO.TMP"


def st():
    return 'SP_T == SP_T + "/" + F$STRING($STATUS)'


def mark(c):
    return 'SP_T == SP_T + "%s"' % c


def make(lines):
    return ["OPEN/WRITE SP_F " + F] + ['WRITE SP_F "%s"' % l for l in lines] + ["CLOSE SP_F"]


CLEAN = ["CLOSE SP_F", "DELETE " + F + ";*"]

CASES = [
    ("FIO.WRITE.STATUS", ["SET NOON", "OPEN/WRITE SP_F " + F, st(), 'WRITE SP_F "one"', st(),
                          "CLOSE SP_F", st()] + CLEAN),
    ("FIO.READ.BACK", ["SET NOON"] + make(["one", "two"]) +
     ["OPEN/READ SP_F " + F, st(), "READ SP_F SP_L", st(), 'SP_T == SP_T + "/" + SP_L',
      "READ SP_F SP_L", 'SP_T == SP_T + "/" + SP_L', "READ/END_OF_FILE=E SP_F SP_L",
      mark("x"), "E:", st()] + CLEAN),
    ("FIO.READ.EOF.NOLABEL", ["SET NOON"] + make([]) +
     ["OPEN/READ SP_F " + F, "READ SP_F SP_L", st(), 'SP_T == SP_T + "/[" + SP_L + "]"'] + CLEAN),
    # (its own file and logical name: the level ends with SP_G still open)
    ("FIO.READ.EOF.DEFAULT", ["OPEN/WRITE SP_G SP_IOD.TMP", "CLOSE SP_G",
                              "OPEN/READ SP_G SP_IOD.TMP", mark("a"), "READ SP_G SP_L",
                              mark("b")]),
    ("FIO.OPEN.MISSING", ["SET NOON", "OPEN/READ SP_F SP_NOSUCH.TMP", st()]),
    ("FIO.OPEN.MISSING.ERROR", ["OPEN/READ/ERROR=L SP_F SP_NOSUCH.TMP", mark("x"), "L:", st()]),
    ("FIO.OPEN.MISSING.DEFAULT", [mark("a"), "OPEN/READ SP_F SP_NOSUCH.TMP", mark("b")]),
    ("FIO.WRITE.LIST", ["SET NOON", "OPEN/WRITE SP_F " + F, 'WRITE SP_F "a", "b", 3, "c"',
                        "CLOSE SP_F", "OPEN/READ SP_F " + F, "READ SP_F SP_L",
                        'SP_T == SP_T + "/" + SP_L'] + CLEAN),
    ("FIO.WRITE.SYMBOL", ["SET NOON", 'SP_W = "sym" + "bol"', "OPEN/WRITE SP_F " + F,
                          "WRITE/SYMBOL SP_F SP_W", st(), "CLOSE SP_F", "OPEN/READ SP_F " + F,
                          "READ SP_F SP_L", 'SP_T == SP_T + "/" + SP_L'] + CLEAN),
    ("FIO.APPEND", ["SET NOON"] + make(["1"]) + ["OPEN/APPEND SP_F " + F, st(),
                                                 'WRITE SP_F "2"', "CLOSE SP_F",
                                                 "OPEN/READ SP_F " + F, "READ SP_F SP_A",
                                                 "READ SP_F SP_B",
                                                 'SP_T == SP_T + "/" + SP_A + SP_B'] + CLEAN),
    ("FIO.READ.TYPE", ["SET NOON"] + make(["123"]) +
     ["OPEN/READ SP_F " + F, "READ SP_F SP_L", 'SP_T == SP_T + "/" + F$TYPE(SP_L)',
      'SP_T == SP_T + "/" + F$STRING(SP_L + 1)', 'SP_T == SP_T + "/" + (SP_L + SP_L)'] + CLEAN),
    ("FIO.LOGICAL", ["SET NOON", "OPEN/WRITE SP_F " + F,
                     'SP_T == SP_T + "/" + F$STRING(F$TRNLNM("SP_F") .NES. "")',
                     "CLOSE SP_F", 'SP_T == SP_T + "/" + F$STRING(F$TRNLNM("SP_F") .EQS. "")',
                     "DELETE " + F + ";*"]),
    ("FIO.CLOSE.NOTOPEN", ["SET NOON", "CLOSE SP_NOTOPEN", st()]),
    ("FIO.READ.NOTOPEN", ["SET NOON", "READ SP_NOTOPEN SP_L", st()]),
    ("FIO.WRITE.READONLY", ["SET NOON"] + make(["r"]) + ["OPEN/READ SP_F " + F,
                                                         'WRITE SP_F "x"', st()] + CLEAN),
    ("FIO.OPEN.TWICE", ["SET NOON"] + make(["t"]) + ["OPEN/READ SP_F " + F,
                                                     "OPEN/READ SP_F " + F, st()] + CLEAN),
]
