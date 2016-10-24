/*
 * x86_cpu_features.h - feature detection for x86 processors
 */

#ifndef LIB_X86_CPU_FEATURES_H
#define LIB_X86_CPU_FEATURES_H

#include "lib_common.h"

#if defined(__x86_64__) && COMPILER_SUPPORTS_TARGET_FUNCTION_ATTRIBUTE
#  define X86_CPU_FEATURES_ENABLED 1
#else
#  define X86_CPU_FEATURES_ENABLED 0
#endif

#if X86_CPU_FEATURES_ENABLED

#define X86_CPU_FEATURE_SSE		0x00000001
#define X86_CPU_FEATURE_SSE2		0x00000002
#define X86_CPU_FEATURE_SSE3		0x00000004
#define X86_CPU_FEATURE_PCLMULQDQ	0x00000008
#define X86_CPU_FEATURE_SSSE3		0x00000010
#define X86_CPU_FEATURE_SSE4_1		0x00000020
#define X86_CPU_FEATURE_SSE4_2		0x00000040
#define X86_CPU_FEATURE_AVX		0x00000080
#define X86_CPU_FEATURE_BMI		0x00000100
#define X86_CPU_FEATURE_AVX2		0x00000200
#define X86_CPU_FEATURE_BMI2		0x00000400

#define X86_CPU_FEATURES_KNOWN		0x80000000

extern u32 _x86_cpu_features;

extern void
x86_setup_cpu_features(void);

/* Does the processor have the specified feature(s)?  */
static inline bool
x86_have_cpu_features(u32 features)
{
	if (_x86_cpu_features == 0)
		x86_setup_cpu_features();
	return (_x86_cpu_features & features) == features;
}

#endif /* X86_CPU_FEATURES_ENABLED */

#endif /* LIB_X86_CPU_FEATURES_H */
