@case LE.CTRLX
@title line editing: ^X at the prompt discards the line (and the type-ahead)
T type "WRITE SYS$OUTPUT 9" gap=0.05
X send "^X"
F type "WRITE SYS$OUTPUT 1" gap=0.05
R send "\r" expect="\$ $"
