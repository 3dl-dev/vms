@case OOB.CTRLO
@title ^O discards output in flight (and says so); a second ^O turns it back on
@stream "LINE +[0-9]+"
G send "@KSLOOP\r" settle=0 quiet
W wait 2 settle=0 quiet
O send "^O" settle=0 merge
OW wait 2 settle=0
O2 send "^O" settle=0 merge
OW2 wait 1.2 settle=0
Y send "^Y" expect="\$ $" tail="*INTERRUPT*"
S send "STOP\r" expect="\$ $"
