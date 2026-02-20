
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <string.h>
#include <descrip.h>
#include <libdef.h>
#include <lib$routines.h>

#include "errchk.h"

struct node {
    void *llink;
    void *rlink;
    short int reserved;
    struct dsc$descriptor_s key_d;
    unsigned short int count;
};


/******************************************************************************/
static int allocation_routine (char *key,
                               struct node **node_p,
                               void *user_date) {

static struct node *p;

    p = malloc (sizeof (struct node));
    if (p == NULL) {
        return SS$_INSFMEM;
    }
    p->key_d.dsc$w_length = strlen (key);
    p->key_d.dsc$a_pointer = malloc (p->key_d.dsc$w_length);
    if (p->key_d.dsc$a_pointer == NULL) {
        return SS$_INSFMEM;
    }
    memcpy (p->key_d.dsc$a_pointer, key, p->key_d.dsc$w_length);
    p->count = 1;

    *node_p = p;
    return SS$_NORMAL;
}


/******************************************************************************/
static int compare_routine (char *key,
                            struct node *node_p) {

    if (strlen (key) < node_p->key_d.dsc$w_length) {
        return -1;
    }

    if (strlen (key) > node_p->key_d.dsc$w_length) {
        return 1;
    }

    return (memcmp (key,
                    node_p->key_d.dsc$a_pointer,
                    node_p->key_d.dsc$w_length));
}


/******************************************************************************/
static int print_routine (struct node *node_p,
                          void *user_data) {

    (void)printf ("%-.*s: %u\n",
                  node_p->key_d.dsc$w_length,
                  node_p->key_d.dsc$a_pointer,
                  node_p->count);
    return SS$_NORMAL;
}


/******************************************************************************/
int main (void) {

/*
** Demo LIB$INSERT_TREE (), LIB$LOOKUP_TREE (), and LIB$TRAVERSE_TREE ().
*/
static int r0_status;
static unsigned int flags = 0;
static char check_word[] = "the";

static struct node *node_p;

static struct node *tree_head = NULL;

static short int i;

static char *p;
static char *key_array[] = { "it", "was", "morning", "and", "the", "new",
                             "sun", "sparkled", "gold", "across", "the",
                             "ripples", "of", "a", "gentle", "sea", NULL };


    while ((p = key_array[i++]) != NULL) {
	r0_status = lib$insert_tree (&tree_head,
                                     (unsigned int *)p,
                                     &flags,
                                     (int (*)())compare_routine,
                                     (int (*)())allocation_routine,
                                     &node_p,
                                     0);
        if (r0_status == LIB$_KEYALRINS) {
            node_p->count++;
        } else {
            errchk_sig (r0_status);
        }
        (void)printf ("%s ", p);
    }

    (void)printf ("\n\nTree constructed.  "
                  "Looking for occurances of \"the\":\n");

    r0_status = lib$lookup_tree (&tree_head,
                                 (unsigned int *)check_word,
                                 (int (*)())compare_routine,
                                 &node_p);
    if (r0_status == LIB$_KEYNOTFOU) {
        (void)printf ("Key not found in tree\n");
    } else {
        errchk_sig (r0_status);
        (void)printf ("Count of the word \"the\": %u\n",
                      node_p->count);
    }

    (void)printf ("Dump of tree.  Note words are ordered:\n");
    r0_status = lib$traverse_tree (&tree_head,
                                   (int (*)())print_routine,
                                   0);
    errchk_sig (r0_status);
}
