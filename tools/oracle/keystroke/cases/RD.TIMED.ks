@case RD.TIMED
@title IO$M_TIMED: READ/TIME_OUT expiring with nothing typed and with a partial line typed
Z send "DELETE/SYMBOL/LOCAL KST2\r" expect="\$ $" quiet
R send "READ/TIME_OUT=3/PROMPT=\"T: \" SYS$COMMAND KST1\r" settle=0 merge
W wait 5 expect="\$ $" settle=1
R2 send "READ/TIME_OUT=4/PROMPT=\"T: \" SYS$COMMAND KST2\r" expect="T: $" settle=0.3
P type "ab" gap=0.1 settle=0 merge
W2 wait 6 expect="\$ $" settle=1
S send "SHOW SYMBOL KST2\r" expect="\$ $"
