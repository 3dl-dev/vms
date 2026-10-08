@case RD.NOECHO
@title SET TERMINAL/NOECHO: typed characters are not echoed; /ECHO restores
N send "SET TERMINAL/NOECHO\r" expect="\$ $"
T type "WRITE SYS$OUTPUT 8\r" gap=0.05 expect="\$ $"
E type "SET TERMINAL/ECHO\r" gap=0.05 expect="\$ $"
