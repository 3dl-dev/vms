@case Q.EDTNUM
@title probe: EDT line numbers for inserted lines -- line mode INSERT between lines, repeated, keypad RETURN, text at [EOB], RESEQUENCE
@mask "[A-Z0-9$_]+:\[[A-Z0-9$_.]+\]" "<DEV:[DIR]>"
V0 send "SET TERMINAL/DEVICE_TYPE=VT100/NOHARDCOPY/WIDTH=80/PAGE=24\r" expect="\$ $" quiet
V1 send "DELETE KSE5.TXT;*\r" expect="\$ $" quiet
V2 send "EDIT/EDT KSE5.TXT\r" expect="\*$" timeout=30
V3 send "INSERT\r" settle=1
V4 type "A\r" gap=0.05 settle=0.5
V5 type "B\r" gap=0.05 settle=0.5
V6 type "C\r" gap=0.05 settle=0.5
V7 send "^Z" expect="\*$"
I1 send "INSERT 2;X1\r" expect="\*$"
I2 send "INSERT 2;X2\r" expect="\*$"
I3 send "INSERT 2;X3\r" expect="\*$"
I4 send "INSERT 2\r" settle=1
I5 type "Y1\r" gap=0.05 settle=0.5
I6 type "Y2\r" gap=0.05 settle=0.5
I7 send "^Z" expect="\*$"
T1 send "TYPE WHOLE\r" expect="\*$"
E1 send "INSERT END;Z1\r" expect="\*$"
T2 send "TYPE WHOLE\r" expect="\*$"
C1 send "1\r" expect="\*$"
C send "CHANGE\r" settle=3
K1 send "{KP2}" settle=1
K2 send "\r" settle=1
K3 type "P" settle=1
K4 send "\r" settle=1
K5 type "Q" settle=1
K6 send "{PF1}{KP4}" settle=2
K7 type "R" settle=1
K8 send "\r" settle=1
K9 type "S" settle=1
KZ send "^Z" expect="\*$" settle=2
T3 send "TYPE WHOLE\r" expect="\*$"
R1 send "RESEQUENCE\r" expect="\*$"
T4 send "TYPE WHOLE\r" expect="\*$"
Q send "QUIT\r" expect="\$ $" timeout=30
V9 send "SET TERMINAL/DEVICE_TYPE=UNKNOWN\r" expect="\$ $" quiet
