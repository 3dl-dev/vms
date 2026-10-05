/* LIB$INITIALIZE activation test (vms-43c). libinit_tab_p32.c registers two
 * initialization routines in the LIB$INITIALIZE psect (longword procedure
 * values, as real VMS code and the port's crtbegin/crtend do). LINK.EXE puts
 * the STARLET LIB$INITIALIZE dispatcher first in the transfer vector; it must
 * run both routines, in psect order, BEFORE main. Sentinel 7 = all of it held. */
extern int printf(const char *, ...);

int ovmx_libinit_count = 0;
int ovmx_libinit_order[4];

void ovmx_libinit_test_a(void) { ovmx_libinit_order[ovmx_libinit_count++] = 1; }
void ovmx_libinit_test_b(void) { ovmx_libinit_order[ovmx_libinit_count++] = 2; }

int main(int argc, char **argv, char **envp)
{
    (void)argv; (void)envp;
    printf("OVMX LIB$INITIALIZE test: %d routine(s) ran before main, order %d,%d argc=%d\n",
           ovmx_libinit_count, ovmx_libinit_order[0], ovmx_libinit_order[1], argc);
    if (ovmx_libinit_count == 2 && ovmx_libinit_order[0] == 1 && ovmx_libinit_order[1] == 2)
        return 7;   /* distinctive success -> $STATUS = C$_EXIT1 + (7-1)*8 */
    return 3;
}
