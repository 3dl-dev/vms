
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <assert.h>
#include <rms.h>
#include <dvidef.h>
#include <descrip.h>
#include <efndef.h>
#include <lkidef.h>
#include <string.h>
#include <builtins.h>
#include <prdef.h>
#include <iledef.h>
#include <iosbdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

struct pid_list {
    struct pid_list *next_pid;
    unsigned int pid;
};


/******************************************************************************/
static int get_lock_info (unsigned int *pid,
                          struct dsc$descriptor_s *resnam_d,
                          char *interesting) {
/*
** This routine is run in EXECUTIVE MODE, and obtains all the locks currently
** on the system, one per call.  It returns the resource name, the pid that
** holds the lock, and an "interesting" flag, indicating that the lock is
** granted or converting.
**
** Note: Errors in executive mode will crash your process...
*/

unsigned __int64 ps;

static IOSB iosb;

static int r0_status;
static unsigned int lockid = 0xffffffff;    /* -1 */
static unsigned short int ret_len;

static char state[3];

static ILE3 lkiitms[] = { 4, LKI$_PID, NULL, NULL,
                          0, LKI$_RESNAM, NULL, &ret_len,
                          3, LKI$_STATE, &state, NULL,
                          0, 0, NULL, NULL };

    /*
    ** Get the processor status
    */
    ps = __PAL_RD_PS();

    /*
    ** Verify write access to the our arguments from the previous mode
    */
    if ((__PAL_PROBEW (pid, 4, ps & PR$M_PS_PRVMOD) == 0) ||
        (__PAL_PROBEW (resnam_d, 4, ps & PR$M_PS_PRVMOD) == 0) ||
        (__PAL_PROBEW (resnam_d->dsc$a_pointer,
                       resnam_d->dsc$w_length,
                       ps & PR$M_PS_PRVMOD) == 0)) {
        return SS$_ACCVIO;
    }

    /*
    ** Fill in the rest of the item list for the GETLKI call.
    */
    lkiitms[0].ile3$ps_bufaddr = pid;
    lkiitms[1].ile3$w_length = resnam_d->dsc$w_length;
    lkiitms[1].ile3$ps_bufaddr = resnam_d->dsc$a_pointer;
    ret_len = 0;

    /*
    ** Get the lock information.
    */
    r0_status = sys$getlkiw (EFN$C_ENF,
                             &lockid,
                             &lkiitms,
                             &iosb,
                             0,
                             0,
                             0);
    errchk_ret (r0_status);

    /*
    ** Fix the length of the resource name in the descriptor.
    */
    if (ret_len <= resnam_d->dsc$w_length) {
        resnam_d->dsc$w_length = ret_len;
    }

    if ((state[2] == LKI$C_GRANTED) ||
        (state[2] == LKI$C_CONVERT)) {
        *interesting = TRUE;
    } else {
        *interesting = FALSE;
    }

    return iosb.iosb$w_status;
}


/******************************************************************************/
static void add_pid (struct pid_list **pid_list, unsigned int pid) {
/*
** Do an insertion sort into a list of PIDs.
*/

static struct pid_list *pid_p;
static struct pid_list *new_pid_p;
static struct pid_list *last_pid_p;

    pid_p = *pid_list;
    last_pid_p = NULL;
    while (pid_p != NULL) {
        if (pid_p->pid == pid) {
            return;
        }
        if (pid_p->pid > pid) {
            break;
        }
        last_pid_p = pid_p;
        pid_p = pid_p->next_pid;
    }

    new_pid_p = calloc (1, sizeof (struct pid_list));
    assert (new_pid_p != NULL);
    new_pid_p->pid = pid;
    new_pid_p->next_pid = NULL;

    if (last_pid_p != NULL) {
        new_pid_p->next_pid = last_pid_p->next_pid;
        last_pid_p->next_pid = new_pid_p;
    } else {
        *pid_list = new_pid_p;
    }
}


