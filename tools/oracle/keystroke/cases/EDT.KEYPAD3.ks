@case EDT.KEYPAD3
@title EDT keypad on a VT100: OPEN LINE, DEL EOL, CHNGCASE, APPEND, REPLACE, WORD, LINE backward, CHAR, UND C after DELETE
@mask "[A-Z0-9$_]+:\[[A-Z0-9$_.]+\]" "<DEV:[DIR]>"
V0 send "SET TERMINAL/DEVICE_TYPE=VT100/NOHARDCOPY/WIDTH=80/PAGE=24\r" expect="\$ $" quiet
V1 send "DELETE KSE4.TXT;*\r" expect="\$ $" quiet
V2 send "EDIT/EDT KSE4.TXT\r" expect="\*$" timeout=30 quiet
V3 send "INSERT\r" settle=1 quiet
V4 type "one two three\r" gap=0.05 settle=0.5 quiet
V5 type "four five\r" gap=0.05 settle=0.5 quiet
V6 send "^Z" expect="\*$" quiet
V7 send "1\r" expect="\*$" quiet
C send "CHANGE\r" settle=3
W1 send "{KP1}" settle=1
W2 send "{KP1}" settle=1
O send "{PF1}{KP0}" settle=1
DE send "{PF1}{KP2}" settle=1
CH send "{PF1}{KP1}" settle=1
S send "{KPDOT}" settle=1
S2 send "{KP1}" settle=1
AP send "{KP9}" settle=1
D send "{DOWN}" settle=1
S3 send "{KPDOT}{KP1}" settle=1
RP send "{PF1}{KP9}" settle=1
B send "{KP5}{KP0}" settle=1
B2 send "{KP0}" settle=1
CR send "{KP4}{KP3}" settle=1
DL send "\x7f" settle=1
UC send "{PF1}{KPCOMMA}" settle=1
E send "{KP2}" settle=1
T type "!" settle=1
Z send "^Z" expect="\*$" settle=2
TW send "TYPE WHOLE\r" expect="\*$"
Q send "QUIT\r" expect="\$ $" timeout=30
V9 send "SET TERMINAL/DEVICE_TYPE=UNKNOWN\r" expect="\$ $" quiet
