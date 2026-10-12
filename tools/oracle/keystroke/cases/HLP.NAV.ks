@case HLP.NAV
@title HELP navigation on a 24-line screen: topic, subtopic, back out with RETURN
V0 send "SET TERMINAL/NOHARDCOPY/WIDTH=80/PAGE=24\r" expect="\$ $" quiet
H send "HELP RECALL\r" settle=3
S send "/ALL\r" settle=3
B1 send "\r" settle=3
B2 send "\r" settle=3
B3 send "\r" expect="\$ $" settle=2
