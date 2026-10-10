/* vms-e1a7 fixture: `#pragma weak` references nothing in the link defines --
 * the shape LIBVMS$SHR uses to reach LIBVMSRMS$SHR (sys$parse, the rights
 * database) across the layering cycle. WEAK_CALL is tested for presence and
 * called through a linkage pair; WEAK_DATA's address is tested and read.
 * HELPER_PROC is weak too, but a --use producer exports it, so it binds as an
 * ordinary (strong) cross-image import.
 * Built with: alpha-dec-vms-gcc -O2 -S weakref_main.c
 *             alpha-dec-vms-gcc -c -o weakref_main.obj weakref_main.s */
extern int WEAK_CALL(int);
extern int WEAK_DATA;
extern int HELPER_PROC(void);
#pragma weak WEAK_CALL
#pragma weak WEAK_DATA
#pragma weak HELPER_PROC
int MAIN_PROC(void)
{
    int r = 0;
    if (WEAK_CALL)
        r += WEAK_CALL(1);
    if (&WEAK_DATA)
        r += WEAK_DATA;
    return r + HELPER_PROC();
}
