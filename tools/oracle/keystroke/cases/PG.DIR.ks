@case PG.DIR
@title output paging: DIRECTORY/PAGE of 30 files on a 24-line screen
@mask "[A-Z0-9$_]+:\[[A-Z0-9$_.]+\]" "<DEV:[DIR]>"
V0 send "SET TERMINAL/NOHARDCOPY/WIDTH=80/PAGE=24\r" expect="\$ $" quiet
D send "DIRECTORY/PAGE [.KSPGD]\r" settle=4
R1 send "\r" settle=3
R2 send "\r" settle=3
R3 send "\r" settle=3
C send "DIRECTORY/PAGE/COLUMNS=1 [.KSPGD]\r" settle=4
C1 send "\r" settle=3
C2 send "\r" settle=3
C3 send "\r" settle=3
