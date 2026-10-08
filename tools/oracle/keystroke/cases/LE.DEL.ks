@case LE.DEL
@title line editing: DELETE rubs out the last character
T type "WRITE SYS$OUTPUT 12X" gap=0.05
D1 send "^?"
D2 send "^?"
F type "23"
R send "\r" expect="\$ $"
