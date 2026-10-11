@case CC.SETUP
@title setup -- KSCC2.COM writes two records
S0 send "SET DEFAULT SYS$LOGIN\r" expect="\$ $" quiet
S1 send "DELETE KSCC2.COM;*\r" expect="\$ $" quiet
S2 type "CREATE KSCC2.COM\r" gap=0.02 settle=2 quiet
S3 type "$ WRITE SYS$OUTPUT \"A\"\r" gap=0.02 settle=0.5 quiet
S4 type "$ WRITE SYS$OUTPUT \"B\"\r" gap=0.02 settle=0.5 quiet
S5 send "^Z" expect="\$ $" quiet
