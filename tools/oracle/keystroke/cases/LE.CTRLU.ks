@case LE.CTRLU
@title line editing: ^U deletes back to the start of the line
T type "WRITE SYS$OUTPUT 99" gap=0.05
U send "^U"
F type "WRITE SYS$OUTPUT 1" gap=0.05
R send "\r" expect="\$ $"
