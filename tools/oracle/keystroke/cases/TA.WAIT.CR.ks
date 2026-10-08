@case TA.WAIT.CR
@title type-ahead: a whole command line (with RETURN) typed during WAIT runs after it, echoed only then
C send "WAIT 0:0:4\r" settle=0.3
T type "WRITE SYS$OUTPUT 4713\r" gap=0.08 settle=0
Q wait 0.5 settle=0
D wait 5 expect="4713.*\$ $" settle=1.5
