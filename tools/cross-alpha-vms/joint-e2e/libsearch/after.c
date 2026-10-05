/* An object listed AFTER --library on the LINK line (vms-4d0 link order): the
 * library's pulled members must be placed before it, where the library
 * appeared -- as crtend.o must follow the libraries' C++ constructors. */
int ovmx_library_search_after(void) { return 9; }
