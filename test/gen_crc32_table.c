/*
 * gen_crc32_table.c
 *
 * A program for CRC-32 table generation.
 *
 * See crc32.c for more details about CRC computation.
 */

#include <stdint.h>
#include <stdio.h>

static uint32_t crc32_table[0x800];

static uint32_t
crc32_update_bit(uint32_t remainder, uint8_t next_bit)
{
	return (remainder >> 1) ^
		(((remainder ^ next_bit) & 1) ? 0xEDB88320 : 0);
}

static uint32_t
crc32_update_byte(uint32_t remainder, uint8_t next_byte)
{
	for (int j = 0; j < 8; j++, next_byte >>= 1)
		remainder = crc32_update_bit(remainder, next_byte & 1);
	return remainder;
}

static void
print_256_entries(const uint32_t *entries)
{
	for (size_t i = 0; i < 256 / 4; i++) {
		printf("\t");
		for (size_t j = 0; j < 4; j++) {
			printf("0x%08x,", entries[i * 4 + j]);
			if (j != 3)
				printf(" ");
		}
		printf("\n");
	}
}

int
main(void)
{
	/* crc32_table[i] for 0 <= i < 0x100 is the CRC-32 of byte i.  */
	for (int i = 0; i < 0x100; i++)
		crc32_table[i] = crc32_update_byte(0, i);

	/* crc32_table[i] for 0x100 <= i < 0x800 is the CRC-32 of byte i % 0x100
	 * followed by i / 0x100 zero bytes.  */
	for (int i = 0x100; i < 0x800; i++)
		crc32_table[i] = crc32_update_byte(crc32_table[i - 0x100], 0);

	printf("/*\n");
	printf(" * crc32_table.h\n");
	printf(" *\n");
	printf(" * Data table to accelerate CRC-32 computation.\n");
	printf(" *\n");
	printf(" * THIS FILE WAS AUTOMATICALLY GENERATED "
	       "BY gen_crc32_table.c.  DO NOT EDIT.\n");
	printf(" */\n");
	printf("\n");
	printf("#include <stdint.h>\n");
	printf("\n");
	printf("static const uint32_t crc32_table[] = {\n");
	print_256_entries(&crc32_table[0x000]);
	printf("#if defined(CRC32_SLICE4) || defined(CRC32_SLICE8)\n");
	print_256_entries(&crc32_table[0x100]);
	print_256_entries(&crc32_table[0x200]);
	print_256_entries(&crc32_table[0x300]);
	printf("#endif /* CRC32_SLICE4 || CRC32_SLICE8 */\n");
	printf("#if defined(CRC32_SLICE8)\n");
	print_256_entries(&crc32_table[0x400]);
	print_256_entries(&crc32_table[0x500]);
	print_256_entries(&crc32_table[0x600]);
	print_256_entries(&crc32_table[0x700]);
	printf("#endif /* CRC32_SLICE8 */\n");
	printf("};\n");
	return 0;
}
