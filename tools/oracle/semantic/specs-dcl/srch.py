# Semantic-oracle DCL spec: ELLIPSIS WILDCARD (the filespec$ellipsis row, rd vms-28c).
# The directory-tree wildcard [...] in a file specification: what $SEARCH (and
# so F$SEARCH, DIRECTORY, DELETE, COPY, PURGE) finds when the directory part is
# [...], [.A...], [...B], [.*...], with and without a wildcard in the name, in
# what order, and what is left after DELETE / PURGE over the tree. The cases
# measure these on the real system. The device is never printed (the result
# is cut to the part of the directory below the default directory) so the
# transcript does not depend on which volume or directory the probe ran in.
#
# KIND "CALL" (comgen.py). SETUP builds, under the default directory,
#   [.SRCHT]      TOP.TMP TOP.DAT
#   [.SRCHT.A]    A1.TMP  A2.DAT  PUR.DAT;1 PUR.DAT;2
#   [.SRCHT.A.B]  B1.TMP
#   [.SRCHT.C]    C1.TMP
# and makes [.SRCHT] the default directory. SP_ITER iterates F$SEARCH over its
# P1 and appends the directory-below-base, name, type and version of every file
# found to SP_T; SP_DIR runs DIRECTORY over its P1 and the case prints
# DIRECTORY's status (its listing goes to the console and is not compared). The last case removes
# the tree. A case prints  <CASE-ID> t=<SP_T> st=<the CALL's $STATUS, hex>.
KIND = "CALL"
SETUP = [
    'SP_BL = F$LENGTH(F$PARSE(F$ENVIRONMENT("DEFAULT"),,,"DIRECTORY")) - 1',
    "CREATE/DIRECTORY [.SRCHT]",
    "CREATE/DIRECTORY [.SRCHT.A]",
    "CREATE/DIRECTORY [.SRCHT.A.B]",
    "CREATE/DIRECTORY [.SRCHT.C]",
    "CALL SP_MK [.SRCHT]TOP.TMP",
    "CALL SP_MK [.SRCHT]TOP.DAT",
    "CALL SP_MK [.SRCHT.A]A1.TMP",
    "CALL SP_MK [.SRCHT.A]A2.DAT",
    "CALL SP_MK [.SRCHT.A]PUR.DAT",
    "CALL SP_MK [.SRCHT.A]PUR.DAT",
    "CALL SP_MK [.SRCHT.A.B]B1.TMP",
    "CALL SP_MK [.SRCHT.C]C1.TMP",
    "SET DEFAULT [.SRCHT]",
]
SUBS = {
    "SP_MK": ["OPEN/WRITE SP_F 'P1'", 'WRITE SP_F "x"', "CLOSE SP_F"],
    "SP_ITER": [
        "SP_N = 0",
        "SP_L:",
        "SP_R = F$SEARCH(P1, 7)",
        'IF SP_R .EQS. "" THEN GOTO SP_X',
        'SP_T == SP_T + F$EXTRACT(SP_BL, 255, F$PARSE(SP_R,,,"DIRECTORY")) + '
        'F$PARSE(SP_R,,,"NAME") + F$PARSE(SP_R,,,"TYPE") + F$PARSE(SP_R,,,"VERSION") + " "',
        "SP_N = SP_N + 1",
        "IF SP_N .LT. 30 THEN GOTO SP_L",
        "SP_X:",
        "EXIT 1",
    ],
    "SP_DIR": ["SET NOON", "DIRECTORY 'P1'", "SP_S = $STATUS", "EXIT 'SP_S'"],
}


def it(spec):
    return ['CALL SP_ITER "%s"' % spec]


CASES = [
    # [...] : the default directory and every directory below it
    ("SRCH.ALL.TMP", it("[...]*.TMP")),
    ("SRCH.ALL.DAT", it("[...]*.DAT")),
    ("SRCH.ALL.WILD", it("[...]*.*;*")),
    ("SRCH.ALL.VERSIONS", it("[...]PUR.DAT;*")),
    ("SRCH.ALL.NAME", it("[...]B1.TMP")),
    ("SRCH.ALL.NAMEWILD", it("[...]*1.TMP")),
    ("SRCH.ALL.PERCENT", it("[...]%1.TMP")),
    ("SRCH.ALL.TOPNAME", it("[...]TOP.*")),
    # a named directory and what is below it
    ("SRCH.SUB.A", it("[.A...]*.TMP")),
    ("SRCH.SUB.AB", it("[.A.B...]*.TMP")),
    ("SRCH.SUB.C", it("[.C...]*.*")),
    # an ellipsis ahead of a name, and a wildcard directory with an ellipsis
    ("SRCH.END.B", it("[...B]*.TMP")),
    ("SRCH.END.C", it("[...C]*.TMP")),
    ("SRCH.STAR.ELL", it("[.*...]*.TMP")),
    # the search ends: nothing matches, or the directory does not exist
    ("SRCH.NONE.NAME", it("[...]NOMATCH.TMP")),
    ("SRCH.NONE.DIR", it("[.NODIR...]*.TMP")),
    # DIRECTORY over the tree: its status when something matches and when nothing does
    ("SRCH.DIR.TMP", ["CALL SP_DIR [...]*.TMP"]),
    ("SRCH.DIR.NONE", ["CALL SP_DIR [...]NOMATCH.TMP"]),
    # COPY / PURGE / DELETE over the tree, then the search proves the result
    ("SRCH.COPY", ["COPY [.A...]*.DAT [.C]", 'CALL SP_ITER "[.C]*.DAT"']),
    ("SRCH.PURGE", ["PURGE [...]*.DAT", 'CALL SP_ITER "[...]PUR.DAT;*"']),
    ("SRCH.DELETE", ["DELETE [...]*.TMP;*", 'CALL SP_ITER "[...]*.TMP"',
                     'CALL SP_ITER "[...]*.DAT"']),
    # remove the tree
    ("SRCH.CLEANUP", [
        "SET NOON",
        "SET DEFAULT [-]",
        "DELETE [.SRCHT.A.B]*.*;*",
        "DELETE [.SRCHT.A]*.*;*",
        "DELETE [.SRCHT.C]*.*;*",
        "DELETE [.SRCHT]*.*;*",
        "SET PROTECTION=(S:RWED,O:RWED,G:RWED,W:RWED) [.SRCHT.A]B.DIR,[.SRCHT]A.DIR,[.SRCHT]C.DIR,[]SRCHT.DIR",
        "DELETE [.SRCHT.A]B.DIR;1",
        "DELETE [.SRCHT]A.DIR;1,C.DIR;1",
        "DELETE []SRCHT.DIR;1",
        'CALL SP_ITER "[...]SRCHT.DIR"',
    ]),
]
