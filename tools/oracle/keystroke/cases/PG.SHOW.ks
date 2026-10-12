@case PG.SHOW
@title output paging: SHOW SYMBOL has no /PAGE on V7.3 (IVQUAL)
V0 send "SET TERMINAL/NOHARDCOPY/WIDTH=80/PAGE=24\r" expect="\$ $" quiet
V2 send "@KSPGS\r" expect="\$ $" timeout=60 quiet
S send "SHOW SYMBOL/GLOBAL/PAGE KSPG*\r" settle=4
R1 send "\r" settle=3
R2 send "\r" settle=3
R3 send "\r" settle=3