/******************************************************************************/
int main (void) {
/*
** Given a device and filename, this code finds all the process IDs with
** granted (or converting) RMS locks on that file by calling $GETLKIW from
** executive mode.
**
** This code understands the format of the RMS file lock, which is
** undocumented.  In other words, this resource name may change in future
** versions of OpenVMS.
**
** This program is very inefficient, as it's getting all the locks on the
** system so I have something to demo sys$cmexec with.  You can obviously
** use more appropriate input to $GETLKI to do this job more efficently.
** Note that $getlki doesn't have to be called from executive mode, you
** can call it from user mode.
**
** You need CMEXEC privilege to run this program.
*/

static IOSB iosb;

static struct FAB fab;
static struct NAM nam;

static struct {
    char rms[4];
    unsigned short int filnum;
    unsigned short int seqnum;
    unsigned short int rvnum;
    char lock_name[64];
} resource, test_resource;

static struct dsc$descriptor_s test_resource_d = { 0,
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              (void *)&test_resource };

static struct dsc$descriptor_s resource_d = { 0,
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              (void *)&resource };

static struct pid_list *pid_list = NULL;
static struct pid_list *temp_p;

static int r0_status;
static unsigned int pid;
static int count;

static unsigned short int lock_len;

static char filename[NAM$C_MAXRSS];
static char esa[NAM$C_MAXRSS];
static char interesting;

static ILE3 dviitms[] =
        { 64, DVI$_DEVLOCKNAM, resource.lock_name, &lock_len,
          0, 0, NULL, NULL };

static struct {
    unsigned int count;
    unsigned int *pid;
    struct dsc$descriptor_s *resnam_d;
    char *interesting;
} exec_args = { 3, &pid, &test_resource_d, &interesting };

static struct dsc$descriptor_s filename_d = { sizeof (filename),
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              filename };

static struct dsc$descriptor_s device_d = { 0,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            NULL };

static $DESCRIPTOR (prompt_d, "Filename: ");


    /*
    ** Get the name of the file from the user.
    */
    r0_status = lib$get_foreign (&filename_d,
                                 &prompt_d,
                                 &filename_d.dsc$w_length);
    if (r0_status == RMS$_EOF) {
        exit (EXIT_SUCCESS);
    } else {
        errchk_sig (r0_status);
    }

    if (filename_d.dsc$w_length == 0) {
        (void)printf ("Filename cannot be null\n");
        exit (EXIT_FAILURE);
    }

    /*
    ** Set up a FAB and NAM to retrieve the device and FID for this file.
    */
    fab = cc$rms_fab;
    fab.fab$l_fna = filename;
    fab.fab$b_fns = filename_d.dsc$w_length;
    fab.fab$l_nam = &nam;

    nam = cc$rms_nam;
    nam.nam$l_esa = esa;
    nam.nam$b_ess = NAM$C_MAXRSS;

    r0_status = sys$parse (&fab);
    errchk_sig (r0_status);

    r0_status = sys$search (&fab);
    if (r0_status == RMS$_FNF) {
        (void)printf ("File %-.*s not found\n",
                      filename_d.dsc$w_length,
                      filename_d.dsc$a_pointer);
        exit (EXIT_FAILURE);
    } else {
        errchk_sig (r0_status);
    }

    /*
    ** Set up a descriptor containing the device spec.  It's stored in the
    ** name as a counted string.
    */
    device_d.dsc$w_length = nam.nam$t_dvi[0];
    device_d.dsc$a_pointer = &nam.nam$t_dvi[1];

    /*
    ** Get the lock name for this disk.
    */
    r0_status = sys$getdviw (EFN$C_ENF,
                             0,
                             &device_d,
                             dviitms,
                             &iosb,
                             0,
                             0,
			     0);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$w_status);

    /*
    ** Assemble the lock resource name.  It's RMS$ followed by the FID,
    ** followed by the lock name of the device on which the file resides.
    */
    (void)memcpy (resource.rms, "RMS$", 4);
    resource.filnum = nam.nam$w_fid[0];
    resource.seqnum = nam.nam$w_fid[1];
    resource.rvnum = nam.nam$w_fid[2];
    resource_d.dsc$w_length = 4 + 6 + lock_len;

    while (r0_status != SS$_NOMORELOCK) {
        /*
        ** Get every lock on the system.
        */
        test_resource_d.dsc$w_length = sizeof (test_resource);
        r0_status = sys$cmexec ((int (*)())&get_lock_info,
                                (unsigned int *)&exec_args);
        if (r0_status == SS$_NOMORELOCK) {
            continue;
        } else {
            errchk_sig (r0_status);
        }

        if (interesting) {
            if ((resource_d.dsc$w_length == test_resource_d.dsc$w_length) &&
                (memcmp (&resource,
                         &test_resource,
                         resource_d.dsc$w_length) == 0)) {
                add_pid (&pid_list, pid);
            }
        }
    }

    count = 0;
    while (pid_list != NULL) {
        (void)printf ("%08x\n", pid_list->pid);
        count++;
        temp_p = pid_list;
        pid_list = pid_list->next_pid;
        free (temp_p);
    }
    if (count == 0) {
        (void)printf ("No RMS locks found for that file\n");
    }
}
