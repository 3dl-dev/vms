@case EDT.KEYPAD2
@title EDT keypad on a VT100, a 40-line file: split and join lines, typing at a line end, BOTTOM/TOP, SECT, scrolling down and up, ^U ^W BS, GOLD COMMAND
@mask "[A-Z0-9$_]+:\[[A-Z0-9$_.]+\]" "<DEV:[DIR]>"
V0 send "SET TERMINAL/DEVICE_TYPE=VT100/NOHARDCOPY/WIDTH=80/PAGE=24\r" expect="\$ $" quiet
V1 send "EDIT/EDT KSPG.TXT\r" expect="\*$" timeout=30
C send "CHANGE\r" settle=3
E send "{KP2}" settle=1
T type "XY" settle=1
R send "{RIGHT}" settle=1
S send "{LEFT}{LEFT}{LEFT}{LEFT}\r" settle=1
J send "\x7f" settle=1
BS send "\x08" settle=1
U send "^U" settle=1
UW send "{PF1}{KPMINUS}" settle=1
BOT send "{PF1}{KP4}" settle=2
TOP send "{PF1}{KP5}" settle=2
SEC send "{KP4}{KP8}" settle=2
DN send "{DOWN}" settle=1
DN2 send "{KP8}" settle=2
UP send "{KP5}{KP8}" settle=2
W send "^W" settle=2
CMD send "{PF1}{KP7}" settle=2
CMD2 type "TYPE 1" settle=1
CMD3 send "{ENTER}" settle=2
Z send "^Z" expect="\*$" settle=2
Q send "QUIT\r" expect="\$ $" timeout=30
V9 send "SET TERMINAL/DEVICE_TYPE=UNKNOWN\r" expect="\$ $" quiet
