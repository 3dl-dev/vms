/* Alpha LP64 fast integer types. OVMX alpha-dec-vms musl port (vms-960). */
typedef int32_t int_fast16_t;
typedef int32_t int_fast32_t;
typedef uint32_t uint_fast16_t;
typedef uint32_t uint_fast32_t;

#define INT_FAST16_MIN  INT32_MIN
#define INT_FAST32_MIN  INT32_MIN

#define INT_FAST16_MAX  INT32_MAX
#define INT_FAST32_MAX  INT32_MAX

#define UINT_FAST16_MAX UINT32_MAX
#define UINT_FAST32_MAX UINT32_MAX

/* vms-ce6: pointer-width limits follow the client's pointer size (see
 * bits/alltypes.h.in _Addr). */
#if defined(__INITIAL_POINTER_SIZE) && __INITIAL_POINTER_SIZE == 64
#define INTPTR_MIN      INT64_MIN
#define INTPTR_MAX      INT64_MAX
#define UINTPTR_MAX     UINT64_MAX
#define PTRDIFF_MIN     INT64_MIN
#define PTRDIFF_MAX     INT64_MAX
#else
#define INTPTR_MIN      INT32_MIN
#define INTPTR_MAX      INT32_MAX
#define UINTPTR_MAX     UINT32_MAX
#define PTRDIFF_MIN     INT32_MIN
#define PTRDIFF_MAX     INT32_MAX
#endif
#define SIZE_MAX        UINT32_MAX
