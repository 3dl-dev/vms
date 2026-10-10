@case EDT.LINE
@title EDT line mode: a new file, INSERT, TYPE WHOLE, EXIT
@mask "[A-Z0-9$_]+:\[[A-Z0-9$_.]+\]" "<DEV:[DIR]>"
V0 send "SET TERMINAL/NOHARDCOPY/WIDTH=80/PAGE=24\r" expect="\$ $" quiet
O send "EDIT/EDT KSE.TXT\r" expect="\*$" timeout=30
I send "INSERT\r" settle=1
A type "ALPHA\r" gap=0.05 settle=0.5
B type "BETA\r" gap=0.05 settle=0.5
Z send "^Z" expect="\*$"
T send "TYPE WHOLE\r" expect="\*$"
X send "EXIT\r" expect="\$ $" timeout=30
