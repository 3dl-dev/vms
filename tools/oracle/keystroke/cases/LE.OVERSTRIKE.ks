@case LE.OVERSTRIKE
@title line editing: ^A toggles insert/overstrike for the rest of the line
T type "WRITE SYS$OUTPUT 1X3" gap=0.05
L1 send "^D"
L2 send "^D"
A send "^A"
O type "2"
R send "\r" expect="\$ $"
