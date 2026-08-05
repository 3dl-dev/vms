#include <stdio.h>

void vms_set_crtl_values(void);

#pragma extern_model save
#pragma extern_model strict_refdef "LIB$INITIALIZE" nowrt, long
#if __INITIAL_POINTER_SIZE
#    pragma __pointer_size __save
#    pragma __pointer_size 32
#else
#    pragma __required_pointer_size __save
#    pragma __required_pointer_size 32
#endif
void (* const iniarray[])() = {vms_set_crtl_values, } ;	/* Set our contribution to the LIB$INITIALIZE array */
#if __INITIAL_POINTER_SIZE
#    pragma __pointer_size __restore
#else
#    pragma __required_pointer_size __restore
#endif
#pragma extern_model restore

int LIB$INITIALIZE();
/* globaldef */ int (*lib_init_ref)() = LIB$INITIALIZE;
