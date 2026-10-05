/* vms-fd1 fixture (patch 0006): a leaf that starts frameless (PT_NULL) and
 * whose integer result is returned as a double. On a CPU without FIX the
 * GENERAL_REGS -> FP_REGS move needs a secondary-memory stack slot, which reload
 * allocates itself; with every call-used integer register live the procedure
 * then becomes a stack procedure unwound through the hard frame pointer. The
 * unpatched alpha VMS back end left the soft frame pointer in that slot's
 * address -- `stq $N,0($63)` -- which the assembler rejects. */
union dbl { double d; unsigned long q; };
double mkd(const unsigned long *p, long n, long a, long b)
{
    union dbl u;
    if (n == 0)
        return 0.0;
    unsigned long v0=p[0]*a, v1=p[1]*b, v2=p[2]+a, v3=p[3]^b, v4=p[4]-a, v5=p[5]|b, v6=p[6]&a, v7=p[7]*3,
                  v8=p[8]*5, v9=p[9]*7, v10=p[10]*9, v11=p[11]*11, v12=p[12]*13, v13=p[13]*15, v14=p[14]*17,
                  v15=p[15]*19, v16=p[16]*21, v17=p[17]*23, v18=p[18]*25, v19=p[19]*27;
    if (a < -1075)
        return 0.0;
    u.q = ((v0+v1)*(v2+v3)) ^ ((v4+v5)*(v6+v7)) ^ ((v8+v9)*(v10+v11)) ^ ((v12+v13)*(v14+v15)) ^ ((v16+v17)*(v18+v19))
        ^ (v0*v19) ^ (v1*v18) ^ (v2*v17) ^ (v3*v16) ^ (v4*v15) ^ (v5*v14) ^ (v6*v13) ^ (v7*v12) ^ (v8*v11) ^ (v9*v10);
    return u.d;
}
