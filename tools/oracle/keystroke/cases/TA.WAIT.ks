@case TA.WAIT
@title type-ahead: a command typed while DCL is busy (WAIT) is not echoed until DCL reads it
C send "WAIT 0:0:4\r" settle=0.3
T type "WRITE SYS$OUTPUT 4712" gap=0.08 settle=0
Q wait 0.5 settle=0
D wait 5 expect="\$ " settle=1
R send "\r" expect="\$ $" settle=1
