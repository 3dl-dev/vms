@case OOB.CTRLY
@title ^Y interrupts a running procedure; CONTINUE resumes it; ^C does the same with no ON CONTROL_C; STOP ends it
@stream "LINE +[0-9]+"
G send "@KSLOOP\r" settle=0 quiet
W wait 2 settle=0 quiet
Y send "^Y" expect="\$ $" tail="*INTERRUPT*"
K send "CONTINUE\r" settle=0 quiet
W2 wait 2 settle=0 quiet
C send "^C" expect="\$ $" tail="*INTERRUPT*"
S send "STOP\r" expect="\$ $"
