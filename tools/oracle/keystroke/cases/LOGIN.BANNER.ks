@case LOGIN.BANNER
@title login on the console: Username:, Password: (no echo), banner, first prompt
@start loggedout
@mask "Last (non-)?interactive login on [^<]*" "Last \1interactive login on <DATE>"
@mask "[ 0-9][0-9]-[A-Z]{3}-[0-9]{4}( [0-9]{2}:[0-9]{2}(:[0-9]{2}(\.[0-9]{2})?)?)?" "<DATE>"
@mask "[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]{2})?" "<TIME>"
@mask "Welcome to [^<]*" "Welcome to <PRODUCT>"
U send "\r" expect="Username: $" settle=1
N type "{USER}\r" gap=0.05 expect="Password: $" settle=1
P type "{PASSWORD}\r" gap=0.05 expect="\n.?\$ $" timeout=120 settle=3
