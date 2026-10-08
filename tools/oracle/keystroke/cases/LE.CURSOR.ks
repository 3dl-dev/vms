@case LE.CURSOR
@title line editing: ^H (start of line), ^E (end), ^D / ^F and the arrow keys move within the line; insert mode is the default
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
