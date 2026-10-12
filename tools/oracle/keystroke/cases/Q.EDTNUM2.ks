@case Q.EDTNUM2
@title probe: EDT numbers for text put in at [EOB] from the PASTE buffer (CUT/APPEND then PASTE/REPLACE), twice
@mask "[A-Z0-9$_]+:\[[A-Z0-9$_.]+\]" "<DEV:[DIR]>"
V0 send "SET TERMINAL/DEVICE_TYPE=VT100/NOHARDCOPY/WIDTH=80/PAGE=24\r" expect="\$ $" quiet
V1 send "DELETE KSE6.TXT;*\r" expect="\$ $" quiet
V2 send "EDIT/EDT KSE6.TXT\r" expect="\*$" timeout=30
V3 send "INSERT\r" settle=1
V4 type "one two\r" gap=0.05 settle=0.5
V5 type "three four\r" gap=0.05 settle=0.5
V7 send "^Z" expect="\*$"
C1 send "1\r" expect="\*$"
C send "CHANGE\r" settle=3
S send "{KPDOT}" settle=1
S1 send "{KP1}" settle=1
X send "{KP6}" settle=1
B send "{PF1}{KP4}" settle=1
P send "{PF1}{KP6}" settle=1
P2 send "{PF1}{KP6}" settle=1
U send "{PF1}{KP5}" settle=1
S2 send "{KPDOT}" settle=1
S3 send "{KP1}" settle=1
A send "{KP9}" settle=1
B2 send "{PF1}{KP4}" settle=1
P3 send "{PF1}{KP6}" settle=1
KZ send "^Z" expect="\*$" settle=2
T1 send "TYPE WHOLE\r" expect="\*$"
Q send "QUIT\r" expect="\$ $" timeout=30
R1 send "EDIT/EDT KSE6.TXT\r" expect="\*$" timeout=30
R2 send "1\r" expect="\*$"
R3 send "CHANGE\r" settle=3
R4 send "{KPDOT}" settle=1
R4A send "{KP1}" settle=1
R4B send "{KP6}" settle=1
R4C send "{PF1}{KP4}" settle=1
R4D send "{PF1}{KP6}" settle=1
R5 send "^Z" expect="\*$" settle=2
R6 send "TYPE WHOLE\r" expect="\*$"
R7 send "QUIT\r" expect="\$ $" timeout=30
V9 send "SET TERMINAL/DEVICE_TYPE=UNKNOWN\r" expect="\$ $" quiet
