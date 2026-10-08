@case RC.RECALL
@title command recall: up-arrow and ^B recall the previous command; down-arrow walks forward
C1 send "WRITE SYS$OUTPUT 5\r" expect="\$ $"
C2 send "WRITE SYS$OUTPUT 6\r" expect="\$ $"
U1 send "{UP}"
U2 send "{UP}"
DN send "{DOWN}"
R1 send "\r" expect="\$ $"
B send "^B"
R2 send "\r" expect="\$ $"
