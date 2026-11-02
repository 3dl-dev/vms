# Semantic-oracle DCL spec: SYMBOL TYPES (rd vms-d80).
# A DCL symbol is a STRING or an INTEGER, and the type is the symbol's own, not
# a guess from its value text: $STATUS and $SEVERITY, quoted assignments and
# READ/INQUIRE results are strings even when their characters spell a number.
# These cases measure, on the real system, what each type yields when used in
# an expression: F$TYPE and F$STRING of it, the arithmetic and string operators
# with operands of one type and of mixed types, the integer and string
# comparisons, and the logical operators over a string that spells a number.
# Each case: (CASE-ID, DCL expression); comgen.py prints $STATUS, the F$TYPE of
# the assigned result, and the result.
SETUP = [
    'SP_SN = "5"',
    'SP_SZ = "007"',
    'SP_SX = "%X10"',
    'SP_SA = "abc"',
    'SP_TR = "TRUE"',
    'SP_SY = "yes"',
    'SP_I = 2',
    'SP_IX = %X10',
    'SET NOON',
    'SP_STS = $STATUS',
    'SP_SEV = $SEVERITY',
]
CASES = [
    # $STATUS / $SEVERITY: what type they are, and what they yield
    ("SYM.STATUS.TYPE", 'F$TYPE(SP_STS)'),
    ("SYM.STATUS.VALUE", 'SP_STS'),
    ("SYM.STATUS.STRING", 'F$STRING(SP_STS)'),
    ("SYM.STATUS.INTEGER", 'F$INTEGER(SP_STS)'),
    ("SYM.STATUS.PLUS.INT", 'SP_STS + 1'),
    ("SYM.STATUS.PLUS.STR", 'SP_STS + "x"'),
    ("SYM.STATUS.NOT", '.NOT. SP_STS'),
    ("SYM.STATUS.AND", 'SP_STS .AND. 1'),
    ("SYM.STATUS.EQ", 'SP_STS .EQ. 1'),
    ("SYM.STATUS.DIRECT.TYPE", 'F$TYPE($STATUS)'),
    ("SYM.STATUS.DIRECT.STRING", 'F$STRING($STATUS)'),
    ("SYM.SEVERITY.TYPE", 'F$TYPE(SP_SEV)'),
    ("SYM.SEVERITY.VALUE", 'SP_SEV'),
    ("SYM.SEVERITY.PLUS.INT", 'SP_SEV + 1'),
    ("SYM.SEVERITY.PLUS.STR", 'SP_SEV + "x"'),
    # a quoted string that spells a decimal number
    ("SYM.STRNUM.TYPE", 'F$TYPE(SP_SN)'),
    ("SYM.STRNUM.VALUE", 'SP_SN'),
    ("SYM.STRNUM.PLUS.INT", 'SP_SN + 1'),
    ("SYM.STRNUM.PLUS.STR", 'SP_SN + SP_SN'),
    ("SYM.STRNUM.MINUS.INT", 'SP_SN - 1'),
    ("SYM.STRNUM.MINUS.STR", 'SP_SN - "5"'),
    ("SYM.STRNUM.TIMES", 'SP_SN * 2'),
    ("SYM.STRNUM.DIV", 'SP_SN / 2'),
    ("SYM.STRNUM.NEG", '-SP_SN'),
    ("SYM.STRNUM.EQ", 'SP_SN .EQ. 5'),
    ("SYM.STRNUM.EQS", 'SP_SN .EQS. "5"'),
    ("SYM.STRNUM.STRING", 'F$STRING(SP_SN)'),
    # leading zeros, a radix prefix, letters, truth words
    ("SYM.STRZERO.VALUE", 'SP_SZ'),
    ("SYM.STRZERO.PLUS.INT", 'SP_SZ + 1'),
    ("SYM.STRZERO.TYPE", 'F$TYPE(SP_SZ)'),
    ("SYM.STRHEX.VALUE", 'SP_SX'),
    ("SYM.STRHEX.TYPE", 'F$TYPE(SP_SX)'),
    ("SYM.STRHEX.PLUS.INT", 'SP_SX + 0'),
    ("SYM.STRHEX.STRING", 'F$STRING(SP_SX)'),
    ("SYM.STRALPHA.PLUS.INT", 'SP_SA + 1'),
    ("SYM.STRALPHA.TIMES", 'SP_SA * 1'),
    ("SYM.STRALPHA.TYPE", 'F$TYPE(SP_SA)'),
    ("SYM.STRTRUE.PLUS.INT", 'SP_TR + 0'),
    ("SYM.STRYES.TIMES", 'SP_SY * 1'),
    ("SYM.STRTRUE.NOT", '.NOT. SP_TR'),
    # integers mixed with strings
    ("SYM.INT.TYPE", 'F$TYPE(SP_I)'),
    ("SYM.INT.PLUS.STRNUM", 'SP_I + "3"'),
    ("SYM.INT.PLUS.STRALPHA", 'SP_I + "x"'),
    ("SYM.INT.EQS", 'SP_I .EQS. "2"'),
    ("SYM.INTHEX.VALUE", 'SP_IX'),
    ("SYM.INTHEX.TYPE", 'F$TYPE(SP_IX)'),
    ("SYM.INTHEX.STRING", 'F$STRING(SP_IX)'),
    # comparisons of two strings: integer vs string operators
    ("SYM.CMP.GT", '"10" .GT. "9"'),
    ("SYM.CMP.GTS", '"10" .GTS. "9"'),
    ("SYM.CMP.EQ.HEX", 'SP_SX .EQ. 16'),
    # the logical operators are bitwise on integers; precedence
    ("SYM.LOG.OR", '1 .OR. 2'),
    ("SYM.LOG.NOT.ZERO", '.NOT. 0'),
    ("SYM.LOG.PREC.NOT.AND", '.NOT. 1 .AND. 3'),
    ("SYM.LOG.PREC.CMP.AND", '1 .EQ. 1 .AND. 2 .EQ. 2'),
    ("SYM.LOG.PREC.ARITH.CMP", '1 + 1 .EQ. 2'),
    ("SYM.CMP.LTS.INT", '10 .LTS. 9'),
    # more string-to-integer conversions
    ("SYM.STRFALSE.PLUS.INT", '"FALSE" + 0'),
    ("SYM.STRNO.TIMES", '"no" * 1'),
    ("SYM.STRTRUE.TYPE", 'F$TYPE(SP_TR)'),
    ("SYM.STRNEG.PLUS.INT", '"-5" + 0'),
    ("SYM.STRSPACE.PLUS.INT", '" 5" + 1'),
    ("SYM.STREMPTY.PLUS.INT", '"" + 1'),
    ("SYM.STRMINUS.INT", '"15" - 5'),
    ("SYM.PAREN.CONCAT.TIMES", '("5" + "5") * 2'),
    ("SYM.LEFTASSOC.INT.STR", '1 + 2 + "x"'),
    ("SYM.LEFTASSOC.STR.INT", '"x" + 1 + 2'),
    # an assignment's result type
    ("SYM.ASSIGN.QUOTED", '"42"'),
    ("SYM.ASSIGN.SUM", '"4" + "2"'),
    ("SYM.ASSIGN.STRING.INT", 'F$STRING(40 + 2)'),
]
