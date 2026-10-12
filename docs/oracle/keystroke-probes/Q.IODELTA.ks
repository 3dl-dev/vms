@case Q.IODELTA
@title probe: JPI$_BUFIO / JPI$_DIRIO deltas across a fixed command sequence
S0 send "SET DEFAULT SYS$LOGIN\r" expect="\$ $" quiet
A send "$ B=F$GETJPI(\"\",\"BUFIO\")\r" expect="\$ $"
A2 send "$ D=F$GETJPI(\"\",\"DIRIO\")\r" expect="\$ $"
A3 send "$ WRITE SYS$OUTPUT F$GETJPI(\"\",\"BUFIO\")-B,\" \",F$GETJPI(\"\",\"DIRIO\")-D\r" expect="\$ $"
W1 send "$ B=F$GETJPI(\"\",\"BUFIO\")\r" expect="\$ $"
W2 send "$ D=F$GETJPI(\"\",\"DIRIO\")\r" expect="\$ $"
W3 send "WRITE SYS$OUTPUT \"X\"\r" expect="\$ $"
W4 send "$ WRITE SYS$OUTPUT F$GETJPI(\"\",\"BUFIO\")-B,\" \",F$GETJPI(\"\",\"DIRIO\")-D\r" expect="\$ $"
T1 send "$ B=F$GETJPI(\"\",\"BUFIO\")\r" expect="\$ $"
T2 send "$ D=F$GETJPI(\"\",\"DIRIO\")\r" expect="\$ $"
T3 send "TYPE KSPG.COM\r" expect="\$ $"
T4 send "$ WRITE SYS$OUTPUT F$GETJPI(\"\",\"BUFIO\")-B,\" \",F$GETJPI(\"\",\"DIRIO\")-D\r" expect="\$ $"
