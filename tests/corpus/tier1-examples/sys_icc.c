
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

/*
** This example if fairly complex, but to demo the ICC services, I don't
** see how I can do it in any less code.
**
** This program contains both client and server code.  When the code runs,
** it attempts to take out a system lock on a resource, and if it succeeds,
** runs the server section of the code.  Else it runs the client section.
**
** The server discovers all the other nodes in the cluster that have the same
** architecture as the node running the server, and creates a process on each
** of them, which runs this code (as the client).
**
** The server section opens an association using ICC, and the clients connect
** to the association, send some data and an EOD, and exit.  Once the server
** sees that all clients have sent data, it shuts down.
**
** You need the SYSLCK and SYSNAM privileges to run this program.  The
** program will create output files in the directory the program resides in,
** so you need write access to that dir.  The two output files are called
** ICC_DEMO_CLNT.OUT_nodename and ICC_DEMO_CLNT.ERR_nodename, where nodename
** is a name of a node in the cluster.  If the program runs successfully,
** there should be one .OUT file for each node in the cluster, and no .ERRs ;-)
*/

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>
#include <string.h>
#include <descrip.h>
#include <efndef.h>
#include <fscndef.h>
#include <iccdef.h>
#include <jpidef.h>
#include <lckdef.h>
#include <lnmdef.h>
#include <prvdef.h>
#include <ssdef.h>
#include <stsdef.h>
#include <syidef.h>
#include <iosbdef.h>
#include <iledef.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

struct node_info {
    struct node_info *next_node;
    unsigned int client_pid;
    unsigned int conn_handle;
    char node_name[15+1];
    struct dsc$descriptor_s node_name_d;
    char seen_eod;
    char seen_disconnect;
};

#define END_BLOCK_TYPE 1
#define DATA_BLOCK_TYPE 2
#define MAXBUFSIZE 1023
#define ASSOC_NAME "DEMO_ASSOC_NAME"
#define TABLE_NAME "ICC$REGISTRY_TABLE"

/*
** Global listhead for nodes.
*/
static struct node_info *node_list = NULL;


/******************************************************************************/
static int determine_master (char *master) {
/*
** Attempt to get an exclusive mode lock on a resource.  If we get it,
** we are the master (ie, the server).
*/

static struct {
    unsigned short int status;
    unsigned short int reserved;
    unsigned int lock_id;
} lksb;

static int r0_status;

static $DESCRIPTOR (resource_d, "ICC_DEMO_MASTER");

    r0_status = sys$enqw (EFN$C_ENF,
                          LCK$K_EXMODE,
                          &lksb,
                          LCK$M_NOQUEUE|LCK$M_SYSTEM,
                          &resource_d,
                          0,
                          0,
                          0,
                          0,
                          0,
                          0,
                          0);
    if (r0_status == SS$_NOTQUEUED) {
        *master = FALSE;
    } else {
        errchk_sig (r0_status);
        errchk_sig (lksb.status);
        *master = TRUE;
    }
    return SS$_NORMAL;
}


/******************************************************************************/
static struct node_info *find_node (struct dsc$descriptor_s *node_d) {
/*
** Find a node on the list of nodes.
*/

static struct node_info *ni_p;

    ni_p = node_list;
    while (ni_p != NULL) {
        if ((node_d->dsc$w_length == ni_p->node_name_d.dsc$w_length) &&
            (memcmp (node_d->dsc$a_pointer,
                     ni_p->node_name_d.dsc$a_pointer,
                     node_d->dsc$w_length) == 0)) {
            break;
        }
        ni_p = ni_p->next_node;
    }

    return ni_p;
}


/******************************************************************************/
static struct node_info *add_node (struct dsc$descriptor_s *node_d) {
/*
** Add a node to the end of the linked list.
*/

static struct node_info *ni_p;
static struct node_info *last_node_p = NULL;

    ni_p = calloc (1, sizeof (struct node_info));
    assert (ni_p != NULL);

