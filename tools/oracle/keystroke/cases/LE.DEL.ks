@case LE.DEL
@title line editing: DELETE rubs out the last character
# A video terminal, like OVMX's console: the lab VAX's OPA0: is a 132-column hardcopy LA36; OVMX's is an 80-column screen (rd vms-eda8, operator ruling 2026-10-09).
V0 send "SET TERMINAL/NOHARDCOPY/WIDTH=80\r" expect="\$ $" quiet
T type "WRITE SYS$OUTPUT 12X" gap=0.05
D1 send "^?"
D2 send "^?"
F type "23"
R send "\r" expect="\$ $"
