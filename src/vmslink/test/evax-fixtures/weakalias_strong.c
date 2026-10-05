/* vms-122 fixture: the strong __vm_wait that overrides the weak dummy. */
volatile int vm_lock;
void __vm_wait(void) { while (vm_lock) ; }
