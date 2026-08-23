extern int printf(const char *, ...);
int main(int argc, char **argv, char **envp) {
    (void)argv; (void)envp;
    printf("OVMX crt0 join: activated, argc=%d\n", argc);
    return 3;   /* distinctive non-zero -> $STATUS readback via C$_EXIT1 fold */
}