    memcpy (ni_p->node_name,
            node_d->dsc$a_pointer,
            node_d->dsc$w_length);
    ni_p->node_name_d.dsc$w_length = node_d->dsc$w_length;
    ni_p->node_name_d.dsc$a_pointer = ni_p->node_name;
    ni_p->next_node = NULL;
    ni_p->seen_eod = FALSE;
    ni_p->seen_disconnect = FALSE;

    /*
    ** Add the new node to the end of the linked list.
    */
    if (last_node_p == NULL) {
        node_list = ni_p;
        last_node_p = ni_p;
    } else {
        last_node_p->next_node = ni_p;
        last_node_p = ni_p;
    }
    return ni_p;
}


/******************************************************************************/
static struct node_info *get_node_list (void) {
/*
** Find all the nodes in the cluster.
*/

static IOSB iosb;

static struct node_info *ni_p;
static struct node_info *our_ni_p;

static int r0_status;
static unsigned int csid = -1; /* Nasty.  Poor def in new starlet causes this */
static int arch_type;
static int save_arch_type;

static char nodename[15+1];
static struct dsc$descriptor_s nodename_d = { 0,
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              nodename };


static ILE3 syiitms[] = 
    {  15, SYI$_NODENAME, nodename, &nodename_d.dsc$w_length,
        4, SYI$_ARCH_TYPE, &arch_type, NULL,
        0, 0, NULL, NULL };

    /*
    ** Get our node name and architecture type.
    */
    r0_status = sys$getsyiw (EFN$C_ENF,
                             0,
                             0,
                             syiitms,
                             &iosb,
                             0,
                             0);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$l_getxxi_status);

    /*
    ** Add this node to the linked list and save our architecture type.
    */
    our_ni_p = add_node (&nodename_d);
    save_arch_type = arch_type;

    /*
    ** Get info on all nodes in the cluster.
    */
    while (r0_status != SS$_NOMORENODE) {
        r0_status = sys$getsyiw (EFN$C_ENF,
                                 &csid,
                                 0,
                                 syiitms,
                                 &iosb,
                                 0,
                                 0);
        if (r0_status == SS$_NOMORENODE) {
            continue;
        } else {
            errchk_sig (r0_status);
        }
        errchk_sig (iosb.iosb$l_getxxi_status);

        /*
        ** For each node of the same architecture type, create a node info
        ** structure for it and add it to the linked list of nodes.
        */
        if (arch_type == save_arch_type) {
            if (find_node (&nodename_d) == NULL) {
                add_node (&nodename_d);
            }
        }
    }
    return our_ni_p;
}


/******************************************************************************/
static void client_send_hello (struct node_info *ni_p) {
/*
** Send a hello message to the server.
*/

static IOS_ICC ios_icc;

static IOSB iosb;

static int r0_status;

static char buffer[MAXBUFSIZE];

static $DESCRIPTOR (association_d, ASSOC_NAME);
static $DESCRIPTOR (table_d, TABLE_NAME);

    /*
    ** Format some stuff to put in the buffer.  This gets sent to the server
    ** unchanged, so you could use it to pass any info you want.
    */
    (void)sprintf (buffer,
                   "%-.*s (connecting on association %-.*s)",
                   ni_p->node_name_d.dsc$w_length,
                   ni_p->node_name,
                   association_d.dsc$w_length,
                   association_d.dsc$a_pointer);

    (void)printf ("Client %s sending connect request... ",
                  buffer);
    (void)fflush (stdout);

    r0_status = sys$icc_connectw (&ios_icc,
                                  0,
                                  0,
                                  ICC$C_DFLT_ASSOC_HANDLE,
                                  &ni_p->conn_handle,
                                  &association_d,
                                  0,
                                  0,
                                  buffer,
                                  strlen (buffer),
                                  0,
                                  0,
                                  0,
                                  0);
    errchk_sig (r0_status);
    errchk_sig (ios_icc.ios_icc$w_status);
    (void)printf ("success\n");

    /*
    ** Format a data message to send.
    */
    (void)sprintf (buffer, "%cHELLO!", DATA_BLOCK_TYPE);
    (void)printf ("Sending hello... ");
    (void)fflush (stdout);

    /*
    ** Send it.
    */
    r0_status = sys$icc_transmitw (ni_p->conn_handle,
                                   &ios_icc,
                                   0,
                                   0,
                                   buffer,
                                   strlen (buffer));
    errchk_sig (r0_status);
    errchk_sig (ios_icc.ios_icc$w_status);
    (void)printf ("success\n");
}


