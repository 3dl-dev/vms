extern int printf(const char *, ...);
extern void *malloc(unsigned long);
extern char *strcpy(char *, const char *);
extern unsigned long strlen(const char *);
int main(int argc, char **argv, char **envp) {
    (void)argv; (void)envp;
    char *p = malloc(32);
    strcpy(p, "hello");
    printf("%s world argc=%d len=%lu\n", p, argc, strlen(p));
    return 0;
}
