/* The LIB$INITIALIZE registrations for libinit_main.c (vms-43c). Compiled with
 * the default 32-bit pointer size, so each entry is a longword procedure value
 * -- what OpenVMS expects in LIB$INITIALIZE and what only a P0 image can hold. */
extern void ovmx_libinit_test_a(void);
extern void ovmx_libinit_test_b(void);
extern int LIB$INITIALIZE();

void (*const ovmx_libinit_tab[2])(void)
    __attribute__((section("LIB$INITIALIZE"), used)) =
    { ovmx_libinit_test_a, ovmx_libinit_test_b };

/* Reference the dispatcher so the STARLET library search links it in, as
 * crtstuff's reference to LIB$INITIALIZE does. */
int (*const ovmx_libinit_dispatcher_ref)() = LIB$INITIALIZE;
