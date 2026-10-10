@case CC.READ
@title carriage control probe: READ/PROMPT and records after it
R1 send "READ/PROMPT=\"Z> \" SYS$COMMAND X\r" expect="Z> $"
R2 send "abc\r" expect="\$ $"
R3 send "READ/PROMPT=\"Z> \" SYS$COMMAND X\r" expect="Z> $"
R4 send "\r" expect="\$ $"
R5 send "P = F$FAO(\"!/Q> \")\r" expect="\$ $"
R6 send "READ/PROMPT='P' SYS$COMMAND X\r" expect="Q> $"
R7 send "x\r" expect="\$ $"
R8 send "WRITE SYS$OUTPUT F$FAO(\"!/A!/B\")\r" expect="\$ $"
