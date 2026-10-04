/*
 * lib_initialize.c -- OVMX's LIB$INITIALIZE dispatcher for Alpha VMS images
 * (vms-43c). A STARLET module: it is linked INTO each image that references
 * LIB$INITIALIZE (the port's libgcc crtbegin.o/crtend.o do), exactly as the
 * LIB$INITIALIZE module of SYS$LIBRARY:STARLET.OLB is on OpenVMS Alpha.
 *
 * Contract (public: OpenVMS RTL Library (LIB$) Manual, LIB$INITIALIZE; OpenVMS
 * Linker Manual, transfer addresses):
 *   - Programs register initialization routines by placing their procedure
 *     values, as LONGWORDS, in the program section named LIB$INITIALIZE. The
 *     image lives in P0, so a longword holds any procedure value (vms-035).
 *   - LINK puts LIB$INITIALIZE ahead of the main transfer address in the image's
 *     transfer vector, so the image activator calls it first, with the standard
 *     six-argument activation list whose first argument is the transfer vector.
 *   - LIB$INITIALIZE calls each routine, in psect order, with
 *       (init_coroutine, cli_coroutine, image_header, image_file_desc,
 *        link_flags, cli_flags)
 *     and then calls the next transfer address (the main program) with the same
 *     context, returning its status. A routine may instead call init_coroutine,
 *     which runs the remaining routines and the main program and returns its
 *     status to the routine (so it can clean up after main); LIB$INITIALIZE then
 *     returns that status without running anything twice.
 *
 * The psect bounds come from LINK.EXE as the linker-defined symbols
 * ovmx$lib_initialize_start / ovmx$lib_initialize_end (vms-43c). Build with the
 * alpha-dec-vms cross cc1, -mpointer-size=64.
 */

typedef int (*ovmx_xfer_fn)(void *, void *, void *, void *, unsigned int,
                            unsigned int);

extern int ovmx$lib_initialize_start[];
extern int ovmx$lib_initialize_end[];

/* Activation context, saved for the coroutine. */
static void *li_xfervec, *li_cli, *li_imghdr, *li_imgfile;
static unsigned int li_linkflag, li_cliflag;
static int *li_next;          /* next LIB$INITIALIZE entry to run          */
static int  li_main_done;     /* main already run (through the coroutine)   */
static int  li_main_status;

static int li_run_rest(void);

/* The init_coroutine handed to every routine. */
static int li_coroutine(void)
{
    return li_run_rest();
}

/* Run the remaining routines, then the main transfer address; once. */
static int li_run_rest(void)
{
    while (li_next < ovmx$lib_initialize_end) {
        int ent = *li_next++;
        if (ent == 0)
            continue;   /* an empty slot (alignment padding) */
        /* A longword procedure value, sign-extended as Alpha loads it. */
        ovmx_xfer_fn rtn = (ovmx_xfer_fn)(long long)ent;
        rtn((void *)li_coroutine, li_cli, li_imghdr, li_imgfile,
            li_linkflag, li_cliflag);
        if (li_main_done)
            return li_main_status;   /* a routine ran main via the coroutine */
    }
    if (!li_main_done) {
        /* The transfer vector: [LIB$INITIALIZE, main, 0]. */
        unsigned long long *vec = (unsigned long long *)li_xfervec;
        ovmx_xfer_fn mainpv = (ovmx_xfer_fn)vec[1];
        li_main_done = 1;
        li_main_status = mainpv((void *)vec[1], li_cli, li_imghdr, li_imgfile,
                                li_linkflag, li_cliflag);
    }
    return li_main_status;
}

int LIB$INITIALIZE(void *xfervec, void *cli_util, void *imghdr,
                   void *image_file_desc, unsigned int linkflag,
                   unsigned int cliflag)
{
    li_xfervec = xfervec;  li_cli = cli_util;  li_imghdr = imghdr;
    li_imgfile = image_file_desc;
    li_linkflag = linkflag;  li_cliflag = cliflag;
    li_next = ovmx$lib_initialize_start;
    return li_run_rest();
}
