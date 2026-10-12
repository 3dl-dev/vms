# Not compared (every step quiet): a directory of 30 files for the paging cases.
@case A2.SETUP3
@title setup -- [.KSPGD] with F1.TXT .. F30.TXT
S0 send "SET DEFAULT SYS$LOGIN\r" expect="\$ $" quiet
S1 send "CREATE/DIRECTORY [.KSPGD]\r" expect="\$ $" quiet
S2 send "DELETE [.KSPGD]*.*;*\r" expect="\$ $" quiet
S3 type "CREATE KSPGD.COM\r" gap=0.02 settle=2 quiet
S4 type "$ I = 0\r" gap=0.02 settle=0.5 quiet
S5 type "$ L:\r" gap=0.02 settle=0.5 quiet
S6 type "$ I = I + 1\r" gap=0.02 settle=0.5 quiet
S7 type "$ OPEN/WRITE F [.KSPGD]F'I'.TXT\r" gap=0.02 settle=0.5 quiet
S8 type "$ CLOSE F\r" gap=0.02 settle=0.5 quiet
S9 type "$ IF I .LT. 30 THEN GOTO L\r" gap=0.02 settle=0.5 quiet
S10 send "^Z" expect="\$ $" quiet
S11 send "@KSPGD\r" expect="\$ $" timeout=120 quiet
S12 type "CREATE KSPGS.COM\r" gap=0.02 settle=2 quiet
S13 type "$ I = 0\r" gap=0.02 settle=0.5 quiet
S14 type "$ L:\r" gap=0.02 settle=0.5 quiet
S15 type "$ I = I + 1\r" gap=0.02 settle=0.5 quiet
S16 type "$ KSPG'I' == I\r" gap=0.02 settle=0.5 quiet
S17 type "$ IF I .LT. 30 THEN GOTO L\r" gap=0.02 settle=0.5 quiet
S18 type "$ DELETE/SYMBOL I\r" gap=0.02 settle=0.5 quiet
S19 send "^Z" expect="\$ $" quiet
