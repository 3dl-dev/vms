@case OOB.PROMPT
@title out-of-band characters at an idle prompt: ^Y ^C ^T (default and SET CONTROL=T) ^O
@mask "CPU=[0-9:.]+ PF=[0-9]+ IO=[0-9]+ MEM=[0-9]+" "CPU=<n> PF=<n> IO=<n> MEM=<n>"
@mask "[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]{2})?" "<TIME>"
@mask "^| [A-Z0-9]+::" "| <NODE>::"
Y send "^Y"
C send "^C"
T send "^T"
S send "SET CONTROL=T\r" expect="\$ $"
T2 send "^T"
O send "^O"
O2 send "^O"
P type "WRITE SYS$OUTPUT 3" gap=0.05
Y2 send "^Y"
