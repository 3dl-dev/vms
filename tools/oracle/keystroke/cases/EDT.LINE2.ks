@case EDT.LINE2
@title EDT line mode: line numbers after INSERT, RETURN and a number at '*', TYPE ranges, FIND, SUBSTITUTE, DELETE, errors, EXIT of an existing file
@mask "[A-Z0-9$_]+:\[[A-Z0-9$_.]+\]" "<DEV:[DIR]>"
V0 send "SET TERMINAL/NOHARDCOPY/WIDTH=80/PAGE=24\r" expect="\$ $" quiet
V1 send "DELETE KSE2.TXT;*\r" expect="\$ $" quiet
O send "EDIT/EDT KSE2.TXT\r" expect="\*$" timeout=30
I send "INSERT\r" settle=1
A type "ONE\r" gap=0.05 settle=0.5
B type "TWO\r" gap=0.05 settle=0.5
C type "THREE\r" gap=0.05 settle=0.5
Z send "^Z" expect="\*$"
X send "EXIT\r" expect="\$ $" timeout=30
O2 send "EDIT/EDT KSE2.TXT\r" expect="\*$" timeout=30
R1 send "\r" expect="\*$"
R2 send "\r" expect="\*$"
R3 send "\r" expect="\*$"
R4 send "\r" expect="\*$"
N1 send "1\r" expect="\*$"
I2 send "INSERT;NEW\r" expect="\*$"
T1 send "TYPE WHOLE\r" expect="\*$"
T2 send "TYPE 2:3\r" expect="\*$"
T3 send "TYPE 9\r" expect="\*$"
F1 send "FIND \"THR\"\r" expect="\*$"
F2 send "\"NOPE\"\r" expect="\*$"
S1 send "SUBSTITUTE/T/X/ WHOLE\r" expect="\*$"
D1 send "DELETE 2\r" expect="\*$"
T4 send "T W\r" expect="\*$"
E1 send "FROB\r" expect="\*$"
X2 send "EXIT\r" expect="\$ $" timeout=30
