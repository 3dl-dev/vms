/* LINK.EXE --library search test (vms-4d0). Calls two routines whose names
 * share their first 40 characters -- distinct only beyond VMS's classic
 * 31-character limit -- defined in two different library members (a.c, c.c),
 * both of which the search must pull. Link-only; never executed. */
extern int ovmx_library_search_long_routine_name_alpha(void);
extern int ovmx_library_search_long_routine_name_beta(void);

int main(void)
{
    return ovmx_library_search_long_routine_name_alpha()
         + ovmx_library_search_long_routine_name_beta();
}
