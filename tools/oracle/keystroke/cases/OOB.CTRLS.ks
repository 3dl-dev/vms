@case OOB.CTRLS
@title ^S suspends output in flight (nothing arrives while held); ^Q resumes it
@stream "LINE +[0-9]+"
G send "@KSLOOP\r" settle=0 quiet
W wait 2 settle=0 quiet
S send "^S" settle=0 quiet
H0 wait 0.8 settle=0 quiet
H wait 2.5 settle=0
Q send "^Q" settle=0 quiet
QW wait 1.5 settle=0 quiet
Y send "^Y" expect="\$ $" tail="*INTERRUPT*"
X send "STOP\r" expect="\$ $"
