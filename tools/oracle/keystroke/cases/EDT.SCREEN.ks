@case EDT.SCREEN
@title EDT keypad (screen) mode on a VT100: CHANGE, arrows, insert, back to line mode, QUIT
@mask "[A-Z0-9$_]+:\[[A-Z0-9$_.]+\]" "<DEV:[DIR]>"
V0 send "SET TERMINAL/DEVICE_TYPE=VT100/NOHARDCOPY/WIDTH=80/PAGE=24\r" expect="\$ $" quiet
O send "EDIT/EDT KSE.TXT\r" expect="\*$" timeout=30
C send "CHANGE\r" settle=3
D send "{DOWN}" settle=1
R send "{RIGHT}" settle=1
X type "X" settle=1
Z send "^Z" expect="\*$" settle=2
Q send "QUIT\r" expect="\$ $" timeout=30
V1 send "SET TERMINAL/DEVICE_TYPE=UNKNOWN\r" expect="\$ $" quiet
