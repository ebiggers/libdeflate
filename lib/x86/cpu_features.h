/*
 * x86/cpu_features.h - feature detection for x86 processors
 */

#ifndef LIB_X86_CPU_FEATURES_H
#define LIB_X86_CPU_FEATURES_H

#include "../lib_common.h"

#if (defined(__i386__) || defined(__x86_64__)) && \
	COMPILER_SUPPORTS_TARGET_FUNCTION_ATTRIBUTE
#  define X86_CPU_FEATURES_ENABLED 1
#else
#  define X86_CPU_FEATURES_ENABLED 0
#endif

#define X86_CPU_FEATURE_SSE2		0x00000001
#define X86_CPU_FEATURE_PCLMUL		0x00000002
#define X86_CPU_FEATURE_AVX		0x00000004
#define X86_CPU_FEATURE_AVX2		0x00000008
#define X86_CPU_FEATURE_BMI2		0x00000010
#define X86_CPU_FEATURE_AVX512BW	0x00000020

#if X86_CPU_FEATURES_ENABLED
#define X86_CPU_FEATURES_KNOWN		0x80000000
extern volatile u32 libdeflate_x86_cpu_features;

void libdeflate_init_x86_cpu_features(void);

static inline u32 get_x86_cpu_features(void)
{
	if (libdeflate_x86_cpu_features == 0)
		libdeflate_init_x86_cpu_features();
	return libdeflate_x86_cpu_features;
}
#else /* X86_CPU_FEATURES_ENABLED */
static inline u32 get_x86_cpu_features(void) { return 0; }
#endif /* !X86_CPU_FEATURES_ENABLED */

#endif /* LIB_X86_CPU_FEATURES_H */
