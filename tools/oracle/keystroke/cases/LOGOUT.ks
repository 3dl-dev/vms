@case LOGOUT
@title LOGOUT from the console: the logout line and the idle line after it
# The SYSTEM-IDENTIFICATION line before Username: is the machine's own identity,
# in its architecture's form: VAX V7.3 "VAX/VMS V7.3      node VAX1", Alpha V8.4
# " Welcome to OpenVMS (TM) Alpha Operating System, Version V8.4", OVMX (Alpha
# form, its own name, INV-0) " Welcome to the OpenVMX X86_64 Operating System,
# Version V0.7-13 (OpenVMS-compatible)". Masked to one token so the step compares
# what surrounds it -- the bell(s), the blank lines, the prompt. The line must be
# there and non-empty in one of those forms; a missing or blank one still differs
# (rd vms-bd71). It precedes any "Welcome to" mask: OVMX's line says "Welcome to
# the", the post-login SYS$WELCOME does not.
@mask "^(?: Welcome to the [^<]+|[A-Z]+/VMS V[0-9.]+ +node [A-Z0-9_$]+)(?=<CR>)" "<SYSTEM-IDENTIFICATION>"
@mask "[ 0-9][0-9]-[A-Z]{3}-[0-9]{4}( [0-9]{2}:[0-9]{2}(:[0-9]{2}(\.[0-9]{2})?)?)?" "<DATE>"
@mask "[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]{2})?" "<TIME>"
L send "LOGOUT\r" expect="logged out at" settle=3
R send "\r" expect="Username: $" settle=1
X send "^Z" settle=3
