#!/bin/bash
# usage: arun.sh NAME  -> MACRO + LINK + RUN SYS$SCRATCH:NAME
N=$1
$(dirname "$0")/aq.sh "MACRO/NOLIST/OBJECT=SYS\$SCRATCH:$N.OBJ SYS\$SCRATCH:$N.MAR" 15
$(dirname "$0")/aq.sh "LINK/EXECUTABLE=SYS\$SCRATCH:$N.EXE SYS\$SCRATCH:$N.OBJ" 15
$(dirname "$0")/aq.sh "RUN SYS\$SCRATCH:$N.EXE" ${2:-8}
