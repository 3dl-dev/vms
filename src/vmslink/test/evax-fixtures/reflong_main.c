/* vms-reflong fixture (DEC C default 32-bit pointers): a statically
 * initialised procedure value of an IMPORTED routine -- a REFLONG cell. */
extern int HELPER_PROC(void);
int (*helper_ptr)(void) = HELPER_PROC;
int MAIN_PROC(void) { return helper_ptr(); }
