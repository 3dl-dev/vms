@case CC.PROMPT
@title carriage control probe: SET PROMPT with and without carriage control
P1 send "SET PROMPT=\"X> \"\r" expect="X> $"
P2 send "\r" expect="X> $"
P3 send "WRITE SYS$OUTPUT \"A\"\r" expect="X> $"
P4 send "SET PROMPT=\"Y> \"/NOCARRIAGE_CONTROL\r" expect="Y> $"
P5 send "\r" expect="Y> $"
P6 send "WRITE SYS$OUTPUT \"A\"\r" expect="Y> $"
P7 send "SET PROMPT\r" expect="\$ $"
P8 send "\r" expect="\$ $"
