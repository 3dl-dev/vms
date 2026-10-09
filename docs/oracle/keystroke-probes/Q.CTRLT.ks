@case Q.CTRLT
@title probe: where does CONTROL=T come from on the lab node
A send "SEARCH SYS$LOGIN:LOGIN.COM,SYS$MANAGER:SYLOGIN.COM CONTROL\r" expect="\$ $"
B send "SET NOCONTROL=T\r" expect="\$ $"
C send "^T"
D send "SET CONTROL=T\r" expect="\$ $"
