@case CC.NOECHO
@title carriage control probe: NOECHO terminal, empty command and a record
N1 send "SET TERMINAL/NOECHO\r" expect="\$ $"
N2 send "\r" expect="\$ $"
N3 send "\r" expect="\$ $"
N4 type "WRITE SYS$OUTPUT \"A\"\r" gap=0.05 expect="\$ $"
N5 type "SET TERMINAL/ECHO\r" gap=0.05 expect="\$ $"
N6 send "\r" expect="\$ $"
