@case LE.CTRLW
@title line editing: ^W at the prompt
# A video terminal, like OVMX's console: the lab VAX's OPA0: is a hardcopy LA36 (rd vms-eda8, operator ruling 2026-10-09).
V0 send "SET TERMINAL/NOHARDCOPY\r" expect="\$ $" quiet
T type "WRITE SYS$OUTPUT 1" gap=0.05
W send "^W"
R send "\r" expect="\$ $"
