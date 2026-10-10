@case PG.TYPE
@title output paging: TYPE/PAGE of a 40-line file on a 24-line screen, RETURN for the next page
V0 send "SET TERMINAL/NOHARDCOPY/WIDTH=80/PAGE=24\r" expect="\$ $" quiet
T send "TYPE/PAGE KSPG.TXT\r" settle=3
R1 send "\r" settle=3
R2 send "\r" settle=3
R3 send "\r" expect="\$ $" settle=2
