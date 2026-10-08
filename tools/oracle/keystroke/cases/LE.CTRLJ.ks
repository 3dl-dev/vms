@case LE.CTRLJ
@title line editing: LINEFEED (^J) deletes the word to the left
T type "WRITE SYS$OUTPUT 1 BOGUS" gap=0.05
J send "^J"
R send "\r" expect="\$ $"
