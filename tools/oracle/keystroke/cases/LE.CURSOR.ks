@case LE.CURSOR
@title line editing: ^H (start of line), ^E (end), ^D / ^F and the arrow keys move within the line; insert mode is the default
# A video terminal, like OVMX's console: the lab VAX's OPA0: is a 132-column hardcopy LA36; OVMX's is an 80-column screen (rd vms-eda8, operator ruling 2026-10-09).
V0 send "SET TERMINAL/NOHARDCOPY/WIDTH=80\r" expect="\$ $" quiet
T type "RITE SYS$OUTPUT 13" gap=0.05
H send "^H"
W type "W"
E send "^E"
D send "^D"
I type "2"
L send "{LEFT}"
G send "{RIGHT}"
F send "^F"
R send "\r" expect="\$ $"