/******************************************************************************/
static void client_send_eod (struct node_info *ni_p) {
/*
** Send an end of data indicator.
*/

static int r0_status;
static unsigned int buffer = END_BLOCK_TYPE;
static IOS_ICC ios_icc;
static IOSB iosb;

    (void)printf ("Sending EOD... ");
    (void)fflush (stdout);
    r0_status = sys$icc_transmitw (ni_p->conn_handle,
                                   &ios_icc,
                                   0,
                                   0,
                                   &buffer,
                                   sizeof (buffer));
    errchk_sig (r0_status);
    errchk_sig (ios_icc.ios_icc$w_status);
    (void)printf ("success\n");
}


/******************************************************************************/
static void client_disconnect (struct node_info *ni_p) {
/*
** Disconnect from the association.
*/

static IOSB iosb;

static int r0_status;

    r0_status = sys$icc_disconnectw (ni_p->conn_handle,
                                     &iosb,
                                     0,
                                     0,
                                     0,
                                     0);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$w_status);
}


/******************************************************************************/
static void server_conn_disc_ast (unsigned int event,
                                  unsigned int conn_handle,
                                  unsigned int data_len,
                                  char *data_buffer,
                                  unsigned int p5,
                                  unsigned int p6,
                                  char *p7) {
/*
** Server routine.  Called as an AST on events received from clients.
*/

static IOSB iosb;

static struct node_info *ni_p;

static int r0_status;
static int i;

static struct dsc$descriptor_s node_d = { 0,
                                          DSC$K_DTYPE_T,
                                          DSC$K_CLASS_S,
                                          NULL };

    switch (event) {
        case ICC$C_EV_CONNECT :
            /*
            ** We have a connect request from a client.  Find its node name,
            ** which happens to be passed to us the first bit of the buffer.
            */
            for (i = 0; i < data_len; i++) {
                if (isspace (data_buffer[i])) {
                    break;
                }
            }

            node_d.dsc$w_length = i;
            node_d.dsc$a_pointer = data_buffer;
            ni_p = find_node (&node_d);
            assert (ni_p != NULL);
            (void)printf ("Received connect event from %-.*s... ",
                          data_len,
                          data_buffer);
            (void)fflush (stdout);

            /*
            ** Accept the request and pass the node specific info as the user
            ** context so that the node info is delivered on subsequent data
            ** and disconnect events.
            */
            r0_status = sys$icc_accept (conn_handle,
                                        0,
                                        0,
                                        (unsigned int)ni_p,
                                        0);
            errchk_sig (r0_status);

            ni_p->conn_handle = conn_handle;
            (void)printf ("accepted\n");
            break;
        case ICC$C_EV_DISCONNECT :
            /*
            ** Received a disconnect event.  Get our user context from p6.
            */
            ni_p = (struct node_info *)p6;
            (void)printf ("Received disconnect event from node %-.*s\n",
                          ni_p->node_name_d.dsc$w_length,
                          ni_p->node_name_d.dsc$a_pointer);
            ni_p->seen_disconnect = TRUE;
            break;
        default :
            (void)printf ("Ignored unrecognised event received from ICC: %i\n",
                          event);
            break;
    }
}


