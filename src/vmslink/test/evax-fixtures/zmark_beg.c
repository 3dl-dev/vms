/* vms-4d0 fixture: a ZERO-length contribution to psect "zmark" used as a
 * boundary marker (the crtbegin __EH_FRAME_BEGIN__ pattern), and a pointer to it. */
static char zmark_begin[] __attribute__((section("zmark"), aligned(8))) = { };
char *zmark_ptr = zmark_begin;
int main(int c, char **v, char **e){ (void)v;(void)e; return zmark_ptr[c & 1]; }
