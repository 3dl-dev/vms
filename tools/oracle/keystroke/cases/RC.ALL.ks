@case RC.ALL
@title RECALL/ALL lists the recall buffer, RECALL n brings one back for editing, RECALL/ERASE empties it
E0 send "RECALL/ERASE\r" expect="\$ $" quiet
C1 send "WRITE SYS$OUTPUT \"R1\"\r" expect="\$ $"
C2 send "WRITE SYS$OUTPUT \"R2\"\r" expect="\$ $"
A send "RECALL/ALL\r" expect="\$ $"
N send "RECALL 2\r" settle=1
U send "^U" settle=1
E send "RECALL/ERASE\r" expect="\$ $"
A2 send "RECALL/ALL\r" expect="\$ $"
