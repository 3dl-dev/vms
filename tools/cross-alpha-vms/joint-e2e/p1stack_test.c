/*
 * p1stack_test.c - vms-ce5: the image runs on a user stack in P1.
 *
 * On OpenVMS Alpha the user stack is in P1 (0x40000000-0x7FFFFFFF), so frame
 * addresses are valid 32-bit values and a longword invocation handle names a
 * frame exactly. Checks: a local's address is in P1; 20 MB of stack is usable
 * (more than the substrate's 8 MB default, so it is IMGACT's P1 stack and not
 * the substrate stack); the LIB$GET_INVO_HANDLE longword lies in P1 and
 * LIB$GET_INVO_CONTEXT finds the frame from it. Sentinel 7 = all held.
 */
extern int printf(const char *, ...);
extern int LIB$GET_INVO_HANDLE(void *icb);
extern int LIB$GET_INVO_CONTEXT(int handle, void *icb);
extern unsigned int lib$get_curr_invo_context(void *icb);

#define P1_LO 0x40000000ULL
#define P1_HI 0x80000000ULL

static unsigned long long lowest;

__attribute__((noinline)) static int dig(int depth)
{
    volatile char pad[64 * 1024];           /* 64 KB per frame */
    pad[0] = (char)depth;
    pad[sizeof pad - 1] = (char)depth;
    unsigned long long a = (unsigned long long)&pad[0];
    if (a < lowest) lowest = a;
    if (depth == 0)
        return pad[0] + pad[sizeof pad - 1];
    return dig(depth - 1) + (pad[0] == (char)depth);
}

int main(int argc, char **argv)
{
    (void)argv;
    int local = 0;
    unsigned long long la = (unsigned long long)&local;
    int in_p1 = la >= P1_LO && la < P1_HI;

    lowest = la;
    int deep = dig(320) == 320;              /* 320 x 64 KB = 20 MB */
    int deep_in_p1 = lowest >= P1_LO;

    static unsigned long long icb[512], icb2[512];   /* >= sizeof(INVO_CONTEXT_BLK) */
    int handle = 0, found = 0;
    if (lib$get_curr_invo_context(icb) & 1) {
        handle = LIB$GET_INVO_HANDLE(icb);
        found = LIB$GET_INVO_CONTEXT(handle, icb2) & 1;
    }
    unsigned long long h = (unsigned long long)(unsigned int)handle;
    int handle_p1 = h >= P1_LO && h < P1_HI;

    int ok = in_p1 && deep && deep_in_p1 && handle_p1 && found;
    printf("OVMX p1stack test: local=%#llx inP1=%d deep=%d lowest=%#llx handle=%#llx handleP1=%d found=%d argc=%d\n",
           la, in_p1, deep, lowest, h, handle_p1, found, argc);
    return ok ? 7 : 3;
}
