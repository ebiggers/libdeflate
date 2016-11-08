/*
 * compiler_gcc.h - definitions for the GNU C Compiler.  This also handles clang
 * and the Intel C Compiler (icc).
 *
 * TODO: icc is not well tested, so some things are currently disabled even
 * though they maybe can be enabled on some icc versions.
 */

#if !defined(__clang__) && !defined(__INTEL_COMPILER)
#  define GCC_PREREQ(major, minor)		\
	(__GNUC__ > (major) ||			\
	 (__GNUC__ == (major) && __GNUC_MINOR__ >= (minor)))
#else
#  define GCC_PREREQ(major, minor)	0
#endif

/* Note: only check the clang version when absolutely necessary!
 * "Vendors" such as Apple can use different version numbers. */
#ifdef __clang__
#  ifdef __apple_build_version__
#    define CLANG_PREREQ(major, minor, apple_version)	\
	(__apple_build_version__ >= (apple_version))
#  else
#    define CLANG_PREREQ(major, minor, apple_version)	\
	(__clang_major__ > (major) ||			\
	 (__clang_major__ == (major) && __clang_minor__ >= (minor)))
#  endif
#else
#  define CLANG_PREREQ(major, minor, apple_version)	0
#endif

#ifndef __has_attribute
#  define __has_attribute(attribute)	0
#endif
#ifndef __has_feature
#  define __has_feature(feature)	0
#endif
#ifndef __has_builtin
#  define __has_builtin(builtin)	0
#endif

#ifdef _WIN32
#  define LIBEXPORT __declspec(dllexport)
#else
#  define LIBEXPORT __attribute__((visibility("default")))
#endif

#define inline			inline
#define forceinline		inline __attribute__((always_inline))
#define restrict		__restrict__
#define likely(expr)		__builtin_expect(!!(expr), 1)
#define unlikely(expr)		__builtin_expect(!!(expr), 0)
#define prefetchr(addr)		__builtin_prefetch((addr), 0)
#define prefetchw(addr)		__builtin_prefetch((addr), 1)
#define _aligned_attribute(n)	__attribute__((aligned(n)))

/*
 * Support for the following x86 instruction set extensions was introduced by
 * the following gcc versions:
 *
 *	PCLMUL	4.4
 *	AVX	4.6
 *	BMI2	4.7
 *	AVX2	4.7
 *
 * With clang, __has_builtin() can be used to detect the presence of one of the
 * associated builtins.
 *
 * Additionally, gcc 4.4 introduced the 'target' function attribute.  With
 * clang, support for this can be detected with with __has_attribute(target).
 *
 * However, prior to gcc 4.9 and clang 3.8, x86 intrinsics not available in the
 * main target could not be used in 'target' attribute functions.  Unfortunately
 * clang has no feature test macro for this so we have to check its version.
 */
#define COMPILER_SUPPORTS_TARGET_FUNCTION_ATTRIBUTE		\
	(GCC_PREREQ(4, 4) || __has_attribute(target))
#if COMPILER_SUPPORTS_TARGET_FUNCTION_ATTRIBUTE
#  define COMPILER_SUPPORTS_TARGET_INTRINSICS			\
	(GCC_PREREQ(4, 9) || CLANG_PREREQ(3, 8, 7030000))
#  define COMPILER_SUPPORTS_PCLMUL_TARGET			\
	(GCC_PREREQ(4, 4) || __has_builtin(__builtin_ia32_pclmulqdq128))
#  define COMPILER_SUPPORTS_AVX_TARGET				\
	(GCC_PREREQ(4, 6) || __has_builtin(__builtin_ia32_maxps256))
#  define COMPILER_SUPPORTS_BMI2_TARGET				\
	(GCC_PREREQ(4, 7) || __has_builtin(__builtin_ia32_pdep_di))
#  define COMPILER_SUPPORTS_AVX2_TARGET				\
	(GCC_PREREQ(4, 7) || __has_builtin(__builtin_ia32_pmaddwd256))
#endif

/* Newer gcc supports __BYTE_ORDER__.  Older gcc doesn't. */
#ifdef __BYTE_ORDER__
#  define CPU_IS_LITTLE_ENDIAN() (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#endif

#if GCC_PREREQ(4, 8) || __has_builtin(__builtin_bswap16)
#  define bswap16	__builtin_bswap16
#endif

#if GCC_PREREQ(4, 3) || __has_builtin(__builtin_bswap32)
#  define bswap32	__builtin_bswap32
#endif

#if GCC_PREREQ(4, 3) || __has_builtin(__builtin_bswap64)
#  define bswap64	__builtin_bswap64
#endif

#if defined(__x86_64__) || defined(__i386__) || defined(__ARM_FEATURE_UNALIGNED)
#  define UNALIGNED_ACCESS_IS_FAST 1
#endif

/* With gcc, we can access unaligned memory through 'packed' structures. */
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

#define bsr32(n)	(31 - __builtin_clz(n))
#define bsr64(n)	(63 - __builtin_clzll(n))
#define bsf32(n)	__builtin_ctz(n)
#define bsf64(n)	__builtin_ctzll(n)
