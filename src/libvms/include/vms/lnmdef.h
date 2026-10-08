/*
 * vms/lnmdef.h - logical name item codes and attributes (vms-38b).
 *
 * The values an OpenVMS Alpha V8.4 node defines (docs/oracle/alpha84-starlet-defs/LNMDEF.txt),
 * every one; tests/integration/test_oracle_constants.sh checks them. Constants
 * only -- no structures, so a DEC C client sees nothing OVMX-specific.
 */
#ifndef __VMS_LNMDEF_H
#define __VMS_LNMDEF_H

#define LNM$M_NO_ALIAS               1
#define LNM$M_CONFINE                2
#define LNM$M_CRELOG                 4
#define LNM$M_TABLE                  8
#define LNM$M_CONCEALED              256
#define LNM$M_TERMINAL               512
#define LNM$M_EXISTS                 1024
#define LNM$M_SHAREABLE              65536
#define LNM$M_CLUSTERWIDE            131072
#define LNM$M_DCL_REQUEST            262144
#define LNM$M_CREATE_IF              16777216
#define LNM$M_CASE_BLIND             33554432
#define LNM$M_INTERLOCKED            67108864
#define LNM$M_LOCAL_ACTION           134217728
#define LNM$S_LNMDEF                 4
#define LNM$V_NO_ALIAS               0
#define LNM$V_CONFINE                1
#define LNM$V_CRELOG                 2
#define LNM$V_TABLE                  3
#define LNM$V_CONCEALED              8
#define LNM$V_TERMINAL               9
#define LNM$V_EXISTS                 10
#define LNM$V_SHAREABLE              16
#define LNM$V_CLUSTERWIDE            17
#define LNM$V_DCL_REQUEST            18
#define LNM$V_CREATE_IF              24
#define LNM$V_CASE_BLIND             25
#define LNM$V_INTERLOCKED            26
#define LNM$V_LOCAL_ACTION           27
#define LNM$C_TABNAMLEN              31
#define LNM$C_NAMLENGTH              255
#define LNM$C_MAXDEPTH               10
#define LNM$_INDEX                   1
#define LNM$_STRING                  2
#define LNM$_ATTRIBUTES              3
#define LNM$_TABLE                   4
#define LNM$_LENGTH                  5
#define LNM$_ACMODE                  6
#define LNM$_MAX_INDEX               7
#define LNM$_PARENT                  8
#define LNM$_LNMB_ADDR               9
#define LNM$_AGENT_ACMODE            10
#define LNM$_CHAIN                   -1

#endif /* __VMS_LNMDEF_H */
