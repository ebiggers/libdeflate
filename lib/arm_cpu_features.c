/*
 * arm_cpu_features.c - feature detection for arm processors
 *
 * Copyright 2017 Jun He <jun.he@linaro.org>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <sys/auxv.h>
#include <asm/hwcap.h>

#include "arm_cpu_features.h"

#if ARM_CPU_FEATURES_ENABLED

u32 _arm_cpu_features = 0;

#define IS_SET(val, feature_mask) ((val) & feature_mask)

/* read cpu features from avaux. */
void
arm_setup_cpu_features(void)
{
	u32 features = getauxval(AT_HWCAP);

	/* Standard feature flags  */
	if (IS_SET(features, HWCAP_FP))
		features |= ARM_CPU_FEATURE_FP;

	if (IS_SET(features, HWCAP_ASIMD))
		features |= ARM_CPU_FEATURE_NEON;

	if (IS_SET(features, HWCAP_EVTSTRM))
		features |= ARM_CPU_FEATURE_EVTSTRM;

	if (IS_SET(features, HWCAP_AES))
		features |= ARM_CPU_FEATURE_AES;

	if (IS_SET(features, HWCAP_PMULL))
		features |= ARM_CPU_FEATURE_PMULL;

	if (IS_SET(features, HWCAP_SHA1))
		features |= ARM_CPU_FEATURE_SHA1;

	if (IS_SET(features, HWCAP_SHA2))
		features |= ARM_CPU_FEATURE_SHA2;

	if (IS_SET(features, HWCAP_CRC32))
		features |= ARM_CPU_FEATURE_CRC32;

	if (IS_SET(features, HWCAP_ATOMICS))
		features |= ARM_CPU_FEATURE_ATOMICS;

	_arm_cpu_features = features | ARM_CPU_FEATURES_KNOWN;
}

#endif /* ARM_CPU_FEATURES_ENABLED */
