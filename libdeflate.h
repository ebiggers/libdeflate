/*
 * libdeflate.h
 *
 * Public header for the DEFLATE compression library.
 */

#ifndef LIBDEFLATE_H
#define LIBDEFLATE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

/* ========================================================================== */
/*                             Compression                                    */
/* ========================================================================== */

struct deflate_compressor;

/*
 * deflate_alloc_compressor() allocates a new DEFLATE compressor.
 * 'compression_level' is the compression level on a zlib-like scale (1 =
 * fastest, 6 = medium/default, 9 = slowest).  The return value is a pointer to
 * the new DEFLATE compressor, or NULL if out of memory.
 *
 * Note: the sliding window size is defined at compilation time (default 32768).
 */
extern struct deflate_compressor *
deflate_alloc_compressor(unsigned int compression_level);

/*
 * deflate_compress() performs DEFLATE compression on a buffer of data.  The
 * function attempts to compress 'in_nbytes' bytes of data located at 'in' and
 * write the results to 'out', which has space for 'out_nbytes_avail' bytes.
 * The return value is the compressed size in bytes, or 0 if the data could not
 * be compressed to 'out_nbytes_avail' bytes or fewer.
 */
extern size_t
deflate_compress(struct deflate_compressor *compressor,
		 const void *in, size_t in_nbytes,
		 void *out, size_t out_nbytes_avail);

/*
 * Like deflate_compress(), but store the data in the zlib wrapper format.
 */
extern size_t
zlib_compress(struct deflate_compressor *compressor,
	      const void *in, size_t in_nbytes,
	      void *out, size_t out_nbytes_avail);

/*
 * Like deflate_compress(), but store the data in the gzip wrapper format.
 */
extern size_t
gzip_compress(struct deflate_compressor *compressor,
	      const void *in, size_t in_nbytes,
	      void *out, size_t out_nbytes_avail);

/*
 * deflate_free_compressor() frees a DEFLATE compressor that was allocated with
 * deflate_alloc_compressor().
 */
extern void
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
 * sliding window size.
 */
extern struct deflate_decompressor *
deflate_alloc_decompressor(void);

/*
 * deflate_decompress() decompresses 'in_nbytes' bytes of DEFLATE-compressed
 * data at 'in' and writes the uncompressed data, which had original size
 * 'out_nbytes', to 'out'.  The return value is true if decompression was
 * successful, or false if the compressed data was invalid.
 *
 * To be clear: the uncompressed size must be known *exactly* and passed as
 * 'out_nbytes'.
 */
extern bool
deflate_decompress(struct deflate_decompressor *decompressor,
		   const void *in, size_t in_nbytes,
		   void *out, size_t out_nbytes);

/*
 * Like deflate_decompress(), but assumes the zlib wrapper format instead of raw
 * DEFLATE.
 */
extern bool
zlib_decompress(struct deflate_decompressor *decompressor,
		const void *in, size_t in_nbytes,
		void *out, size_t out_nbytes);

/*
 * Like deflate_decompress(), but assumes the gzip wrapper format instead of raw
 * DEFLATE.
 */
extern bool
gzip_decompress(struct deflate_decompressor *decompressor,
		const void *in, size_t in_nbytes,
		void *out, size_t out_nbytes);

/*
 * deflate_free_decompressor() frees a DEFLATE decompressor that was allocated
 * with deflate_alloc_decompressor().
 */
extern void
deflate_free_decompressor(struct deflate_decompressor *decompressor);


#ifdef __cplusplus
}
#endif

#endif /* LIBDEFLATE_H */
