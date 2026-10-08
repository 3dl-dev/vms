@case RD.PROMPT
@title IO$_READPROMPT: DCL READ/PROMPT and INQUIRE (prompt, echo, upcasing), then the value
R send "READ/PROMPT=\"Name: \" SYS$COMMAND X\r" expect="Name: $"
T type "abc def\r" gap=0.05 expect="\$ $"
S send "SHOW SYMBOL X\r" expect="\$ $"
I send "INQUIRE Y \"Your name\"\r" expect=": $"
J type "abc def\r" gap=0.05 expect="\$ $"
K send "SHOW SYMBOL Y\r" expect="\$ $"
