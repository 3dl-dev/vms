@case TA.USERNAME
@title type-ahead: the user name typed in the same burst as the RETURN that wakes the line, before Username: is shown
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
@start loggedout
U send "\r{USER}\r" expect="Password: $" timeout=60 settle=1.5
P type "{PASSWORD}\r" gap=0.05 expect="\n.?\$ $" timeout=120 settle=3 quiet
