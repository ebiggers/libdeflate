#ifdef __aarch64__

#define ENABLE_PMULL 1
#define ENABLE_EOR3 1

	.text
#if ENABLE_PMULL && ENABLE_EOR3
	.arch           armv8-a+crypto+crc+sha3
#elif ENABLE_PMULL
	.arch           armv8-a+crypto+crc
#else
	.arch           armv8-a+crc
#endif
	.align 4

#if ENABLE_PMULL
.Lconstants:
	.quad 0x33FFF533, 0x910EEEC1	// multipliers_8
	.quad 0x8F352D95, 0x1D9513D7	// multipliers_4
	.quad 0xAE689191, 0xCCAA009E	// multipliers_1
	.quad 0xFFFFFFFF, 0		// mask32
	.quad 0xB8BC6765, 0		// final_multiplier
	.quad 0x00000001F7011641, 0	// barrett_reduction_constant_1
	.quad 0x00000001DB710641, 0	// barrett_reduction_constant_2
#endif

	CRC		.req	w0
	DATA		.req	x1
	SIZE		.req	x2
	TMP1		.req	x3
	TMP2		.req	x4
	WTMP2		.req	w4
	TMP3		.req	x5
	TMP4		.req	x6
	TMP5		.req	x7
	CONSTANTS	.req	x12

	// u32 crc32_arm(u32 crc, const u8 *data, size_t size);
#ifdef __APPLE__
.globl _crc32_arm
_crc32_arm:
#else
.globl crc32_arm
crc32_arm:
#endif
#if ENABLE_PMULL
	// Check for short lengths.
	cmp	SIZE, #(256+15)
	b.lt	.Lscalar
	// Handle any part before the first 16-byte boundary.
	ands	TMP1, DATA, #15
	b.eq	.Lsetup_pmull
	mov	TMP2, #16
	sub	TMP1, TMP2, TMP1
	sub	SIZE, SIZE, TMP1
	tbz	TMP1, #0, 2f
	ldrb	WTMP2, [DATA], #1
	crc32b	CRC, CRC, WTMP2
2:	tbz	TMP1, #1, 4f
	ldrh	WTMP2, [DATA], #2
	crc32h	CRC, CRC, WTMP2
4:	tbz	TMP1, #2, 8f
	ldr	WTMP2, [DATA], #4
	crc32w	CRC, CRC, WTMP2
8:	tbz	TMP1, #3, .Lsetup_pmull
	ldr	TMP2, [DATA], #8
	crc32x	CRC, CRC, TMP2

