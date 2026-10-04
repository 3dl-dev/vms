#ifndef OVMX_OPTARGS_H
#define OVMX_OPTARGS_H
/*
 * ovmx_optargs.h - omitted trailing arguments of VMS routines.
 *
 * VMS routines are documented with optional trailing arguments, and callers
 * routinely omit them: on VMS the callee reads the argument count from the
 * argument list (AP) and treats an omitted argument as 0.  The x86-64 / AArch64
 * C ABI carries no argument count -- an omitted trailing argument of a fixed-
 * arity prototype is garbage in a register (and is a compile error in C).
 *
 * OVMX_PAD_<N>(f, args...) calls f with args..., padded with 0 up to N total
 * arguments, which is exactly what the VMS argument list means by "omitted".
 * Used from a function-like macro of the routine's own name, declared AFTER
 * the prototype:
 *
 *     #define sys$open(...) OVMX_PAD_3(sys$open, __VA_ARGS__)
 *
 * A translation unit that DEFINES such a routine writes the definition with
 * the name parenthesised -- uint32_t (sys$open)(void *fab, ...) -- which a
 * function-like macro does not expand.
 */
#define OVMX_NARGS_(a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14,a15,a16,n,...) n
#define OVMX_NARGS(...) OVMX_NARGS_(__VA_ARGS__,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0)
#define OVMX_CAT_(a,b) a##b
#define OVMX_CAT(a,b) OVMX_CAT_(a,b)
#define OVMX_ZEROS_0 
#define OVMX_ZEROS_1 ,0
#define OVMX_ZEROS_2 ,0,0
#define OVMX_ZEROS_3 ,0,0,0
#define OVMX_ZEROS_4 ,0,0,0,0
#define OVMX_ZEROS_5 ,0,0,0,0,0
#define OVMX_ZEROS_6 ,0,0,0,0,0,0
#define OVMX_ZEROS_7 ,0,0,0,0,0,0,0
#define OVMX_ZEROS_8 ,0,0,0,0,0,0,0,0
#define OVMX_ZEROS_9 ,0,0,0,0,0,0,0,0,0
#define OVMX_ZEROS_10 ,0,0,0,0,0,0,0,0,0,0
#define OVMX_ZEROS_11 ,0,0,0,0,0,0,0,0,0,0,0
#define OVMX_ZEROS_12 ,0,0,0,0,0,0,0,0,0,0,0,0
#define OVMX_ZEROS_13 ,0,0,0,0,0,0,0,0,0,0,0,0,0
#define OVMX_ZEROS_14 ,0,0,0,0,0,0,0,0,0,0,0,0,0,0
#define OVMX_ZEROS_15 ,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0

#define OVMX_PAD_2(f, ...) f(__VA_ARGS__ OVMX_CAT(OVMX_ZEROS_, OVMX_SUB_2(OVMX_NARGS(__VA_ARGS__))))
#define OVMX_PAD_3(f, ...) f(__VA_ARGS__ OVMX_CAT(OVMX_ZEROS_, OVMX_SUB_3(OVMX_NARGS(__VA_ARGS__))))
#define OVMX_PAD_4(f, ...) f(__VA_ARGS__ OVMX_CAT(OVMX_ZEROS_, OVMX_SUB_4(OVMX_NARGS(__VA_ARGS__))))
#define OVMX_PAD_5(f, ...) f(__VA_ARGS__ OVMX_CAT(OVMX_ZEROS_, OVMX_SUB_5(OVMX_NARGS(__VA_ARGS__))))
#define OVMX_PAD_6(f, ...) f(__VA_ARGS__ OVMX_CAT(OVMX_ZEROS_, OVMX_SUB_6(OVMX_NARGS(__VA_ARGS__))))
#define OVMX_PAD_7(f, ...) f(__VA_ARGS__ OVMX_CAT(OVMX_ZEROS_, OVMX_SUB_7(OVMX_NARGS(__VA_ARGS__))))
#define OVMX_PAD_8(f, ...) f(__VA_ARGS__ OVMX_CAT(OVMX_ZEROS_, OVMX_SUB_8(OVMX_NARGS(__VA_ARGS__))))
#define OVMX_PAD_9(f, ...) f(__VA_ARGS__ OVMX_CAT(OVMX_ZEROS_, OVMX_SUB_9(OVMX_NARGS(__VA_ARGS__))))
#define OVMX_PAD_10(f, ...) f(__VA_ARGS__ OVMX_CAT(OVMX_ZEROS_, OVMX_SUB_10(OVMX_NARGS(__VA_ARGS__))))
#define OVMX_PAD_11(f, ...) f(__VA_ARGS__ OVMX_CAT(OVMX_ZEROS_, OVMX_SUB_11(OVMX_NARGS(__VA_ARGS__))))
#define OVMX_PAD_12(f, ...) f(__VA_ARGS__ OVMX_CAT(OVMX_ZEROS_, OVMX_SUB_12(OVMX_NARGS(__VA_ARGS__))))
#define OVMX_PAD_13(f, ...) f(__VA_ARGS__ OVMX_CAT(OVMX_ZEROS_, OVMX_SUB_13(OVMX_NARGS(__VA_ARGS__))))
#define OVMX_PAD_14(f, ...) f(__VA_ARGS__ OVMX_CAT(OVMX_ZEROS_, OVMX_SUB_14(OVMX_NARGS(__VA_ARGS__))))
#define OVMX_PAD_15(f, ...) f(__VA_ARGS__ OVMX_CAT(OVMX_ZEROS_, OVMX_SUB_15(OVMX_NARGS(__VA_ARGS__))))
#define OVMX_PAD_16(f, ...) f(__VA_ARGS__ OVMX_CAT(OVMX_ZEROS_, OVMX_SUB_16(OVMX_NARGS(__VA_ARGS__))))

