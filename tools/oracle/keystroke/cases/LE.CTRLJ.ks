@case LE.CTRLJ
@title line editing: LINEFEED (^J) deletes the word to the left
# A video terminal, like OVMX's console: the lab VAX's OPA0: is a hardcopy LA36 (rd vms-eda8, operator ruling 2026-10-09).
V0 send "SET TERMINAL/NOHARDCOPY\r" expect="\$ $" quiet
T type "WRITE SYS$OUTPUT 1 BOGUS" gap=0.05
J send "^J"
R send "\r" expect="\$ $"
