# Semantic-oracle DCL spec: IF CONDITIONS (rd vms-c17a).
# What DCL's IF decides for conditions whose operands are themselves
# expressions: string concatenation and removal, integer arithmetic and its
# precedence, parentheses, lexical functions, and the logical operators around
# them. The DCL Dictionary's operator precedence puts the arithmetic and string
# operators (+ - * /) above the comparisons (.EQS. .EQ. ...), and those above
# .NOT., .AND. and .OR.; these cases measure that on the real system rather
# than assume it. Each case: (CASE-ID, IF condition). comgen.py (KIND "IF")
# runs IF <condition> THEN SP_V = "T" with SP_V preset to "F" and prints it.
KIND = "IF"
SETUP = [
    'SP_N = "VAX1"',
    'SP_FULL = "_VAX1$OPA0:"',
    'SP_I = 2',
]
CASES = [
    # string concatenation on either side of a string comparison
    ("IFX.CONCAT.RIGHT", 'SP_FULL .EQS. "_" + SP_N + "$OPA0:"'),
    ("IFX.CONCAT.LEFT", '"_" + SP_N + "$OPA0:" .EQS. SP_FULL'),
    ("IFX.CONCAT.NES", 'SP_FULL .NES. "_" + SP_N + "$OPA0:"'),
    ("IFX.CONCAT.LTS", '"a" + "b" .LTS. "ac"'),
    ("IFX.CONCAT.QUOTEDPLUS", '"a+b" .EQS. "a" + "+" + "b"'),
    ("IFX.REMOVE", '"abcdef" - "cd" .EQS. "abef"'),
    ("IFX.LEXICAL", 'F$EXTRACT(0, 1, SP_FULL) + "X" .EQS. "_X"'),
    # integer arithmetic on either side of an integer comparison
    ("IFX.ARITH.LEFT", 'SP_I + 1 .EQ. 3'),
    ("IFX.ARITH.RIGHT", '3 .EQ. SP_I + 1'),
    ("IFX.ARITH.MUL", '2 * SP_I .GT. 3'),
    ("IFX.ARITH.PREC", '1 + 2 * 3 .EQ. 7'),
    ("IFX.ARITH.PAREN", '(1 + 2) * 3 .EQ. 9'),
    ("IFX.ARITH.NEG", '-1 .LT. 0'),
    ("IFX.ARITH.MINUS", '5 - SP_I .EQ. 3'),
    # an integer result compared as a string
    ("IFX.INT.EQS", 'SP_I + 1 .EQS. "3"'),
    # the logical operators around expression operands
    ("IFX.AND", '"a" + "b" .EQS. "ab" .AND. 1 + 1 .EQ. 2'),
    ("IFX.OR", '"a" + "b" .EQS. "ba" .OR. SP_I * 2 .EQ. 4'),
    ("IFX.NOT.PAREN", '.NOT. ("a" + "b" .EQS. "ba")'),
    ("IFX.FALSE", '"a" + "b" .EQS. "a"'),
]
