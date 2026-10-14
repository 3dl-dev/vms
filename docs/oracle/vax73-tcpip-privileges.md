# TCP/IP Services: the privilege SET ROUTE requires (real VAX V7.3)

rd vms-ec6. Captured 2026-10-14 in a disposable lab pod (`tcpp-ec6`, node VAX1,
OpenVMS VAX V7.3 with TCP/IP Services) by `tools/oracle/semantic/capture.py --dcl`,
logged in as SYSTEM. Each step enables only TMPMBX and NETMBX plus the named privilege
(`SET PROCESS/PRIVILEGE=(NOALL,TMPMBX,NETMBX,<P>)`), runs `TCPIP SET ROUTE`, then
re-enables all privileges, shows the route and removes it. Observed output of the real
system (clean-room Rule 8); nothing disassembled.

| privileges enabled | `TCPIP SET ROUTE 10.99.99.5/GATEWAY=10.2.1.254` | route present after |
|---|---|---|
| TMPMBX, NETMBX | `%SYSTEM-E-NOPRIV, insufficient privilege or object protection violation` | no (and no TCPIP$ROUTE record: the removal reports `-RMS-E-RNF`) |
| + OPER | accepted (no output) | yes (`AH 10.99.99.5 10.2.1.254`) |
| + SYSPRV | `%SYSTEM-E-NOPRIV` | no |
| + BYPASS | `%SYSTEM-E-NOPRIV` | no |
| + SYSNAM | `%SYSTEM-E-NOPRIV` | no |

**Rule:** TCP/IP route configuration requires OPER. SYSPRV, BYPASS and SYSNAM do not
grant it. A refused request changes neither the live table nor the route database.

Not measured here:
- `TCPIP SET INTERFACE` re-setting LO0 to its own address was accepted without any
  privilege. That is a no-op, so it says nothing about a real change.
- A socket bind to a port below 1024. That needs the TCPIP$INETDEF constants dumped
  for a probe, which is not done yet.

Also observed: `TCPIP SET ROUTE ... /NETWORK_MASK=255.255.255.0` is refused
`%CLI-W-NOVALU, value not allowed - remove value specification \NETWORK_MASK=\`.
OVMX's DCL accepts that form.

## Transcript

```
$ SET PROCESS/PRIVILEGE=(NOALL,TMPMBX,NETMBX)
$ TCPIP SET ROUTE 10.99.99.5/GATEWAY=10.2.1.254
%SYSTEM-E-NOPRIV, insufficient privilege or object protection violation
$ SET PROCESS/PRIVILEGE=ALL
$ TCPIP SHOW ROUTE 10.99.99.5
                             DYNAMIC
Type           Destination                           Gateway
AN    10.2.1.0/24                           10.2.1.1
$ TCPIP SET NOROUTE 10.99.99.5/GATEWAY=10.2.1.254
%TCPIP-E-ROUTEERROR, error processing route request
-TCPIP-E-DATAPROB, cannot access the TCPIP$ROUTE database; review additional  messages to correct problem
-TCPIP-W-NORECORD, information not found
-RMS-E-RNF, record not found
$ SET PROCESS/PRIVILEGE=(NOALL,TMPMBX,NETMBX,OPER)
$ TCPIP SET ROUTE 10.99.99.5/GATEWAY=10.2.1.254
$ SET PROCESS/PRIVILEGE=ALL
$ TCPIP SHOW ROUTE 10.99.99.5
                             DYNAMIC
Type           Destination                           Gateway
AN    10.2.1.0/24                           10.2.1.1
AH    10.99.99.5                            10.2.1.254
$ TCPIP SET NOROUTE 10.99.99.5/GATEWAY=10.2.1.254
$ SET PROCESS/PRIVILEGE=(NOALL,TMPMBX,NETMBX,SYSPRV)
$ TCPIP SET ROUTE 10.99.99.5/GATEWAY=10.2.1.254
%SYSTEM-E-NOPRIV, insufficient privilege or object protection violation
$ SET PROCESS/PRIVILEGE=ALL
$ TCPIP SHOW ROUTE 10.99.99.5
                             DYNAMIC
Type           Destination                           Gateway
AN    10.2.1.0/24                           10.2.1.1
$ TCPIP SET NOROUTE 10.99.99.5/GATEWAY=10.2.1.254
%TCPIP-E-ROUTEERROR, error processing route request
-TCPIP-E-DATAPROB, cannot access the TCPIP$ROUTE database; review additional  messages to correct problem
-TCPIP-W-NORECORD, information not found
-RMS-E-RNF, record not found
$ SET PROCESS/PRIVILEGE=(NOALL,TMPMBX,NETMBX,BYPASS)
$ TCPIP SET ROUTE 10.99.99.5/GATEWAY=10.2.1.254
%SYSTEM-E-NOPRIV, insufficient privilege or object protection violation
$ SET PROCESS/PRIVILEGE=ALL
$ TCPIP SHOW ROUTE 10.99.99.5
                             DYNAMIC
Type           Destination                           Gateway
AN    10.2.1.0/24                           10.2.1.1
$ TCPIP SET NOROUTE 10.99.99.5/GATEWAY=10.2.1.254
%TCPIP-E-ROUTEERROR, error processing route request
-TCPIP-E-DATAPROB, cannot access the TCPIP$ROUTE database; review additional  messages to correct problem
-TCPIP-W-NORECORD, information not found
-RMS-E-RNF, record not found
$ SET PROCESS/PRIVILEGE=(NOALL,TMPMBX,NETMBX,SYSNAM)
$ TCPIP SET ROUTE 10.99.99.5/GATEWAY=10.2.1.254
%SYSTEM-E-NOPRIV, insufficient privilege or object protection violation
$ SET PROCESS/PRIVILEGE=ALL
$ TCPIP SHOW ROUTE 10.99.99.5
                             DYNAMIC
Type           Destination                           Gateway
AN    10.2.1.0/24                           10.2.1.1
$ TCPIP SET NOROUTE 10.99.99.5/GATEWAY=10.2.1.254
%TCPIP-E-ROUTEERROR, error processing route request
-TCPIP-E-DATAPROB, cannot access the TCPIP$ROUTE database; review additional  messages to correct problem
-TCPIP-W-NORECORD, information not found
-RMS-E-RNF, record not found
```
