@case EDT.KEYPAD
@title EDT keypad on a VT100: cursor, DEL C/W/L and UND, SELECT CUT PASTE, FIND and FNDNXT, ADVANCE/BACKUP, ^Z to line mode
@mask "[A-Z0-9$_]+:\[[A-Z0-9$_.]+\]" "<DEV:[DIR]>"
V0 send "SET TERMINAL/DEVICE_TYPE=VT100/NOHARDCOPY/WIDTH=80/PAGE=24\r" expect="\$ $" quiet
V1 send "DELETE KSE3.TXT;*\r" expect="\$ $" quiet
V2 send "EDIT/EDT KSE3.TXT\r" expect="\*$" timeout=30 quiet
V3 send "INSERT\r" settle=1 quiet
V4 type "ALPHA BETA\r" gap=0.05 settle=0.5 quiet
V5 type "GAMMA\r" gap=0.05 settle=0.5 quiet
V6 type "DELTA EPSILON\r" gap=0.05 settle=0.5 quiet
V7 send "^Z" expect="\*$" quiet
V8 send "1\r" expect="\*$" quiet
C send "CHANGE\r" settle=3
R send "{RIGHT}{RIGHT}{RIGHT}" settle=1
X send "{DEL}" settle=1
T type "Z" settle=1
W send "{KPMINUS}" settle=1
U send "{PF1}{KPMINUS}" settle=1
L send "{PF4}" settle=1
UL send "{PF1}{PF4}" settle=1
CC send "{KPCOMMA}" settle=1
UC send "{PF1}{KPCOMMA}" settle=1
D send "{DOWN}" settle=1
S send "{KPDOT}" settle=1
S2 send "{RIGHT}{RIGHT}" settle=1
CUT send "{KP6}" settle=1
D2 send "{DOWN}" settle=1
P send "{PF1}{KP6}" settle=1
F send "{PF1}{PF3}" settle=2
F2 type "EPS" settle=1
F3 send "{ENTER}" settle=2
N send "{KP5}{PF3}" settle=2
A send "{KP4}{KP0}" settle=1
E send "{KP2}" settle=1
B send "{KP5}{KP1}" settle=1
Z send "^Z" expect="\*$" settle=2
TW send "TYPE WHOLE\r" expect="\*$"
Q send "QUIT\r" expect="\$ $" timeout=30
V9 send "SET TERMINAL/DEVICE_TYPE=UNKNOWN\r" expect="\$ $" quiet
