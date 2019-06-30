/*
 * arm/cpu_features.c - feature detection for ARM processors
 *
 * Copyright 2018 Eric Biggers
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

/*
 * ARM processors don't have a standard way for unprivileged programs to detect
 * processor features.  But, on Linux we can read the AT_HWCAP and AT_HWCAP2
 * values from /proc/self/auxv.
 *
 * Ideally we'd use the C library function getauxval(), but it's not guaranteed
 * to be available: it was only added to glibc in 2.16, and in Android it was
 * added to API level 18 for ARM and level 21 for AArch64.
 */

#include "cpu_features.h"

#if ARM_CPU_FEATURES_ENABLED

#ifdef __linux__

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define AT_HWCAP	16
#define AT_HWCAP2	26

volatile u32 _cpu_features = 0;

static void scan_auxv(unsigned long *hwcap, unsigned long *hwcap2)
{
	int fd;
	unsigned long auxbuf[32];
	int filled = 0;
	int i;

	fd = open("/proc/self/auxv", O_RDONLY);
	if (fd < 0)
		return;

	for (;;) {
		do {
			int ret = read(fd, &((char *)auxbuf)[filled],
				       sizeof(auxbuf) - filled);
			if (ret <= 0) {
				if (ret < 0 && errno == EINTR)
					continue;
				goto out;
			}
			filled += ret;
		} while (filled < 2 * sizeof(long));

		i = 0;
		do {
			unsigned long type = auxbuf[i];
			unsigned long value = auxbuf[i + 1];

			if (type == AT_HWCAP)
				*hwcap = value;
			else if (type == AT_HWCAP2)
				*hwcap2 = value;
			i += 2;
			filled -= 2 * sizeof(long);
		} while (filled >= 2 * sizeof(long));

		memmove(auxbuf, &auxbuf[i], filled);
	}
out:
	close(fd);
}

void setup_cpu_features(void)
{
	u32 features = 0;
	unsigned long hwcap = 0;
	unsigned long hwcap2 = 0;

	scan_auxv(&hwcap, &hwcap2);

#ifdef __arm__
	STATIC_ASSERT(sizeof(long) == 4);
	if (hwcap & (1 << 12))	/* HWCAP_NEON */
		features |= ARM_CPU_FEATURE_NEON;
	if (hwcap2 & (1 << 1))	/* HWCAP2_PMULL */
		features |= ARM_CPU_FEATURE_PMULL;
#else
	STATIC_ASSERT(sizeof(long) == 8);
	if (hwcap & (1 << 1))	/* HWCAP_ASIMD */
		features |= ARM_CPU_FEATURE_NEON;
	if (hwcap & (1 << 4))	/* HWCAP_PMULL */
		features |= ARM_CPU_FEATURE_PMULL;
#endif

	_cpu_features = features | ARM_CPU_FEATURES_KNOWN;
}

#elif defined(__aarch64__) /* not __linux__ */

volatile u32 _cpu_features = 0;

void setup_cpu_features(void)
{
	/* NEON is a mandatory part of AArch64 */
	_cpu_features = ARM_CPU_FEATURE_NEON;

	/* On FreeBSD >= 12, Linux >= 4.11 and potentially others, it is possible to use
	   privileged system registers from userspace to check CPU feature support.

	   For proper support of SoCs where different cores have different capabilities,
	   the OS has to always report only the features supported by all cores, like FreeBSD does. */

#define bits_shift(x, high, low) ((x >> low) & ((1 << (high - low + 1)) - 1))

#if __FreeBSD__ >= 12
	uint64_t isar0 = 0;
	__asm__("mrs %0, ID_AA64ISAR0_EL1" : "=r"(isar0));

	if (bits_shift(isar0, 7, 4) >= 1)
		_cpu_features |= ARM_CPU_FEATURE_PMULL;
#endif

	_cpu_features |= ARM_CPU_FEATURES_KNOWN;
}

#endif

#endif /* ARM_CPU_FEATURES_ENABLED */
