/*
 * libdeflate.h - public header for libdeflate
 */

#ifndef LIBDEFLATE_H
#define LIBDEFLATE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* Microsoft C / Visual Studio garbage.  If you want to link to the DLL version
 * of libdeflate, then #define LIBDEFLATE_DLL.  */
#ifdef _MSC_VER
#  ifdef BUILDING_LIBDEFLATE
#    define LIBDEFLATEAPI __declspec(dllexport)
#  elif defined(LIBDEFLATE_DLL)
#    define LIBDEFLATEAPI __declspec(dllimport)
#  endif
#endif
#ifndef LIBDEFLATEAPI
#  define LIBDEFLATEAPI
#endif

/* ========================================================================== */
/*                             Compression                                    */
/* ========================================================================== */

struct deflate_compressor;

/*
 * deflate_alloc_compressor() allocates a new DEFLATE compressor.
 * 'compression_level' is the compression level on a zlib-like scale but with a
 * higher maximum value (1 = fastest, 6 = medium/default, 9 = slow, 12 =
 * slowest).  The return value is a pointer to the new DEFLATE compressor, or
 * NULL if out of memory.
 *
 * Note: for compression, the sliding window size is defined at compilation time
 * to 32768, the largest size permissible in the DEFLATE format.  It cannot be
 * changed at runtime.
 */
LIBDEFLATEAPI struct deflate_compressor *
deflate_alloc_compressor(unsigned int compression_level);

/*
 * deflate_compress() performs DEFLATE compression on a buffer of data.  The
 * function attempts to compress 'in_nbytes' bytes of data located at 'in' and
 * write the results to 'out', which has space for 'out_nbytes_avail' bytes.
 * The return value is the compressed size in bytes, or 0 if the data could not
 * be compressed to 'out_nbytes_avail' bytes or fewer.
 */
LIBDEFLATEAPI size_t
deflate_compress(struct deflate_compressor *compressor,
		 const void *in, size_t in_nbytes,
		 void *out, size_t out_nbytes_avail);

/*
 * deflate_compress_bound() returns a worst-case upper bound on the number of
 * bytes of compressed data that may be produced by compressing any buffer of
 * length less than or equal to 'in_nbytes' using deflate_compress() with the
 * specified compressor.  Mathematically, this bound will necessarily be a
 * number greater than or equal to 'in_nbytes'.  It may be an overestimate of
 * the true upper bound.  The return value is guaranteed to be the same for all
 * invocations with the same compressor and same 'in_nbytes'.
 *
 * As a special case, 'compressor' may be NULL.  This causes the bound to be
 * taken across *any* deflate_compressor that could ever be allocated with this
 * build of the library, with any options.
 *
 * Note that this function is not necessary in many applications.  With
 * block-based compression, it is usually preferable to separately store the
 * uncompressed size of each block and to store any blocks that did not compress
 * to less than their original size uncompressed.  In that scenario, there is no
 * need to know the worst-case compressed size, since the maximum number of
 * bytes of compressed data that may be used would always be one less than the
 * input length.  You can just pass a buffer of that size to deflate_compress()
 * and store the data uncompressed if deflate_compress() returns 0, indicating
 * that the compressed data did not fit into the provided output buffer.
 */
LIBDEFLATEAPI size_t
deflate_compress_bound(struct deflate_compressor *compressor, size_t in_nbytes);

/*
 * Like deflate_compress(), but stores the data in the zlib wrapper format.
 */
LIBDEFLATEAPI size_t
zlib_compress(struct deflate_compressor *compressor,
	      const void *in, size_t in_nbytes,
	      void *out, size_t out_nbytes_avail);

/*
 * Like deflate_compress_bound(), but assumes the data will be compressed with
 * zlib_compress() rather than with deflate_compress().
 */
LIBDEFLATEAPI size_t
zlib_compress_bound(struct deflate_compressor *compressor, size_t in_nbytes);

/*
 * Like deflate_compress(), but stores the data in the gzip wrapper format.
 */
LIBDEFLATEAPI size_t
gzip_compress(struct deflate_compressor *compressor,
	      const void *in, size_t in_nbytes,
	      void *out, size_t out_nbytes_avail);

/*
 * Like deflate_compress_bound(), but assumes the data will be compressed with
 * gzip_compress() rather than with deflate_compress().
 */
LIBDEFLATEAPI size_t
gzip_compress_bound(struct deflate_compressor *compressor, size_t in_nbytes);

/*
 * deflate_free_compressor() frees a DEFLATE compressor that was allocated with
 * deflate_alloc_compressor().  If a NULL pointer is passed in, no action is
 * taken.
 */
