@case LE.OVERSTRIKE
@title line editing: ^A toggles insert/overstrike for the rest of the line
# A video terminal, like OVMX's console: the lab VAX's OPA0: is a 132-column hardcopy LA36; OVMX's is an 80-column screen (rd vms-eda8, operator ruling 2026-10-09).
V0 send "SET TERMINAL/NOHARDCOPY/WIDTH=80\r" expect="\$ $" quiet
T type "WRITE SYS$OUTPUT 1X3" gap=0.05
L1 send "^D"
L2 send "^D"
A send "^A"
O type "2"
R send "\r" expect="\$ $"
