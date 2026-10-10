@case Q.SETDEF
@title probe: SET DEFAULT validation
A send "SET DEFAULT [.NONEXISTENT_QWERTY_DIR]\r" expect="\$ $"
B send "SHOW DEFAULT\r" expect="\$ $"
C send "SET DEFAULT NOSUCHDEV:[X]\r" expect="\$ $"
D send "SHOW DEFAULT\r" expect="\$ $"
E send "SET DEFAULT SYS$LOGIN\r" expect="\$ $"
F send "SET DEFAULT [A.B.C.D.E.F.G.H.I]\r" expect="\$ $"
G send "SET DEFAULT [\r" expect="\$ $"
H send "SET DEFAULT SYS$LOGIN\r" expect="\$ $"
