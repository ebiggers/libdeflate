/*
 * gzip_decompress.c - decompress with a gzip wrapper
 *
 * Written in 2014-2016 by Eric Biggers <ebiggers3@gmail.com>
 *
 * To the extent possible under law, the author(s) have dedicated all copyright
 * and related and neighboring rights to this software to the public domain
 * worldwide. This software is distributed without any warranty.
 *
 * You should have received a copy of the CC0 Public Domain Dedication along
 * with this software. If not, see
 * <http://creativecommons.org/publicdomain/zero/1.0/>.
 */

#include "crc32.h"
#include "gzip_constants.h"
#include "unaligned.h"

#include "libdeflate.h"

LIBEXPORT enum libdeflate_result
libdeflate_gzip_decompress(struct libdeflate_decompressor *d,
			   const void *in, size_t in_nbytes,
			   void *out, size_t out_nbytes_avail,
			   size_t *actual_out_nbytes_ret)
{
	const u8 *in_next = in;
	const u8 * const in_end = in_next + in_nbytes;
	u8 flg;
	size_t actual_out_nbytes;
	enum libdeflate_result result;

	if (in_nbytes < GZIP_MIN_OVERHEAD)
		return LIBDEFLATE_BAD_DATA;

	/* ID1 */
	if (*in_next++ != GZIP_ID1)
		return LIBDEFLATE_BAD_DATA;
	/* ID2 */
	if (*in_next++ != GZIP_ID2)
		return LIBDEFLATE_BAD_DATA;
	/* CM */
	if (*in_next++ != GZIP_CM_DEFLATE)
		return LIBDEFLATE_BAD_DATA;
	flg = *in_next++;
	/* MTIME */
	in_next += 4;
	/* XFL */
	in_next += 1;
	/* OS */
	in_next += 1;

	if (flg & GZIP_FRESERVED)
		return LIBDEFLATE_BAD_DATA;

	/* Extra field */
	if (flg & GZIP_FEXTRA) {
		u16 xlen = get_unaligned_le16(in_next);
		in_next += 2;

		if (in_end - in_next < (u32)xlen + GZIP_FOOTER_SIZE)
			return LIBDEFLATE_BAD_DATA;

		in_next += xlen;
	}

	/* Original file name (zero terminated) */
	if (flg & GZIP_FNAME) {
		while (*in_next++ != 0 && in_next != in_end)
			;
		if (in_end - in_next < GZIP_FOOTER_SIZE)
			return LIBDEFLATE_BAD_DATA;
	}

	/* File comment (zero terminated) */
	if (flg & GZIP_FCOMMENT) {
		while (*in_next++ != 0 && in_next != in_end)
			;
		if (in_end - in_next < GZIP_FOOTER_SIZE)
			return LIBDEFLATE_BAD_DATA;
	}

	/* CRC16 for gzip header */
	if (flg & GZIP_FHCRC) {
		in_next += 2;
		if (in_end - in_next < GZIP_FOOTER_SIZE)
			return LIBDEFLATE_BAD_DATA;
	}

	/* Compressed data  */
	result = libdeflate_deflate_decompress(d, in_next,
					in_end - GZIP_FOOTER_SIZE - in_next,
					out, out_nbytes_avail,
					actual_out_nbytes_ret);
	if (result != LIBDEFLATE_SUCCESS)
		return result;

	if (actual_out_nbytes_ret)
		actual_out_nbytes = *actual_out_nbytes_ret;
	else
		actual_out_nbytes = out_nbytes_avail;

	in_next = in_end - GZIP_FOOTER_SIZE;

	/* CRC32 */
	if (crc32_gzip(out, actual_out_nbytes) != get_unaligned_le32(in_next))
		return LIBDEFLATE_BAD_DATA;
	in_next += 4;

	/* ISIZE */
	if ((u32)actual_out_nbytes != get_unaligned_le32(in_next))
		return LIBDEFLATE_BAD_DATA;

	return LIBDEFLATE_SUCCESS;
}
