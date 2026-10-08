@case TA.CTRLX
@title ^X purges the type-ahead buffer: a line typed during WAIT then ^X never runs
C send "WAIT 0:0:4\r" settle=0.3
T type "WRITE SYS$OUTPUT 4714\r" gap=0.05 settle=0
X send "^X" settle=0
D wait 5 expect="\$ " settle=2
R send "\r" expect="\$ $" settle=1
