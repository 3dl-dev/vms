@case ERR.DCL
@title common DCL error messages
@mask "[A-Z0-9$_]+:\[[A-Z0-9$_.]+\]" "<DEV:[DIR]>"
E1 send "FOO\r" expect="\$ $"
E2 send "SHOW FOO\r" expect="\$ $"
E3 send "SHOW TIME/BAD\r" expect="\$ $"
E4 send "TYPE NOSUCH.TXT\r" expect="\$ $"
E5 send "DIRECTORY NOSUCH.TXT\r" expect="\$ $"
E6 send "WRITE SYS$OUTPUT\r" expect="(\$|:) $"
E6Z send "^Z" expect="\$ $"
E7 send "@NOSUCH\r" expect="\$ $"
E8 send "SET DEFAULT NOSUCHDEV:[X]\r" expect="\$ $"
E8R send "SET DEFAULT SYS$LOGIN\r" expect="\$ $" quiet
E9 send "X = 1 +\r" expect="\$ $"
E10 send "WRITE SYS$OUTPUT F$NOSUCH()\r" expect="\$ $"