#define OVMX_SUB_2_1 1
#define OVMX_SUB_2_2 0
#define OVMX_SUB_2(g) OVMX_CAT(OVMX_SUB_2_, g)
#define OVMX_SUB_3_1 2
#define OVMX_SUB_3_2 1
#define OVMX_SUB_3_3 0
#define OVMX_SUB_3(g) OVMX_CAT(OVMX_SUB_3_, g)
#define OVMX_SUB_4_1 3
#define OVMX_SUB_4_2 2
#define OVMX_SUB_4_3 1
#define OVMX_SUB_4_4 0
#define OVMX_SUB_4(g) OVMX_CAT(OVMX_SUB_4_, g)
#define OVMX_SUB_5_1 4
#define OVMX_SUB_5_2 3
#define OVMX_SUB_5_3 2
#define OVMX_SUB_5_4 1
#define OVMX_SUB_5_5 0
#define OVMX_SUB_5(g) OVMX_CAT(OVMX_SUB_5_, g)
#define OVMX_SUB_6_1 5
#define OVMX_SUB_6_2 4
#define OVMX_SUB_6_3 3
#define OVMX_SUB_6_4 2
#define OVMX_SUB_6_5 1
#define OVMX_SUB_6_6 0
#define OVMX_SUB_6(g) OVMX_CAT(OVMX_SUB_6_, g)
#define OVMX_SUB_7_1 6
#define OVMX_SUB_7_2 5
#define OVMX_SUB_7_3 4
#define OVMX_SUB_7_4 3
#define OVMX_SUB_7_5 2
#define OVMX_SUB_7_6 1
#define OVMX_SUB_7_7 0
#define OVMX_SUB_7(g) OVMX_CAT(OVMX_SUB_7_, g)
#define OVMX_SUB_8_1 7
#define OVMX_SUB_8_2 6
#define OVMX_SUB_8_3 5
#define OVMX_SUB_8_4 4
#define OVMX_SUB_8_5 3
#define OVMX_SUB_8_6 2
#define OVMX_SUB_8_7 1
#define OVMX_SUB_8_8 0
#define OVMX_SUB_8(g) OVMX_CAT(OVMX_SUB_8_, g)
#define OVMX_SUB_9_1 8
#define OVMX_SUB_9_2 7
#define OVMX_SUB_9_3 6
#define OVMX_SUB_9_4 5
#define OVMX_SUB_9_5 4
#define OVMX_SUB_9_6 3
#define OVMX_SUB_9_7 2
#define OVMX_SUB_9_8 1
#define OVMX_SUB_9_9 0
#define OVMX_SUB_9(g) OVMX_CAT(OVMX_SUB_9_, g)
#define OVMX_SUB_10_1 9
#define OVMX_SUB_10_2 8
#define OVMX_SUB_10_3 7
#define OVMX_SUB_10_4 6
#define OVMX_SUB_10_5 5
#define OVMX_SUB_10_6 4
#define OVMX_SUB_10_7 3
#define OVMX_SUB_10_8 2
#define OVMX_SUB_10_9 1
#define OVMX_SUB_10_10 0
#define OVMX_SUB_10(g) OVMX_CAT(OVMX_SUB_10_, g)
#define OVMX_SUB_11_1 10
#define OVMX_SUB_11_2 9
#define OVMX_SUB_11_3 8
#define OVMX_SUB_11_4 7
#define OVMX_SUB_11_5 6
#define OVMX_SUB_11_6 5
#define OVMX_SUB_11_7 4
#define OVMX_SUB_11_8 3
#define OVMX_SUB_11_9 2
#define OVMX_SUB_11_10 1
#define OVMX_SUB_11_11 0
#define OVMX_SUB_11(g) OVMX_CAT(OVMX_SUB_11_, g)
#define OVMX_SUB_12_1 11
#define OVMX_SUB_12_2 10
#define OVMX_SUB_12_3 9
#define OVMX_SUB_12_4 8
#define OVMX_SUB_12_5 7
#define OVMX_SUB_12_6 6
#define OVMX_SUB_12_7 5
#define OVMX_SUB_12_8 4
#define OVMX_SUB_12_9 3
#define OVMX_SUB_12_10 2
#define OVMX_SUB_12_11 1
#define OVMX_SUB_12_12 0
#define OVMX_SUB_12(g) OVMX_CAT(OVMX_SUB_12_, g)
#define OVMX_SUB_13_1 12
#define OVMX_SUB_13_2 11
#define OVMX_SUB_13_3 10
#define OVMX_SUB_13_4 9
#define OVMX_SUB_13_5 8
#define OVMX_SUB_13_6 7
#define OVMX_SUB_13_7 6
#define OVMX_SUB_13_8 5
#define OVMX_SUB_13_9 4
#define OVMX_SUB_13_10 3
#define OVMX_SUB_13_11 2
#define OVMX_SUB_13_12 1
#define OVMX_SUB_13_13 0
#define OVMX_SUB_13(g) OVMX_CAT(OVMX_SUB_13_, g)
#define OVMX_SUB_14_1 13
#define OVMX_SUB_14_2 12
#define OVMX_SUB_14_3 11
#define OVMX_SUB_14_4 10
#define OVMX_SUB_14_5 9
#define OVMX_SUB_14_6 8
#define OVMX_SUB_14_7 7
#define OVMX_SUB_14_8 6
#define OVMX_SUB_14_9 5
#define OVMX_SUB_14_10 4
#define OVMX_SUB_14_11 3
#define OVMX_SUB_14_12 2
#define OVMX_SUB_14_13 1
#define OVMX_SUB_14_14 0
#define OVMX_SUB_14(g) OVMX_CAT(OVMX_SUB_14_, g)
#define OVMX_SUB_15_1 14
#define OVMX_SUB_15_2 13
#define OVMX_SUB_15_3 12
#define OVMX_SUB_15_4 11
#define OVMX_SUB_15_5 10
#define OVMX_SUB_15_6 9
#define OVMX_SUB_15_7 8
#define OVMX_SUB_15_8 7
#define OVMX_SUB_15_9 6
#define OVMX_SUB_15_10 5
#define OVMX_SUB_15_11 4
#define OVMX_SUB_15_12 3
#define OVMX_SUB_15_13 2
#define OVMX_SUB_15_14 1
#define OVMX_SUB_15_15 0
#define OVMX_SUB_15(g) OVMX_CAT(OVMX_SUB_15_, g)
#define OVMX_SUB_16_1 15
#define OVMX_SUB_16_2 14
#define OVMX_SUB_16_3 13
#define OVMX_SUB_16_4 12
#define OVMX_SUB_16_5 11
#define OVMX_SUB_16_6 10
#define OVMX_SUB_16_7 9
#define OVMX_SUB_16_8 8
#define OVMX_SUB_16_9 7
#define OVMX_SUB_16_10 6
#define OVMX_SUB_16_11 5
#define OVMX_SUB_16_12 4
#define OVMX_SUB_16_13 3
#define OVMX_SUB_16_14 2
#define OVMX_SUB_16_15 1
#define OVMX_SUB_16_16 0
#define OVMX_SUB_16(g) OVMX_CAT(OVMX_SUB_16_, g)

#endif /* OVMX_OPTARGS_H */
