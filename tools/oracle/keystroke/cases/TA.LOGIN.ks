@case TA.LOGIN
@title type-ahead: a command typed while LOGINOUT is still logging in, before the first $ -- held unechoed, echoed after the prompt
@start loggedout
@mask "Last (non-)?interactive login on [^<]*" "Last \1interactive login on <DATE>"
@mask "[ 0-9][0-9]-[A-Z]{3}-[0-9]{4}( [0-9]{2}:[0-9]{2}(:[0-9]{2}(\.[0-9]{2})?)?)?" "<DATE>"
@mask "[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]{2})?" "<TIME>"
@mask "Welcome to [^<]*" "Welcome to <PRODUCT>"
U send "\r" expect="Username: $" settle=1 quiet
N type "{USER}\r" gap=0.05 expect="Password: $" settle=1 quiet
P send "{PASSWORD}\r" settle=0 merge
T type "WRITE SYS$OUTPUT 4711\r" gap=0.05 settle=0 merge
W wait 1 expect="4711.*\$ $" timeout=120 settle=3
