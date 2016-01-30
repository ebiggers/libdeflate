/*
 * zlib_decompress.c - decompress with a zlib wrapper
 *
 * Author:	Eric Biggers
 * Year:	2014-2016
 *
 * The author dedicates this file to the public domain.
 * You can do whatever you want with this file.
 */

#include "adler32.h"
#include "unaligned.h"
#include "zlib_constants.h"

#include "libdeflate.h"

LIBEXPORT enum decompress_result
zlib_decompress(struct deflate_decompressor *d,
		const void *in, size_t in_nbytes,
		void *out, size_t out_nbytes_avail,
		size_t *actual_out_nbytes_ret)
{
	const u8 *in_next = in;
	const u8 * const in_end = in_next + in_nbytes;
	u16 hdr;
	size_t actual_out_nbytes;
	enum decompress_result result;

	if (in_nbytes < ZLIB_MIN_OVERHEAD)
		return DECOMPRESS_BAD_DATA;

	/* 2 byte header: CMF and FLG  */
	hdr = get_unaligned_be16(in_next);
	in_next += 2;

	/* FCHECK */
	if ((hdr % 31) != 0)
		return DECOMPRESS_BAD_DATA;

	/* CM */
	if (((hdr >> 8) & 0xF) != ZLIB_CM_DEFLATE)
		return DECOMPRESS_BAD_DATA;

	/* CINFO */
	if ((hdr >> 12) > ZLIB_CINFO_32K_WINDOW)
		return DECOMPRESS_BAD_DATA;

	/* FDICT */
	if ((hdr >> 5) & 1)
		return DECOMPRESS_BAD_DATA;

	/* Compressed data  */
	result = deflate_decompress(d, in_next,
				    in_end - ZLIB_FOOTER_SIZE - in_next,
				    out, out_nbytes_avail,
				    actual_out_nbytes_ret);
	if (result != DECOMPRESS_SUCCESS)
		return result;

	if (actual_out_nbytes_ret)
		actual_out_nbytes = *actual_out_nbytes_ret;
	else
		actual_out_nbytes = out_nbytes_avail;

	in_next = in_end - ZLIB_FOOTER_SIZE;

	/* ADLER32  */
	if (adler32(out, actual_out_nbytes) != get_unaligned_be32(in_next))
		return DECOMPRESS_BAD_DATA;

	return DECOMPRESS_SUCCESS;
}
