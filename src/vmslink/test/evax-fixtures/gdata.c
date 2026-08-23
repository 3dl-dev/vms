extern int printf(const char *, ...);
static const char greeting[] = "hi from global data";
const char *g_ptr = greeting;         /* global ptr -> global data = a REFQUAD data reloc */
int main(int c, char **v, char **e){ (void)v;(void)e; return g_ptr[c & 1]; }
