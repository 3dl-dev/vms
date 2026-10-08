@case LOGOUT
@title LOGOUT from the console: the logout line and the idle line after it
@mask "[ 0-9][0-9]-[A-Z]{3}-[0-9]{4}( [0-9]{2}:[0-9]{2}(:[0-9]{2}(\.[0-9]{2})?)?)?" "<DATE>"
@mask "[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]{2})?" "<TIME>"
L send "LOGOUT\r" expect="logged out at" settle=3
R send "\r" expect="Username: $" settle=1
X send "^Z" settle=3
