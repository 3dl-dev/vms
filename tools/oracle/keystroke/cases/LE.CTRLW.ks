@case LE.CTRLW
@title line editing: ^W at the prompt
T type "WRITE SYS$OUTPUT 1" gap=0.05
W send "^W"
R send "\r" expect="\$ $"
