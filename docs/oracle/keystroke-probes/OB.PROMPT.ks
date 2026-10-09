@case OB.PROMPT
@title OOB probe: ^Y/^C/^O/^T at a prompt, mid-line, after a record, and with CONTROL disabled
@mask "CPU=[0-9:.]+ PF=[0-9]+ IO=[0-9]+ MEM=[0-9]+" "CPU=<n> PF=<n> IO=<n> MEM=<n>"
@mask "[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]{2})?" "<TIME>"
Y1 send "^Y" expect="\$ $"
W1 send "WRITE SYS$OUTPUT \"A\"\r" expect="\$ $"
Y2 type "ABC" gap=0.05
Y3 send "^Y" expect="\$ $"
E1 send "\r" expect="\$ $"
C1 type "ABC" gap=0.05
C2 send "^C" expect="\$ $"
T0 send "SET CONTROL=T\r" expect="\$ $"
T1 type "ABC" gap=0.05
T2 send "^T"
T3 send "D\r" expect="\$ $"
O1 send "^O"
O2 send "WRITE SYS$OUTPUT \"B\"\r" expect="\$ $"
O3 send "^O"
O4 send "WRITE SYS$OUTPUT \"C\"\r" expect="\$ $"
N0 send "SET NOCONTROL=Y\r" expect="\$ $"
N1 type "AB" gap=0.05
N2 send "^Y"
N3 send "^C"
N4 send "\r" expect="\$ $"
N5 send "SET CONTROL=(Y,T)\r" expect="\$ $"
