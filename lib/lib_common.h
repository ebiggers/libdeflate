/*
 * lib_common.h - internal header included by all library code
 */

#ifndef LIB_LIB_COMMON_H
#define LIB_LIB_COMMON_H

#ifdef LIBDEFLATE_H
#  error "lib_common.h must always be included before libdeflate.h"
   /* because BUILDING_LIBDEFLATE must be set first */
#endif

#define BUILDING_LIBDEFLATE

#include "../common_defs.h"

#include <stdio.h>
#include <inttypes.h>
#include <time.h>

static forceinline u64 now(void)
{
#ifdef __x86_64__
	u32 lo, hi;
	__asm__ volatile("rdtsc" : "=a" (lo), "=d" (hi));
	return ((u64)hi << 32) | lo;
#elif defined(__i386__)
	u32 lo, hi;
	__asm__ volatile("rdtsc" : "=a" (lo), "=d" (hi));
	return ((u64)hi << 32) | lo;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return 1000000000 * (u64)ts.tv_sec + ts.tv_nsec;
#endif
}
static u64 g_start_time __attribute__((unused));
static u64 g_total_time __attribute__((unused));
static u64 g_num_measurements __attribute__((unused));

#define BEGIN g_start_time = now();
#define END  g_total_time += now() - g_start_time; g_num_measurements++;

void *libdeflate_malloc(size_t size);
void libdeflate_free(void *ptr);

void *libdeflate_aligned_malloc(size_t alignment, size_t size);
void libdeflate_aligned_free(void *ptr);

#ifdef FREESTANDING
/*
 * With -ffreestanding, <string.h> may be missing, and we must provide
 * implementations of memset(), memcpy(), memmove(), and memcmp().
 * See https://gcc.gnu.org/onlinedocs/gcc/Standards.html
 *
 * Also, -ffreestanding disables interpreting calls to these functions as
 * built-ins.  E.g., calling memcpy(&v, p, WORDBYTES) will make a function call,
 * not be optimized to a single load instruction.  For performance reasons we
 * don't want that.  So, declare these functions as macros that expand to the
 * corresponding built-ins.  This approach is recommended in the gcc man page.
 * We still need the actual function definitions in case gcc calls them.
 */
void *memset(void *s, int c, size_t n);
#define memset(s, c, n)		__builtin_memset((s), (c), (n))

void *memcpy(void *dest, const void *src, size_t n);
#define memcpy(dest, src, n)	__builtin_memcpy((dest), (src), (n))

void *memmove(void *dest, const void *src, size_t n);
#define memmove(dest, src, n)	__builtin_memmove((dest), (src), (n))

int memcmp(const void *s1, const void *s2, size_t n);
#define memcmp(s1, s2, n)	__builtin_memcmp((s1), (s2), (n))

#undef LIBDEFLATE_ENABLE_ASSERTIONS
#else
#include <string.h>
#endif

/*
 * Runtime assertion support.  Don't enable this in production builds; it may
 * hurt performance significantly.
 */
#ifdef LIBDEFLATE_ENABLE_ASSERTIONS
void libdeflate_assertion_failed(const char *expr, const char *file, int line);
#define ASSERT(expr) { if (unlikely(!(expr))) \
	libdeflate_assertion_failed(#expr, __FILE__, __LINE__); }
#else
#define ASSERT(expr) (void)(expr)
#endif

#define CONCAT_IMPL(a, b)	a##b
#define CONCAT(a, b)		CONCAT_IMPL(a, b)
#define ADD_SUFFIX(name)	CONCAT(name, SUFFIX)

#endif /* LIB_LIB_COMMON_H */
