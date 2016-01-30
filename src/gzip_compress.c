/*
 * gzip_compress.c - compress with a gzip wrapper
 *
 * Author:	Eric Biggers
 * Year:	2014-2016
 *
 * The author dedicates this file to the public domain.
 * You can do whatever you want with this file.
 */

#include "crc32.h"
#include "deflate_compress.h"
#include "gzip_constants.h"
#include "unaligned.h"

#include "libdeflate.h"

LIBEXPORT size_t
gzip_compress(struct deflate_compressor *c, const void *in, size_t in_size,
	      void *out, size_t out_nbytes_avail)
{
	u8 *out_next = out;
	unsigned compression_level;
	u8 xfl;
	size_t deflate_size;

	if (out_nbytes_avail <= GZIP_MIN_OVERHEAD)
		return 0;

	/* ID1 */
	*out_next++ = GZIP_ID1;
	/* ID2 */
	*out_next++ = GZIP_ID2;
	/* CM */
	*out_next++ = GZIP_CM_DEFLATE;
	/* FLG */
	*out_next++ = 0;
	/* MTIME */
	put_unaligned_le32(GZIP_MTIME_UNAVAILABLE, out_next);
	out_next += 4;
	/* XFL */
	xfl = 0;
	compression_level = deflate_get_compression_level(c);
	if (compression_level < 2)
		xfl |= GZIP_XFL_FASTEST_COMRESSION;
	else if (compression_level >= 8)
		xfl |= GZIP_XFL_SLOWEST_COMRESSION;
	*out_next++ = xfl;
	/* OS */
	*out_next++ = GZIP_OS_UNKNOWN;	/* OS  */

	/* Compressed data  */
	deflate_size = deflate_compress(c, in, in_size, out_next,
					out_nbytes_avail - GZIP_MIN_OVERHEAD);
	if (deflate_size == 0)
		return 0;
	out_next += deflate_size;

	/* CRC32 */
	put_unaligned_le32(crc32_gzip(in, in_size), out_next);
	out_next += 4;

	/* ISIZE */
	put_unaligned_le32((u32)in_size, out_next);
	out_next += 4;

	return out_next - (u8 *)out;
}

LIBEXPORT size_t
gzip_compress_bound(struct deflate_compressor *c, size_t in_nbytes)
{
	return GZIP_MIN_OVERHEAD + deflate_compress_bound(c, in_nbytes);
}
