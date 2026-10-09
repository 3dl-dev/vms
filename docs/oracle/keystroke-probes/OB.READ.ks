@case OB.READ
@title OOB probe: ^Y inside READ/PROMPT and INQUIRE, and ^Z there
R1 send "READ/PROMPT=\"Z> \" SYS$COMMAND X\r" expect="Z> $"
R2 type "AB" gap=0.05
R3 send "^Y" expect="\$ $"
I1 send "INQUIRE Q \"Name\"\r" expect=": $"
I2 send "^Y" expect="\$ $"
