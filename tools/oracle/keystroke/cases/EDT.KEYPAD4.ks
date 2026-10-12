@case EDT.KEYPAD4
@title EDT keypad on a VT100: TAB, LINEFEED (delete word back), PAGE, ENTER alone, typing over the end of a long line
@mask "[A-Z0-9$_]+:\[[A-Z0-9$_.]+\]" "<DEV:[DIR]>"
V0 send "SET TERMINAL/DEVICE_TYPE=VT100/NOHARDCOPY/WIDTH=80/PAGE=24\r" expect="\$ $" quiet
V1 send "EDIT/EDT KSPG.TXT\r" expect="\*$" timeout=30 quiet
C send "CHANGE\r" settle=3
E send "{KP2}" settle=1
T send "\t" settle=1
X type "AB" settle=1
J send "\n" settle=1
P send "{KP7}" settle=2
P2 send "{KP7}" settle=2
EN send "{ENTER}" settle=1
B send "{KP5}" settle=1
P3 send "{KP7}" settle=2
Z send "^Z" expect="\*$" settle=2
Q send "QUIT\r" expect="\$ $" timeout=30
V9 send "SET TERMINAL/DEVICE_TYPE=UNKNOWN/LINE_EDITING\r" expect="\$ $" quiet
