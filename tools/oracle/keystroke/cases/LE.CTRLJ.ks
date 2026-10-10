@case LE.CTRLJ
@title line editing: LINEFEED (^J) deletes the word to the left
# A video terminal, like OVMX's console: the lab VAX's OPA0: is a 132-column hardcopy LA36; OVMX's is an 80-column screen (rd vms-eda8, operator ruling 2026-10-09).
V0 send "SET TERMINAL/NOHARDCOPY/WIDTH=80\r" expect="\$ $" quiet
T type "WRITE SYS$OUTPUT 1 BOGUS" gap=0.05
J send "^J"
R send "\r" expect="\$ $"