.Lsetup_pmull:  // DATA % 16 == 0 && size >= 256
	// Save the callee-save vector registers (required by AArch64 ABI).
	stp	d8, d9, [sp, #-16]
	stp	d10, d11, [sp, #-32]
	stp	d12, d13, [sp, #-48]
	stp	d14, d15, [sp, #-64]

	// Load the first 128 data bytes and the multipliers_8 constants.  Also
	// XOR the initial CRC with the first 4 data bytes.
	ld1	{v0.16b-v3.16b}, [DATA], #64
	mov	WTMP2, v0.s[0]
	sub	SIZE, SIZE, #2*128
	eor	WTMP2, WTMP2, CRC
	adr	CONSTANTS, .Lconstants
	mov	v0.s[0], WTMP2
	ldr	q31, [CONSTANTS], #16
	ld1	{v4.16b-v7.16b}, [DATA], #64
.Lpmull_loop:
	// Fold 8 vectors (v0-v7, 128 bytes) into the next 8 vectors (128 bytes)
	// by folding across 8 vectors at a time.
	pmull	v8.1q, v0.1d, v31.1d
	pmull2	v0.1q, v0.2d, v31.2d
	ld1	{v16.16b-v19.16b}, [DATA], #64
	pmull	v9.1q, v1.1d, v31.1d
	pmull2	v1.1q, v1.2d, v31.2d
	pmull	v10.1q, v2.1d, v31.1d
	pmull2	v2.1q,  v2.2d, v31.2d
	pmull	v11.1q, v3.1d, v31.1d
	pmull2	v3.1q,  v3.2d, v31.2d
	pmull	v12.1q, v4.1d, v31.1d
	ld1	{v20.16b-v23.16b}, [DATA], #64
	pmull2	v4.1q,  v4.2d, v31.2d
#if ENABLE_EOR3
	eor3	v0.16b, v0.16b, v8.16b, v16.16b
	eor3	v1.16b, v1.16b, v9.16b, v17.16b
#endif
	pmull	v13.1q, v5.1d, v31.1d
	pmull2	v5.1q,  v5.2d, v31.2d
	pmull	v14.1q, v6.1d, v31.1d
	pmull2	v6.1q,  v6.2d, v31.2d
	pmull	v15.1q, v7.1d, v31.1d
	pmull2	v7.1q,  v7.2d, v31.2d
	subs	SIZE, SIZE, #128
#if ENABLE_EOR3
	eor3	v2.16b, v2.16b, v10.16b, v18.16b
	eor3	v3.16b, v3.16b, v11.16b, v19.16b
	eor3	v4.16b, v4.16b, v12.16b, v20.16b
	eor3	v5.16b, v5.16b, v13.16b, v21.16b
	eor3	v6.16b, v6.16b, v14.16b, v22.16b
	eor3	v7.16b, v7.16b, v15.16b, v23.16b
#else
	eor	v0.16b, v0.16b, v8.16b
	eor	v1.16b, v1.16b, v9.16b
	eor	v2.16b, v2.16b, v10.16b
	eor	v3.16b, v3.16b, v11.16b
	eor	v4.16b, v4.16b, v12.16b
	eor	v5.16b, v5.16b, v13.16b
	eor	v6.16b, v6.16b, v14.16b
	eor	v7.16b, v7.16b, v15.16b
	eor	v0.16b, v0.16b, v16.16b
	eor	v1.16b, v1.16b, v17.16b
	eor	v2.16b, v2.16b, v18.16b
	eor	v3.16b, v3.16b, v19.16b
	eor	v4.16b, v4.16b, v20.16b
	eor	v5.16b, v5.16b, v21.16b
	eor	v6.16b, v6.16b, v22.16b
	eor	v7.16b, v7.16b, v23.16b
#endif /* !ENABLE_EOR3 */
	b.ge	.Lpmull_loop
	add	SIZE, SIZE, #128

	// Load {multipliers_4, multipliers_1, mask32, final_multiplier}
	ld1	{v20.16b-v23.16b}, [CONSTANTS], #64
	// Fold 8 vectors (v0-v7, 128 bytes) into 4 vectors (v0-v3, 64 bytes) by
	// folding across 4 vectors at a time.
	pmull	v8.1q,  v0.1d, v20.1d
	pmull2	v0.1q,  v0.2d, v20.2d
	pmull	v9.1q,  v1.1d, v20.1d
	pmull2	v1.1q,  v1.2d, v20.2d
	pmull	v10.1q, v2.1d, v20.1d
	pmull2	v2.1q,  v2.2d, v20.2d
	pmull	v11.1q, v3.1d, v20.1d
	pmull2	v3.1q,  v3.2d, v20.2d
	eor	v0.16b, v0.16b, v4.16b
	eor	v1.16b, v1.16b, v5.16b
	eor	v2.16b, v2.16b, v6.16b
	eor	v3.16b, v3.16b, v7.16b
	eor	v0.16b, v0.16b, v8.16b
	eor	v1.16b, v1.16b, v9.16b
	eor	v2.16b, v2.16b, v10.16b
	eor	v3.16b, v3.16b, v11.16b
	// Fold 4 vectors (v0-3, 64 bytes) into 1 vector (v0, 16 bytes) by
	// folding across 1 vector at a time.
	pmull	v10.1q, v0.1d, v21.1d
	pmull2	v0.1q,  v0.2d, v21.2d
	eor	v0.16b, v0.16b, v10.16b
	eor	v0.16b, v0.16b, v1.16b
	pmull	v10.1q, v0.1d, v21.1d
	pmull2	v0.1q,  v0.2d, v21.2d
	eor	v0.16b, v0.16b, v10.16b
	eor	v0.16b, v0.16b, v2.16b
	pmull	v10.1q, v0.1d, v21.1d
	pmull2	v0.1q,  v0.2d, v21.2d
	eor	v0.16b, v0.16b, v10.16b
	eor	v0.16b, v0.16b, v3.16b

	// Do the final 128-bit => 32-bit reduction on v0.

	// Load {barrett_reduction_constant_0, barrett_reduction_constant_1}.
	ld1	{v24.16b-v25.16b}, [CONSTANTS]
	eor	v10.16b, v10.16b, v10.16b
	ext	v21.16b, v21.16b, v21.16b, #8
	// Fold 128 => 96 bits, implicitly appending 32 zeroes.
	ext	v1.16b, v0.16b, v10.16b, #8
	pmull	v0.1q, v0.1d, v21.1d
	eor	v0.16b, v0.16b, v1.16b
	// Fold 96 => 64 bits.
	ext	v1.16b, v0.16b, v10.16b, #4
	and	v0.16b, v0.16b, v22.16b
	pmull	v0.1q, v0.1d, v23.1d
	eor	v0.16b, v0.16b, v1.16b
	// Reduce 64 => 32 bits using Barrett reduction.
	and	v1.16b, v0.16b, v22.16b
	pmull	v1.1q, v1.1d, v24.1d
	and	v1.16b, v1.16b, v22.16b
	pmull	v1.1q, v1.1d, v25.1d
	eor	v0.16b, v0.16b, v1.16b
	mov	CRC, v0.s[1]

	// Restore the callee-save vector registers.
	ldp	d8, d9, [sp, #-16]
	ldp	d10, d11, [sp, #-32]
	ldp	d12, d13, [sp, #-48]
	ldp	d14, d15, [sp, #-64]
.Lscalar:
#endif /* ENABLE_PMULL */
	cmp	SIZE, #32
	b.lt	.Llessthan32
	ands	TMP1, DATA, #7
	b.eq	.Lscalar_8byte_aligned
	// Align DATA to an 8-byte boundary.
	mov	TMP2, #8
	sub	TMP1, TMP2, TMP1
	sub	SIZE, SIZE, TMP1
	tbz	TMP1, #0, 2f
	ldrb	WTMP2, [DATA], #1
	crc32b	CRC, CRC, WTMP2
2:	tbz	TMP1, #1, 4f
	ldrh	WTMP2, [DATA], #2
	crc32h	CRC, CRC, WTMP2
4:	tbz	TMP1, #2, .Lscalar_8byte_aligned
	ldr	WTMP2, [DATA], #4
	crc32w	CRC, CRC, WTMP2
.Lscalar_8byte_aligned:
	ands	TMP1, SIZE, #~31
	b.eq	.Llessthan32
.Lscalar_loop:
	ldp	TMP2, TMP3, [DATA], #16
	ldp	TMP4, TMP5, [DATA], #16
	crc32x	CRC, CRC, TMP2
	crc32x	CRC, CRC, TMP3
	crc32x	CRC, CRC, TMP4
	crc32x	CRC, CRC, TMP5
	subs	TMP1, TMP1, #32
	b.ne	.Lscalar_loop
	ands	SIZE, SIZE, #31
	b.eq	.Ldone
.Llessthan32:
	tbz	SIZE, #4, .Llessthan16
	ldp	TMP2, TMP3, [DATA], #16
	crc32x	CRC, CRC, TMP2
	crc32x	CRC, CRC, TMP3
.Llessthan16:
	tbz	SIZE, #3, .Llessthan8
	ldr	TMP2, [DATA], #8
	crc32x	CRC, CRC, TMP2
.Llessthan8:
	tbz	SIZE, #2, .Llessthan4
	ldr	WTMP2, [DATA], #4
	crc32w	CRC, CRC, WTMP2
.Llessthan4:
	tbz	SIZE, #1, .Llessthan2
	ldrh	WTMP2, [DATA], #2
	crc32h	CRC, CRC, WTMP2
.Llessthan2:
	tbz	SIZE, #0, .Ldone
	ldrb	WTMP2, [DATA], #1
	crc32b	CRC, CRC, WTMP2
.Ldone:
	ret

#endif /* __aarch64__ */
