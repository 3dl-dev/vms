@case TA.USERNAME
@title type-ahead: the user name typed in the same burst as the RETURN that wakes the line, before Username: is shown
@start loggedout
U send "\r{USER}\r" expect="Password: $" timeout=60 settle=1.5
P type "{PASSWORD}\r" gap=0.05 expect="\n.?\$ $" timeout=120 settle=3 quiet