/******************************************************************************/
static void server_receive_data_ast (unsigned int message_size,
                                     unsigned int conn_handle,
                                     unsigned int user_context) {
/*
** Server routine called an an AST to get data from clients.
*/

static IOS_ICC ios_icc;

static struct node_info *ni_p;

static int r0_status;

static char buffer[MAXBUFSIZE];

    ni_p = (struct node_info *)user_context;
    (void)printf ("Received data event from %-.*s\n",
                  ni_p->node_name_d.dsc$w_length,
                  ni_p->node_name_d.dsc$a_pointer);

    memset (buffer, 0, sizeof (buffer));

    r0_status = sys$icc_receivew (conn_handle,
                                  &ios_icc,
                                  0,
                                  0,
                                  buffer,
                                  MAXBUFSIZE);
    errchk_sig (r0_status);
    errchk_sig (ios_icc.ios_icc$w_status);

    switch (buffer[0]) {
        case DATA_BLOCK_TYPE :
            (void)printf ("Received data message from %-.*s\n",
                          ni_p->node_name_d.dsc$w_length,
                          ni_p->node_name_d.dsc$a_pointer);
            (void)printf ("  Data: %-.*s\n",
                          ios_icc.ios_icc$l_rcv_len - 1,
                          buffer+1);
            break;
        case END_BLOCK_TYPE :
            (void)printf ("EOD indicator received from %-.*s\n",
                          ni_p->node_name_d.dsc$w_length,
                          ni_p->node_name_d.dsc$a_pointer);
            ni_p->seen_eod = TRUE;
            break;
        default :
            (void)printf ("Bad block type = %d!\n", buffer[0]);
            exit (EXIT_FAILURE);
            break;
    }
}


/******************************************************************************/
static void start_server (unsigned int *server_handle) {
/*
** Open communications.
*/

static int r0_status;

static $DESCRIPTOR (association_d, ASSOC_NAME);
static $DESCRIPTOR (table_d, TABLE_NAME);

    /*
    ** Open an association.  Specify our event and data handlers.
    */
    r0_status = sys$icc_open_assoc (server_handle,
                                    0,
                                    &association_d,
                                    &table_d,
                                    &server_conn_disc_ast,
                                    &server_conn_disc_ast,
                                    &server_receive_data_ast,
                                    0,
                                    0);
    errchk_sig (r0_status);
}


/******************************************************************************/
static void start_client (struct node_info *ni_p) {
/*
** Create a client process on a specific node.
*/

static int r0_status;

static $DESCRIPTOR (prcnam_d, "ICC_DEMO_CLNT");

static char output[255+1];
static char error[255+1];
static char program[255+1];

static struct dsc$descriptor_s output_d = { 0,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            output };
static struct dsc$descriptor_s error_d = { 0,
                                           DSC$K_DTYPE_T,
                                           DSC$K_CLASS_S,
                                           error };
static struct dsc$descriptor_s program_d = { sizeof (program) - 1,
                                             DSC$K_DTYPE_T,
                                             DSC$K_CLASS_S,
                                             program };

static struct {
    unsigned short int retlen;
    unsigned short int function;
    char *address;
} fcitms[] = { 0, FSCN$_DEVICE, NULL,
               0, FSCN$_ROOT, NULL,
               0, FSCN$_DIRECTORY, NULL,
               0, 0, NULL };

static PRVDEF privs;
static int item_code = JPI$_IMAGNAME;

    /*
    ** Get the name of this executable as it is running in this process.
    */
    r0_status = lib$getjpi (&item_code,
                            0,
                            0,
                            0,
                            &program_d,
                            &program_d.dsc$w_length);
    errchk_sig (r0_status);

    /*
    ** Get the addresses and lengths of the various bits of the filename.
    */
    r0_status = sys$filescan (&program_d,
                              fcitms,
                              0,
                              0,
                              0);
    errchk_sig (r0_status);

    /*
    ** Format some output file names.
    */
    output_d.dsc$w_length = sprintf (output,
                                     "%-.*s%-.*s%-.*sICC_DEMO_CLNT.OUT_%-.*s",
                                     fcitms[0].retlen,
                                     fcitms[0].address,
                                     fcitms[1].retlen,
                                     fcitms[1].address,
                                     fcitms[2].retlen,
                                     fcitms[2].address,
                                     ni_p->node_name_d.dsc$w_length,
                                     ni_p->node_name_d.dsc$a_pointer);

    error_d.dsc$w_length = sprintf (error,
                                    "%-.*s%-.*s%-.*sICC_DEMO_CLNT.ERR_%-.*s",
                                     fcitms[0].retlen,
                                     fcitms[0].address,
                                     fcitms[1].retlen,
                                     fcitms[1].address,
                                     fcitms[2].retlen,
                                     fcitms[2].address,
                                     ni_p->node_name_d.dsc$w_length,
                                     ni_p->node_name_d.dsc$a_pointer);

    /*
    ** Required privileges for the client.
    */
    privs.prv$v_netmbx = TRUE;
    privs.prv$v_tmpmbx = TRUE;
    privs.prv$v_syslck = TRUE;
    privs.prv$v_sysnam = TRUE;

    /*
    ** Create the client process.
    */
    r0_status = sys$creprc (&ni_p->client_pid,
                            &program_d,
                            0,
                            &output_d,
                            &error_d,
                            (GENERIC_64 *)&privs,
                            0,
                            &prcnam_d,
                            0,
                            0,
                            0,
                            0,
                            0,
                            &ni_p->node_name_d);
    errchk_sig (r0_status);

    (void)printf ("Client process started on node %-.*s\n",
                  ni_p->node_name_d.dsc$w_length,
                  ni_p->node_name_d.dsc$a_pointer);
}


