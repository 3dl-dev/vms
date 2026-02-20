
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <descrip.h>
#include <string.h>
#include <armdef.h>
#include <nsadef.h>
#include <efndef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

/*
** Note you need the AUDIT privilege to run this code.  The code will write
** an audit record in your audit log.  Obviously, if you have some policy
** about audit records and what can and can't write them, don't run this code.
**
** The best way to demo the code is to ensure you have security alarms
** switched on and then do a $ REPLY/ENABLE=SECURITY/TEMP so you can see the
** alarm as the code is run.  Else you can do an $ ANALYSE/AUDIT to get the
** alarm from the audit log.
*/

static int r0_status;
static int final_status = SS$_NORMAL;
static unsigned int audit_flags = NSA$M_NOEVTCHECK || NSA$M_FLUSH;
static unsigned int event_type = NSA$C_MSG_OBJ_ACCESS;
static unsigned int sub_type = NSA$C_OBJ_ACCESS;
static unsigned int access = ARM$M_READ;

static char security[] = "SECURITY";
static char obj_class[] = "DEVICE";
static char obj_name[] = "SYS$SYSDEVICE:";

static struct {
    short int length;
    short int func;
    void *addr;
    void *retlen;
} aeitms[] =
        { sizeof (event_type),  NSA$_EVENT_TYPE,        &event_type,    NULL,
          sizeof (sub_type),    NSA$_EVENT_SUBTYPE,     &sub_type,      NULL,
          sizeof (security)-1,  NSA$_ALARM_NAME,        security,       NULL,
          sizeof (security)-1,  NSA$_AUDIT_NAME,        security,       NULL,
          sizeof (obj_class)-1, NSA$_OBJECT_CLASS,      obj_class,      NULL,
          sizeof (access),      NSA$_ACCESS_DESIRED,    &access,        NULL,
          sizeof (final_status),NSA$_FINAL_STATUS,      &final_status,  NULL,
          sizeof (obj_name)-1,  NSA$_DEVICE_NAME,       obj_name,       NULL,
          0,                    0,                      NULL,           NULL };

    r0_status = sys$audit_eventw (EFN$C_ENF,
                                  audit_flags,
                                  &aeitms,
                                  0,
                                  0,
                                  0);
    if (r0_status == SS$_NOAUDIT) {
        (void)printf ("You need the AUDIT privilege to run this code\n");
    } else {
        errchk_sig (r0_status);
    }
}
