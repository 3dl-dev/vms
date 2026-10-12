@case EDT.LINE3
@title EDT line mode: COPY, MOVE, WRITE, INCLUDE, TYPE ranges (BEGIN/END/REST/.), FIND, SET NUMBERS, SHOW BUFFER, an out-of-range line
@mask "[A-Z0-9$_]+:\[[A-Z0-9$_.]+\]" "<DEV:[DIR]>"
V1 send "DELETE KSE7.TXT;*,KSE8.TXT;*\r" expect="\$ $" quiet
O send "EDIT/EDT KSE7.TXT\r" expect="\*$" timeout=30
I send "INSERT\r" settle=1
A type "ONE\r" gap=0.05 settle=0.5
B type "TWO\r" gap=0.05 settle=0.5
C type "THREE\r" gap=0.05 settle=0.5
D type "FOUR\r" gap=0.05 settle=0.5
Z send "^Z" expect="\*$"
CP send "COPY 1:2 TO END\r" expect="\*$"
T1 send "TYPE WHOLE\r" expect="\*$"
MV send "MOVE 3 TO BEGIN\r" expect="\*$"
T2 send "TYPE WHOLE\r" expect="\*$"
WR send "WRITE KSE8.TXT 1:2\r" expect="\*$" timeout=30
IN send "INCLUDE KSE8.TXT END\r" expect="\*$" timeout=30
T3 send "TYPE BEGIN:3\r" expect="\*$"
T4 send "TYPE REST\r" expect="\*$"
T5 send "TYPE .\r" expect="\*$"
F1 send "FIND BEGIN\r" expect="\*$"
F2 send "\"FOUR\"\r" expect="\*$"
X1 send "TYPE 99\r" expect="\*$"
X2 send "DELETE 99\r" expect="\*$"
SB send "SHOW BUFFER\r" expect="\*$"
Q send "QUIT\r" expect="\$ $" timeout=30
