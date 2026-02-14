/* Classic VMS Hello World - should compile with gcc -lvms */
#include <descrip.h>
#include <ssdef.h>
#include <lib$routines.h>

int main(void) {
    $DESCRIPTOR(msg, "Hello from OpenVMS!");
    lib$put_output(&msg);
    return 0;  /* Unix success code */
}
