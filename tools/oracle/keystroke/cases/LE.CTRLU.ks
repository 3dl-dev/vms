@case LE.CTRLU
@title line editing: ^U deletes back to the start of the line
# A video terminal, like OVMX's console: the lab VAX's OPA0: is a hardcopy LA36 (rd vms-eda8, operator ruling 2026-10-09).
V0 send "SET TERMINAL/NOHARDCOPY\r" expect="\$ $" quiet
T type "WRITE SYS$OUTPUT 99" gap=0.05
U send "^U"
F type "WRITE SYS$OUTPUT 1" gap=0.05
R send "\r" expect="\$ $"
