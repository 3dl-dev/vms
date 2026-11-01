#!/bin/bash
# TEST: TELNET and FTP refuse an unrecognized qualifier before the client is entered (vms-097).
# EXPECT: contains:%DCL-W-IVQUAL
# EXPECT: contains: \ZQBOGUS\
# EXPECT_NOT: contains:%TELNET-I-TRYING
# EXPECT_NOT: contains:%FTP-E-NOOP
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'TELNET/ZQBOGUS 127.0.0.1\nFTP/ZQBOGUS 127.0.0.1\n' | $VMSDCL 2>&1
