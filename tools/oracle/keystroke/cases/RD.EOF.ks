@case RD.EOF
@title ^Z at a READ: end of file
R send "READ/PROMPT=\"Z: \" SYS$COMMAND X\r" expect="Z: $"
Z send "^Z" expect="\$ $"
