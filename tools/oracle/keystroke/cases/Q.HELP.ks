@case Q.HELP
@title probe: HELP framing -- top level, a multi-level path, no-documentation, column layout, prompts
V0 send "SET TERMINAL/NOHARDCOPY/WIDTH=80/PAGE=0\r" expect="\$ $" quiet
A send "HELP RECALL /ALL\r" settle=3
A1 send "\r" settle=2
A2 send "\r" settle=2
A3 send "\r" settle=3
B send "HELP NOSUCHTOPICQ\r" settle=3
B1 send "\r" expect="\$ $" settle=2
C send "HELP SHOW\r" settle=4
C1 send "TIME\r" settle=3
C2 send "^Z" expect="\$ $" settle=2
D send "HELP SET TERMINAL\r" settle=4
D1 send "^Z" expect="\$ $" settle=2
E send "HELP\r" settle=4
E1 send "LOGOUT\r" settle=3
E2 send "\r" settle=2
E3 send "\r" expect="\$ $" settle=2
F send "HELP/NOPROMPT RECALL\r" expect="\$ $" settle=3
V1 send "SET TERMINAL/PAGE=24\r" expect="\$ $" quiet
