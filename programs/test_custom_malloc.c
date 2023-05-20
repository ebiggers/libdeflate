/*
 * test_custom_malloc.c
 *
 * Test the support for custom memory allocators.
 * Also test injecting allocation failures.
 */

#include "test_util.h"

static int malloc_count = 0;
static int free_count = 0;

static void *do_malloc(size_t size)
{
	malloc_count++;
	return malloc(size);
}

static void *do_fail_malloc(size_t size)
{
	malloc_count++;
	return NULL;
}

static void do_free(void *ptr)
{
	free_count++;
	free(ptr);
}

static void reset_state(void)
{
	libdeflate_set_memory_allocator(malloc, free);
	malloc_count = 0;
	free_count = 0;
}

/* Test that the custom allocator is actually used when requested. */
static void do_custom_memalloc_test(bool global)
{
	static const struct libdeflate_options options = {
		.sizeof_options = sizeof(options),
		.malloc_func = do_malloc,
		.free_func = do_free,
	};
	int level;
	struct libdeflate_compressor *c;
	struct libdeflate_decompressor *d;

	if (global)
		libdeflate_set_memory_allocator(do_malloc, do_free);

	for (level = 0; level <= 12; level++) {
		malloc_count = free_count = 0;
		if (global)
			c = libdeflate_alloc_compressor(level);
		else
			c = libdeflate_alloc_compressor_ex(level, &options);
		ASSERT(c != NULL);
		ASSERT(malloc_count == 1);
		ASSERT(free_count == 0);
		libdeflate_free_compressor(c);
		ASSERT(malloc_count == 1);
		ASSERT(free_count == 1);
	}

	malloc_count = free_count = 0;
	if (global)
		d = libdeflate_alloc_decompressor();
	else
		d = libdeflate_alloc_decompressor_ex(&options);
	ASSERT(d != NULL);
	ASSERT(malloc_count == 1);
	ASSERT(free_count == 0);
	libdeflate_free_decompressor(d);
	ASSERT(malloc_count == 1);
	ASSERT(free_count == 1);

	reset_state();
}

#define offsetofend(type, field) \
	(offsetof(type, field) + sizeof(((type *)NULL)->field))

/* Test some edge cases involving libdeflate_options. */
static void do_options_test(void)
{
	struct libdeflate_options options = { 0 };
	struct libdeflate_compressor *c;
	struct libdeflate_decompressor *d;
	/* Size in libdeflate v1.19 */
	size_t min_size = offsetofend(struct libdeflate_options, free_func);

	/* sizeof_options must be at least the minimum size. */
	for (; options.sizeof_options < min_size;
	     options.sizeof_options++) {
		c = libdeflate_alloc_compressor_ex(6, &options);
		ASSERT(c == NULL);
		d = libdeflate_alloc_decompressor_ex(&options);
		ASSERT(d == NULL);
	}

	/* NULL malloc_func and free_func means "use the global allocator". */
	options.sizeof_options = min_size;
	malloc_count = free_count = 0;
	libdeflate_set_memory_allocator(do_malloc, do_free);
	c = libdeflate_alloc_compressor_ex(6, &options);
	libdeflate_free_compressor(c);
	ASSERT(malloc_count == 1);
	ASSERT(free_count == 1);
	d = libdeflate_alloc_decompressor_ex(&options);
	libdeflate_free_decompressor(d);
	ASSERT(malloc_count == 2);
	ASSERT(free_count == 2);

	reset_state();
}

/* Test injecting memory allocation failures. */
static void do_fault_injection_test(void)
{
	int level;
	struct libdeflate_compressor *c;
	struct libdeflate_decompressor *d;

	libdeflate_set_memory_allocator(do_fail_malloc, do_free);

	for (level = 0; level <= 12; level++) {
		malloc_count = free_count = 0;
		c = libdeflate_alloc_compressor(level);
		ASSERT(c == NULL);
		ASSERT(malloc_count == 1);
		ASSERT(free_count == 0);
	}

	malloc_count = free_count = 0;
	d = libdeflate_alloc_decompressor();
	ASSERT(d == NULL);
	ASSERT(malloc_count == 1);
	ASSERT(free_count == 0);

	reset_state();
}

int
tmain(int argc, tchar *argv[])
{
	begin_program(argv);

	do_custom_memalloc_test(true);
	do_custom_memalloc_test(false);
	do_options_test();
	do_fault_injection_test();
	return 0;
}
