@case CC.EMPTY
@title carriage control probe: empty commands, one and two records, a procedure
E1 send "\r" expect="\$ $"
E2 send "\r" expect="\$ $"
W1 send "WRITE SYS$OUTPUT \"A\"\r" expect="\$ $"
W2 send "@KSCC2\r" expect="\$ $"
W3 send "WRITE SYS$OUTPUT \"\"\r" expect="\$ $"
