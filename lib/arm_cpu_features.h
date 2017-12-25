/*
 * arm_cpu_features.h - feature detection for arm processors
 */

#ifndef LIB_ARM_CPU_FEATURES_H
#define LIB_ARM_CPU_FEATURES_H

#include "lib_common.h"

#if (defined(__arm__) || defined(__aarch64__)) && COMPILER_SUPPORTS_TARGET_FUNCTION_ATTRIBUTE
#  define ARM_CPU_FEATURES_ENABLED 1
#else
#  define ARM_CPU_FEATURES_ENABLED 0
#endif

#if ARM_CPU_FEATURES_ENABLED

#define ARM_CPU_FEATURE_FP		0x00000001
#define ARM_CPU_FEATURE_NEON		0x00000002
#define ARM_CPU_FEATURE_EVTSTRM 	0x00000004
#define ARM_CPU_FEATURE_AES		0x00000008
#define ARM_CPU_FEATURE_PMULL		0x00000010
#define ARM_CPU_FEATURE_SHA1		0x00000020
#define ARM_CPU_FEATURE_SHA2		0x00000040
#define ARM_CPU_FEATURE_CRC32		0x00000080
#define ARM_CPU_FEATURE_ATOMICS 	0x00000100

#define ARM_CPU_FEATURES_KNOWN		0x80000000

extern u32 _arm_cpu_features;

extern void
arm_setup_cpu_features(void);

/* Does the processor have the specified feature(s)?  */
static inline bool
arm_have_cpu_features(u32 features)
{
	if (_arm_cpu_features == 0)
		arm_setup_cpu_features();
	return (_arm_cpu_features & features) == features;
}

#endif /* ARM_CPU_FEATURES_ENABLED */

#endif /* LIB_ARM_CPU_FEATURES_H */