/******************************************************************************/
static void wait_awhile (void) {
/*
** Wait five seconds.
*/

static GENERIC_64 delay_bin;

static int r0_status;
static unsigned int efn;

static $DESCRIPTOR (delay_d, "0 00:00:05.00");

static char first_time = TRUE;

    if (first_time) {
        /*
        ** First time through, convert ascii time to binary.
        */
        first_time = FALSE;
        r0_status = sys$bintim (&delay_d,
                                &delay_bin);
        errchk_sig (r0_status);
    }

    r0_status = lib$get_ef (&efn);
    errchk_sig (r0_status);

    r0_status = sys$setimr (efn,
                            &delay_bin,
                            0,
                            0,
                            0);
    errchk_sig (r0_status);

    r0_status = sys$waitfr (efn);
    errchk_sig (r0_status);

    r0_status = lib$free_ef (&efn);
    errchk_sig (r0_status);
}


/******************************************************************************/
static void server_wait () {
/*
** Wait for the clients to finish.
*/ 

static struct node_info *ni_p;

static int r0_status;

static char finished = FALSE;

    while (!finished) {
        ni_p = node_list;
        while (ni_p != NULL) {
            if (!ni_p->seen_disconnect) {
                break;
            }
            ni_p = ni_p->next_node;
        }
        if (ni_p == NULL) {
            /*
            ** If all the disconnect indicators are set, we have received
            ** info from all the clients.  Exit the loop.
            */
            finished = TRUE;
        } else {
            /*
            ** We haven't seen a disconnect from at least one client. Go
            ** to sleep for a while.
            */
            wait_awhile ();
        }
    }
}


/******************************************************************************/
static int main (void) {

static struct node_info *ni_p;

static int r0_status;
static unsigned int server_handle;

static char master;
    
    /*
    ** Are we a client or are we the server?
    */
    determine_master (&master);

    /*
    ** Get list of nodes in the cluster.  Of course, this will work on a
    ** standalone node as well.  The return value is the address of this
    ** node's info.
    */
    ni_p = get_node_list ();

    if (master) {
        /*
        ** Here we are the server.
        */
        (void)printf ("ICC_DEMO - master\n");

        /*
        ** Open communication channel so the clients can talk to us.
        */
        start_server (&server_handle);

        ni_p = node_list;
        while (ni_p != NULL) {
            /*
            ** Start a client on each node in the cluster.
            */
            start_client (ni_p);
            ni_p = ni_p->next_node;
        }

        /*
        ** Wait for all clients to finish.
        */
        server_wait ();

        /*
        ** Close the ICC association.
        */
        r0_status = sys$icc_close_assoc (server_handle);
        errchk_sig (r0_status);

        (void)printf ("Master exit\n");
    } else {
        /*
        ** Here the code is acting as a client.
        */
        (void)printf ("ICC_DEMO - client\n");

        /*
        ** Send a message to the server.
        */
        client_send_hello (ni_p);

        /*
        ** Tell the server we have finished.
        */
        client_send_eod (ni_p);

        /*
        ** Disconnect.
        */
        client_disconnect (ni_p);

        (void)printf ("Client exit\n");
    }
}
