/*
 * gzip_compress.c
 *
 * Generate DEFLATE-compressed data in the gzip wrapper format.
 */

#include "libdeflate.h"

#include "crc32.h"
#include "deflate_compress.h"
#include "gzip_constants.h"
#include "unaligned.h"

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
	put_unaligned_u32_le(GZIP_MTIME_UNAVAILABLE, out_next);
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
	put_unaligned_u32_le(crc32_gzip(in, in_size), out_next);
	out_next += 4;

	/* ISIZE */
	put_unaligned_u32_le((u32)in_size, out_next);
	out_next += 4;

	return out_next - (u8 *)out;
}
