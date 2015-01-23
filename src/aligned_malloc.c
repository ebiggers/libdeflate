/*
 * aligned_malloc.c
 *
 * Aligned memory allocation using only malloc() and free().
 * Avoids portability problems with posix_memalign(), aligned_alloc(), etc.
 *
 * This file has no copyright assigned and is placed in the Public Domain.
 */

#include "aligned_malloc.h"

#include <stdint.h>
#include <stdlib.h>

void *
aligned_malloc(size_t alignment, size_t size)
{
	const uintptr_t mask = alignment - 1;
	char *ptr = NULL;
	char *raw_ptr;

	raw_ptr = malloc(mask + sizeof(size_t) + size);
	if (raw_ptr) {
		ptr = (char *)raw_ptr + sizeof(size_t);
		ptr = (void *)(((uintptr_t)ptr + mask) & ~mask);
		*((size_t *)ptr - 1) = ptr - raw_ptr;
	}
	return ptr;
}

void
aligned_free(void *ptr)
{
	if (ptr)
		free((char *)ptr - *((size_t *)ptr - 1));
}
