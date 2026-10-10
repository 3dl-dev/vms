# Not compared (every step quiet): the files the paging and editor cases use, in SYS$LOGIN.
@case A1.SETUP2
@title setup -- KSPG.TXT (40 lines) and an empty slate for KSE.TXT
S0 send "SET DEFAULT SYS$LOGIN\r" expect="\$ $" quiet
S1 send "DELETE KSPG.TXT;*,KSPG.COM;*,KSE.TXT;*\r" expect="\$ $" quiet
S2 type "CREATE KSPG.COM\r" gap=0.02 settle=2 quiet
S3 type "$ OPEN/WRITE F KSPG.TXT\r" gap=0.02 settle=0.5 quiet
S4 type "$ I = 0\r" gap=0.02 settle=0.5 quiet
S5 type "$ L:\r" gap=0.02 settle=0.5 quiet
S6 type "$ I = I + 1\r" gap=0.02 settle=0.5 quiet
S7 type "$ WRITE F \"PAGE LINE \", I\r" gap=0.02 settle=0.5 quiet
S8 type "$ IF I .LT. 40 THEN GOTO L\r" gap=0.02 settle=0.5 quiet
S9 type "$ CLOSE F\r" gap=0.02 settle=0.5 quiet
S10 send "^Z" expect="\$ $" quiet
S11 send "@KSPG\r" expect="\$ $" timeout=60 quiet
