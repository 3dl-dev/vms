/* Library member the search MUST pull: its routine's name equals a.c's for the
 * first 40 characters. A linker that clamps names at 31 characters sees ONE
 * symbol, pulls only a.obj, and silently binds both calls to it. */
int ovmx_library_search_long_routine_name_beta(void) { return 4; }
