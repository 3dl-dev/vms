@case BC.READ
@title a broadcast that arrives while a line is half typed: shown, then the prompt and the partial line are redisplayed
@mask "[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]{2})?" "<TIME>"
@mask "process [A-Z0-9_$]+ spawned" "process <NAME> spawned"
@mask "on [A-Z0-9]+ from" "on <NODE> from"
@mask "on [A-Z0-9]+\." "on <NODE>."
@mask "at _[A-Z0-9]+\$OPA0:" "at <TERM>"
S send "SPAWN/NOWAIT/OUTPUT=NL: @KSBC\r" expect="\$ $" settle=0.5
T type "WRITE SYS$OUTPUT 7" gap=0.05 settle=0 merge
W wait 9 settle=0.5
R send "\r" expect="\$ $"
