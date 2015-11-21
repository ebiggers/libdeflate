/*
 * aligned_malloc.c - aligned memory allocation
 *
 * This file provides portable aligned memory allocation functions that only
 * use malloc() and free().  This avoids portability problems with
 * posix_memalign(), aligned_alloc(), etc.
 *
 * Author:	Eric Biggers
 * Year:	2014, 2015
 *
 * The author dedicates this file to the public domain.
 * You can do whatever you want with this file.
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
