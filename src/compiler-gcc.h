/*
 * compiler-gcc.h - definitions for the GNU C Compiler.  Currently this also
 * handles clang and the Intel C Compiler.
 */

#define GCC_PREREQ(major, minor)					\
	(!defined(__clang__) && !defined(__INTEL_COMPILER) &&		\
	 (__GNUC__ > major ||						\
	  (__GNUC__ == major && __GNUC_MINOR__ >= minor)))

#define CLANG_PREREQ(major, minor)					\
	(defined(__clang__) &&						\
	 (__clang_major__ > major ||					\
	  (__clang_major__ == major && __clang_minor__ >= minor)))

#ifdef _WIN32
#  define LIBEXPORT __declspec(dllexport)
#else
#  define LIBEXPORT __attribute__((visibility("default")))
#endif
#define forceinline		inline __attribute__((always_inline))
#define likely(expr)		__builtin_expect(!!(expr), 1)
#define unlikely(expr)		__builtin_expect(!!(expr), 0)
#define prefetchr(addr)		__builtin_prefetch((addr), 0)
#define prefetchw(addr)		__builtin_prefetch((addr), 1)
#define _aligned_attribute(n)	__attribute__((aligned(n)))

/* Newer gcc supports __BYTE_ORDER__.  Older gcc doesn't.  */
#ifdef __BYTE_ORDER__
#  define CPU_IS_LITTLE_ENDIAN() (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#endif

/* With gcc, we can access unaligned memory through 'packed' structures.  */
#define DEFINE_UNALIGNED_TYPE(type)				\
								\
struct type##unaligned {					\
	type v;							\
} __attribute__((packed));					\
								\
static forceinline type						\
load_##type##_unaligned(const void *p)				\
{								\
	return ((const struct type##unaligned *)p)->v;		\
}								\
								\
static forceinline void						\
store_##type##_unaligned(type v, void *p)			\
{								\
	((struct type##unaligned *)p)->v = v;			\
}

#if defined(__x86_64__) || defined(__i386__) || defined(__ARM_FEATURE_UNALIGNED)
#  define UNALIGNED_ACCESS_IS_FAST 1
#endif

#if GCC_PREREQ(4, 8)
#  define compiler_bswap16	__builtin_bswap16
#endif

#if GCC_PREREQ(4, 3)
#  define compiler_bswap32	__builtin_bswap32
#  define compiler_bswap64	__builtin_bswap64
#endif

#define compiler_fls32(n)	(31 - __builtin_clz(n))
#define compiler_fls64(n)	(63 - __builtin_clzll(n))
#define compiler_ffs32(n)	__builtin_ctz(n)
#define compiler_ffs64(n)	__builtin_ctzll(n)

/* Does the compiler support the 'target' function attribute?  */
#define COMPILER_SUPPORTS_TARGET_FUNCTION_ATTRIBUTE		\
	(CLANG_PREREQ(3, 7) || GCC_PREREQ(4, 4))

/* Does the compiler support __attribute__((target("bmi2")))?  */
#define COMPILER_SUPPORTS_BMI2_TARGET				\
	(COMPILER_SUPPORTS_TARGET_FUNCTION_ATTRIBUTE &&		\
	 (CLANG_PREREQ(3, 7) || GCC_PREREQ(4, 7)))
