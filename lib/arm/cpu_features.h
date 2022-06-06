/*
 * arm/cpu_features.h - feature detection for ARM processors
 */

#ifndef LIB_ARM_CPU_FEATURES_H
#define LIB_ARM_CPU_FEATURES_H

#include "../lib_common.h"

#if (defined(__arm__) || defined(__aarch64__)) && \
	(defined(__linux__) || \
	 (defined(__aarch64__) && defined(__APPLE__))) && \
	COMPILER_SUPPORTS_TARGET_FUNCTION_ATTRIBUTE && \
	!defined(FREESTANDING)
#  define ARM_CPU_FEATURES_ENABLED 1
#else
#  define ARM_CPU_FEATURES_ENABLED 0
#endif

#define ARM_CPU_FEATURE_NEON		0x00000001
#define ARM_CPU_FEATURE_PMULL		0x00000002
#define ARM_CPU_FEATURE_CRC32		0x00000004
#define ARM_CPU_FEATURE_SHA3		0x00000008

#if ARM_CPU_FEATURES_ENABLED
#define ARM_CPU_FEATURES_KNOWN		0x80000000
extern volatile u32 libdeflate_arm_cpu_features;

void libdeflate_init_arm_cpu_features(void);

static inline u32 get_arm_cpu_features(void)
{
	if (libdeflate_arm_cpu_features == 0)
		libdeflate_init_arm_cpu_features();
	return libdeflate_arm_cpu_features;
}
#else /* ARM_CPU_FEATURES_ENABLED */
static inline u32 get_arm_cpu_features(void) { return 0; }
#endif /* !ARM_CPU_FEATURES_ENABLED */

#endif /* LIB_ARM_CPU_FEATURES_H */
