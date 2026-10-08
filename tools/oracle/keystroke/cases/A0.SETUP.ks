# Not compared (every step quiet): creates the two helper procedures the other
# cases use, in SYS$LOGIN, by typing them into DCL CREATE.
@case A0.SETUP
@title setup -- KSLOOP.COM (slow steady output) and KSBC.COM (delayed broadcast)
S0 send "SET DEFAULT SYS$LOGIN\r" expect="\$ $" quiet
S1 send "REPLY/DISABLE\r" expect="\$ $" quiet
S2 send "DELETE KSLOOP.COM;*,KSBC.COM;*\r" expect="\$ $" quiet
S3 type "CREATE KSLOOP.COM\r" gap=0.02 settle=2 quiet
S4 type "$ I = 0\r" gap=0.02 settle=0.5 quiet
S5 type "$ L: I = I + 1\r" gap=0.02 settle=0.5 quiet
S6 type "$ WRITE SYS$OUTPUT \"LINE \", I\r" gap=0.02 settle=0.5 quiet
S7 type "$ WAIT 0:0:0.25\r" gap=0.02 settle=0.5 quiet
S8 type "$ IF I .LT. 60 THEN GOTO L\r" gap=0.02 settle=0.5 quiet
S9 type "$ EXIT\r" gap=0.02 settle=0.5 quiet
S10 send "^Z" expect="\$ $" quiet
S11 type "CREATE KSBC.COM\r" gap=0.02 settle=2 quiet
S12 type "$ WAIT 0:0:4\r" gap=0.02 settle=0.5 quiet
S13 type "$ REPLY/USER=SYSTEM \"KSBC BROADCAST\"\r" gap=0.02 settle=0.5 quiet
S14 send "^Z" expect="\$ $" quiet