LIBDEFLATEAPI void
deflate_free_compressor(struct deflate_compressor *compressor);

/* ========================================================================== */
/*                             Decompression                                  */
/* ========================================================================== */

struct deflate_decompressor;

/*
 * deflate_alloc_decompressor() allocates a new DEFLATE decompressor.  The
 * return value is a pointer to the new DEFLATE decompressor, or NULL if out of
 * memory.
 *
 * This function takes no parameters, and the returned decompressor is valid for
 * decompressing data that was compressed at any compression level and with any
 * sliding window size.  It can also be used for any wrapper format (raw
 * DEFLATE, zlib, or gzip); however, the appropriate decompression function must
 * be called.
 */
LIBDEFLATEAPI struct deflate_decompressor *
deflate_alloc_decompressor(void);

/*
 * Result of a call to deflate_decompress(), zlib_decompress(), or
 * gzip_decompress().
 */
enum decompress_result {
	/* Decompression was successful.  */
	DECOMPRESS_SUCCESS = 0,

	/* Decompressed failed because the compressed data was invalid, corrupt,
	 * or otherwise unsupported.  */
	DECOMPRESS_BAD_DATA = 1,

	/* A NULL 'actual_out_nbytes_ret' was provided, but the data would have
	 * decompressed to fewer than 'out_nbytes_avail' bytes.  */
	DECOMPRESS_SHORT_OUTPUT = 2,

	/* The data would have decompressed to more than 'out_nbytes_avail'
	 * bytes.  */
	DECOMPRESS_INSUFFICIENT_SPACE = 3,
};

/*
 * deflate_decompress() decompresses 'in_nbytes' bytes of DEFLATE-compressed
 * data at 'in' and writes the uncompressed data to 'out', which is a buffer of
 * at least 'out_nbytes_avail' bytes.  If decompression was successful, then 0
 * (DECOMPRESS_SUCCESS) is returned; otherwise, a nonzero result code such as
 * DECOMPRESS_BAD_DATA is returned.  If a nonzero result code is returned, then
 * the contents of the output buffer are undefined.
 *
 * deflate_decompress() can be used in cases where the actual uncompressed size
 * is known (recommended) or unknown (not recommended):
 *
 *   - If the actual uncompressed size is known, then pass the actual
 *     uncompressed size as 'out_nbytes_avail' and pass NULL for
 *     'actual_out_nbytes_ret'.  This makes deflate_decompress() fail with
 *     DECOMPRESS_SHORT_OUTPUT if the data decompressed to fewer than the
 *     specified number of bytes.
 *
 *   - If the actual uncompressed size is unknown, then provide a non-NULL
 *     'actual_out_nbytes_ret' and provide a buffer with some size
 *     'out_nbytes_avail' that you think is large enough to hold all the
 *     uncompressed data.  In this case, if the data decompresses to less than
 *     or equal to 'out_nbytes_avail' bytes, then deflate_decompress() will
 *     write the actual uncompressed size to *actual_out_nbytes_ret and return 0
 *     (DECOMPRESS_SUCCESS).  Otherwise, it will return
 *     DECOMPRESS_INSUFFICIENT_SPACE if the provided buffer was not large enough
 *     but no other problems were encountered, or another nonzero result code if
 *     decompression failed for another reason.
 */
LIBDEFLATEAPI enum decompress_result
deflate_decompress(struct deflate_decompressor *decompressor,
		   const void *in, size_t in_nbytes,
		   void *out, size_t out_nbytes_avail,
		   size_t *actual_out_nbytes_ret);

/*
 * Like deflate_decompress(), but assumes the zlib wrapper format instead of raw
 * DEFLATE.
 */
LIBDEFLATEAPI enum decompress_result
zlib_decompress(struct deflate_decompressor *decompressor,
		const void *in, size_t in_nbytes,
		void *out, size_t out_nbytes_avail,
		size_t *actual_out_nbytes_ret);

/*
 * Like deflate_decompress(), but assumes the gzip wrapper format instead of raw
 * DEFLATE.
 */
LIBDEFLATEAPI enum decompress_result
gzip_decompress(struct deflate_decompressor *decompressor,
		const void *in, size_t in_nbytes,
		void *out, size_t out_nbytes_avail,
		size_t *actual_out_nbytes_ret);

/*
 * deflate_free_decompressor() frees a DEFLATE decompressor that was allocated
 * with deflate_alloc_decompressor().  If a NULL pointer is passed in, no action
 * is taken.
 */
LIBDEFLATEAPI void
deflate_free_decompressor(struct deflate_decompressor *decompressor);


#ifdef __cplusplus
}
#endif

#endif /* LIBDEFLATE_H */
