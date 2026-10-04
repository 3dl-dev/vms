/* Library member the search must NOT pull: nothing references it, and it
 * references a symbol nothing defines -- so linking it (whole-archive) fails
 * %LINK-F-UNDEF, which is exactly what proves the search is selective. */
extern int ovmx_library_search_never_defined(void);
int ovmx_library_search_unused_member(void) { return ovmx_library_search_never_defined(); }
