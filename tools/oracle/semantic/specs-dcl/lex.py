# Semantic-oracle DCL spec: LEXICAL FUNCTIONS (rd vms-8d1).
# F$ functions with inputs whose answers do not depend on the system they run
# on: strings (F$LENGTH F$EXTRACT F$LOCATE F$ELEMENT F$EDIT), conversions
# (F$INTEGER F$STRING F$TYPE F$CVSI F$CVUI), F$FAO, F$CVTIME of a fixed time,
# F$PARSE fields of a fixed spec, F$MESSAGE, F$MODE / F$PRIVILEGE / F$USER, and
# the error paths (a bad argument, an out-of-range position).
# Each case: (CASE-ID, DCL expression). comgen.py turns this into SP_LEX.COM,
# which assigns the expression to a symbol and prints the $STATUS, the symbol's
# F$TYPE and its value -- an error leaves the value empty.
SETUP = [
    'SP_S1 = "The quick brown fox"',
    'SP_LIST = "red,green,,blue"',
    'SP_T = "7-OCT-2026 13:45:56.78"',
]
CASES = [
    # strings
    ("LEX.LENGTH", 'F$LENGTH(SP_S1)'),
    ("LEX.LENGTH.EMPTY", 'F$LENGTH("")'),
    ("LEX.EXTRACT", 'F$EXTRACT(4, 5, SP_S1)'),
    ("LEX.EXTRACT.PAST_END", 'F$EXTRACT(16, 10, SP_S1)'),
    ("LEX.EXTRACT.START_PAST", 'F$EXTRACT(40, 3, SP_S1)'),
    ("LEX.EXTRACT.NEGSTART", 'F$EXTRACT(-1, 3, SP_S1)'),
    ("LEX.EXTRACT.NEGLEN", 'F$EXTRACT(0, -1, SP_S1)'),
    ("LEX.LOCATE", 'F$LOCATE("brown", SP_S1)'),
    ("LEX.LOCATE.MISSING", 'F$LOCATE("zebra", SP_S1)'),
    ("LEX.LOCATE.EMPTY", 'F$LOCATE("", SP_S1)'),
    ("LEX.ELEMENT.0", 'F$ELEMENT(0, ",", SP_LIST)'),
    ("LEX.ELEMENT.2", 'F$ELEMENT(2, ",", SP_LIST)'),
    ("LEX.ELEMENT.3", 'F$ELEMENT(3, ",", SP_LIST)'),
    ("LEX.ELEMENT.PAST", 'F$ELEMENT(9, ",", SP_LIST)'),
    ("LEX.ELEMENT.LONGDELIM", 'F$ELEMENT(0, ",,", SP_LIST)'),
    ("LEX.EDIT.UPCASE", 'F$EDIT("  mixed Case  ", "UPCASE")'),
    ("LEX.EDIT.TRIM", 'F$EDIT("  a  b  ", "TRIM")'),
    ("LEX.EDIT.COMPRESS", 'F$EDIT("  a    b  ", "COMPRESS")'),
    ("LEX.EDIT.COLLAPSE", 'F$EDIT("  a  b  ", "COLLAPSE")'),
    ("LEX.EDIT.UNCOMMENT", 'F$EDIT("abc ! comment", "UNCOMMENT")'),
    ("LEX.EDIT.MULTI", 'F$EDIT("  x  y ! c", "TRIM,UPCASE,UNCOMMENT")'),
    ("LEX.EDIT.BADKEY", 'F$EDIT("x", "NOSUCHKEY")'),
    # conversions
    ("LEX.INTEGER", 'F$INTEGER("42")'),
    ("LEX.INTEGER.HEX", 'F$INTEGER("%X1F")'),
    ("LEX.INTEGER.OCTAL", 'F$INTEGER("%O17")'),
    ("LEX.INTEGER.NEG", 'F$INTEGER("-7")'),
    ("LEX.INTEGER.TRUE", 'F$INTEGER("TRUE")'),
    ("LEX.INTEGER.YES", 'F$INTEGER("YES")'),
    ("LEX.INTEGER.JUNK", 'F$INTEGER("12AB")'),
    ("LEX.STRING", 'F$STRING(-12)'),
    ("LEX.STRING.STR", 'F$STRING("abc")'),
    ("LEX.TYPE.INT", 'F$TYPE(SP_S1) + "/" + F$TYPE(3) + "/" + F$TYPE(SP_NOT_DEFINED)'),
    ("LEX.TYPE.NUMSTR", 'F$TYPE("12")'),
    ("LEX.CVSI", 'F$CVSI(0, 4, "%X0F")'),
    ("LEX.CVUI", 'F$CVUI(0, 8, "A")'),
    # F$FAO
    ("LEX.FAO.BASIC", 'F$FAO("!UL item!%S at !XL", 2, 255)'),
    ("LEX.FAO.STRINGS", 'F$FAO("[!AS][!5AS][!2AS]", "ab", "cd", "efgh")'),
    ("LEX.FAO.PAD", 'F$FAO("!5UL|!5SL|!5ZL", 42, -3, 7)'),
    ("LEX.FAO.REPEAT", 'F$FAO("!3*- !2(UL )", 4, 5)'),
    ("LEX.FAO.UIC", 'F$FAO("!%U", %X00010004)'),
    # time
    ("LEX.CVTIME.ABS", 'F$CVTIME(SP_T)'),
    ("LEX.CVTIME.WEEKDAY", 'F$CVTIME(SP_T,, "WEEKDAY")'),
    ("LEX.CVTIME.DATE", 'F$CVTIME(SP_T, "ABSOLUTE", "DATE")'),
    ("LEX.CVTIME.MONTH", 'F$CVTIME(SP_T, "ABSOLUTE", "MONTH")'),
    ("LEX.CVTIME.COMPARISON", 'F$CVTIME(SP_T, "COMPARISON")'),
    ("LEX.CVTIME.HUNDREDTH", 'F$CVTIME(SP_T,, "HUNDREDTH")'),
    ("LEX.CVTIME.DELTA", 'F$CVTIME("7-OCT-2026 13:45:56.78+1-02:00",, "DATETIME")'),
    ("LEX.CVTIME.BAD", 'F$CVTIME("32-OCT-2026")'),
    # file specs and messages
    ("LEX.PARSE.NAME", 'F$PARSE("SP_X.DAT;3",,, "NAME")'),
    ("LEX.PARSE.TYPE", 'F$PARSE("SP_X.DAT;3",,, "TYPE")'),
    ("LEX.PARSE.VERSION", 'F$PARSE("SP_X.DAT;3",,, "VERSION")'),
    ("LEX.PARSE.DEFTYPE", 'F$PARSE("SP_X", ".COM",, "TYPE")'),
    ("LEX.PARSE.SYNTAX", 'F$PARSE("A[B",,, "NAME", "SYNTAX_ONLY")'),
    ("LEX.MESSAGE.NORMAL", 'F$MESSAGE(1)'),
    ("LEX.MESSAGE.ACCVIO", 'F$MESSAGE(12)'),
    ("LEX.MESSAGE.FNF", 'F$MESSAGE(%X18292)'),
    ("LEX.MESSAGE.TEXT", 'F$MESSAGE(%X1C, "TEXT")'),
    ("LEX.MESSAGE.UNKNOWN", 'F$MESSAGE(%X0FFF8012)'),
    # process
    ("LEX.MODE", 'F$MODE()'),
    ("LEX.USER", 'F$USER()'),
    ("LEX.PRIVILEGE.HELD", 'F$PRIVILEGE("SYSPRV")'),
    ("LEX.PRIVILEGE.BAD", 'F$PRIVILEGE("NOSUCHPRIV")'),
    ("LEX.GETJPI.USERNAME", 'F$GETJPI("", "USERNAME")'),
    ("LEX.GETJPI.BADITEM", 'F$GETJPI("", "NOSUCHITEM")'),
    ("LEX.GETDVI.NLA0", 'F$GETDVI("NLA0:", "DEVCLASS")'),
    ("LEX.GETDVI.EXISTS", 'F$GETDVI("SP_NO_SUCH_DEVICE:", "EXISTS")'),
    ("LEX.TRNLNM.MISSING", 'F$TRNLNM("SP_NO_SUCH_LOGICAL")'),
    ("LEX.TRNLNM.TABLE", 'F$TRNLNM("LNM$FILE_DEV", "LNM$SYSTEM_DIRECTORY",,,, "TABLE")'),
    # the error paths
    ("LEX.NOSUCHFUNC", 'F$NOSUCHFUNC(1)'),
    ("LEX.WRONGTYPE", 'F$LENGTH(5)'),
    ("LEX.TOOMANY", 'F$LENGTH("a", "b")'),
]
