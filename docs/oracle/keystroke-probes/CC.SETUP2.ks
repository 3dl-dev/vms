@case CC.SETUP2
@title setup -- KSCC3.COM (record then READ/PROMPT), KSCC4.COM (record then INQUIRE)
S0 send "SET DEFAULT SYS$LOGIN\r" expect="\$ $" quiet
S1 send "DELETE KSCC3.COM;*,KSCC4.COM;*\r" expect="\$ $" quiet
S2 type "CREATE KSCC3.COM\r" gap=0.02 settle=2 quiet
S3 type "$ WRITE SYS$OUTPUT \"A\"\r" gap=0.02 settle=0.5 quiet
S4 type "$ READ/PROMPT=\"Z> \" SYS$COMMAND X\r" gap=0.02 settle=0.5 quiet
S5 send "^Z" expect="\$ $" quiet
S6 type "CREATE KSCC4.COM\r" gap=0.02 settle=2 quiet
S7 type "$ WRITE SYS$OUTPUT \"A\"\r" gap=0.02 settle=0.5 quiet
S8 type "$ INQUIRE Y \"Your name\"\r" gap=0.02 settle=0.5 quiet
S9 send "^Z" expect="\$ $" quiet
