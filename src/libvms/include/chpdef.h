/*
 * CHPDEF.H - $CHKPRO / $CHECK_ACCESS / $CREATE_USER_PROFILE item codes and flags
 *
 * Values as OpenVMS VAX V7.3 SYS$LIBRARY:STARLET.MLB defines them
 * (docs/oracle/vax73-starlet-defs/CHPDEF.txt, captured from the real node;
 * clean-room, nothing disassembled).
 */

#ifndef __CHPDEF_H
#define __CHPDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Item codes */
#define CHP$_END             0
#define CHP$_ACCESS          1   /* access wanted (ARM$M_ mask) */
#define CHP$_FLAGS           2   /* CHP$M_OBSERVE / ALTER / USEREADALL ... */
#define CHP$_PRIV            3   /* the subject's privileges (quadword) */
#define CHP$_ACMODE          4
#define CHP$_ACCLASS         5
#define CHP$_CLASS           5
#define CHP$_RIGHTS          6   /* the subject's rights (quadwords {id, attributes}) */
#define CHP$_ADD_RIGHTS      7
#define CHP$_ADDRIGHTS       7
#define CHP$_MODE            8
#define CHP$_MODES           9
#define CHP$_MIN_CLASS       10
#define CHP$_MINCLASS        10
#define CHP$_MAX_CLASS       11
#define CHP$_MAXCLASS        11
#define CHP$_OWNER           12  /* the object's owner UIC */
#define CHP$_PROT            13  /* the object's protection code */
#define CHP$_ACL             14  /* the object's ACL (ACEs back to back) */
#define CHP$_AUDIT_NAME      15
#define CHP$_AUDITNAME       15
#define CHP$_ALARM_NAME      16
#define CHP$_ALARMNAME       16
#define CHP$_MATCHED_ACE     17
#define CHP$_MATCHEDACE      17
#define CHP$_PRIVUSED        18
#define CHP$_AUDIT_LIST      19
#define CHP$_OBJECT_NAME     20
#define CHP$_OBJECT_CLASS    21
#define CHP$_UIC             22  /* the subject's UIC */
#define CHP$_OBJECT_SPECIFIC 23
#define CHP$_MAX_CODE        24

#define CHP$K_MATCHED_ACE_LENGTH 255
#define CHP$K_ALARM_LENGTH   768
#define CHP$K_AUDIT_LENGTH   1560

/* CHP$_PRIVUSED bits */
#define CHP$M_SYSPRV         0x00000001
#define CHP$M_BYPASS         0x00000002
#define CHP$M_UPGRADE        0x00000004
#define CHP$M_DOWNGRADE      0x00000008
#define CHP$M_GRPPRV         0x00000010
#define CHP$M_READALL        0x00000020
#define CHP$M_OPER           0x00000040
#define CHP$M_GRPNAM         0x00000080
#define CHP$M_SYSNAM         0x00000100
#define CHP$M_GROUP          0x00000200
#define CHP$M_WORLD          0x00000400
#define CHP$M_PRMCEB         0x00000800
#define CHP$K_NUMBER_OF_PRIVS 12

/* CHP$_FLAGS bits */
#define CHP$M_OBSERVE        0x00000001
#define CHP$M_ALTER          0x00000002
#define CHP$M_READ           0x00000001
#define CHP$M_WRITE          0x00000002
#define CHP$M_USEREADALL     0x00000004
#define CHP$M_AUDIT          0x00000008
#define CHP$M_NOFAILAUD      0x00000010
#define CHP$M_NOSUCCAUD      0x00000020
#define CHP$M_DELETE         0x00000040
#define CHP$M_MANDATORY      0x00000080
#define CHP$M_FLUSH          0x00000100
#define CHP$M_CREATE         0x00000200
#define CHP$M_INTERNAL       0x00000400
#define CHP$M_SERVER         0x00000800

/* $CREATE_USER_PROFILE flags */
#define CHP$M_NOACCESS       0x00000001
#define CHP$M_REMDUPID       0x00000002
#define CHP$M_INCSYSID       0x00000004
#define CHP$M_INCIMGID       0x00000008
#define CHP$M_DEFPRIV        0x00000010
#define CHP$M_DEFCLASS       0x00000020

#ifdef __cplusplus
}
#endif

#endif /* __CHPDEF_H */
