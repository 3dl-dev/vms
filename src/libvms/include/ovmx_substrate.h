/*
 * ovmx_substrate.h - the substrate scope (rd vms-a66e).
 *
 * The C RTL a program calls and the system services that program calls share
 * one C library in the process. A C program's file names are RMS file
 * specifications and its terminals and pipes are VMS channels (DECC$SHR's file
 * layer, src/vmsrms/crtl_rms_fd.c + crtl_devfd.c); nothing it names reaches the
 * substrate's file system. A system service, though, does the substrate's own
 * work underneath the VMS layer -- $CREPRC forks its task and hands it host
 * pipes, IMGACT maps an image from the /vms staging, $GETJPI reads the
 * substrate's accounting -- and those calls go to the kernel as they are.
 *
 * OVMX_SUBSTRATE_SCOPE() at the top of such a routine marks its whole body:
 * while any such routine is running, ovmx_substrate_active() is nonzero and
 * the C RTL file layer leaves every call to the kernel. The scope ends at any
 * return (a cleanup variable), so an early return cannot leak it.
 */
#ifndef OVMX_SUBSTRATE_H
#define OVMX_SUBSTRATE_H

int  ovmx_substrate_enter(void);
void ovmx_substrate_leave(void);
int  ovmx_substrate_active(void);

static inline void ovmx_substrate_scope_end(int *scope)
{
    (void)scope;
    ovmx_substrate_leave();
}

#define OVMX_SUBSTRATE_SCOPE()                                                  \
    int ovmx_substrate_scope_ __attribute__((cleanup(ovmx_substrate_scope_end), \
                                             unused)) = ovmx_substrate_enter()

#endif /* OVMX_SUBSTRATE_H */
