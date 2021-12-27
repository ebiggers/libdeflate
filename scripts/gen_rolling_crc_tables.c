#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t crc32_table[0x100];

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
main(int argc, char *argv[])
{
	int rolling_window_size = 0;

	if (argc == 2)
		rolling_window_size = atoi(argv[1]);

	if (rolling_window_size < 1) {
		fprintf(stderr, "Usage: %s ROLLING_WINDOW_SIZE\n", argv[0]);
		return 2;
	}

	for (int i = 0; i < 0x100; i++)
		crc32_table[i] = crc32_update_byte(0, i);
	printf("#include <stdint.h>\n");
	printf("\n");
	printf("#define ROLLING_WINDOW_SIZE %d\n", rolling_window_size);
	printf("\n");
	printf("static const uint32_t crc32_roll_tab[] = {\n");
	print_256_entries(&crc32_table[0]);
	printf("};\n");
	printf("\n");
	for (int i = 0; i < 0x100; i++) {
		for (int j = 0; j < rolling_window_size; j++)
			crc32_table[i] = crc32_update_byte(crc32_table[i], 0);
	}
	printf("static const uint32_t crc32_unroll_tab[] = {\n");
	print_256_entries(&crc32_table[0]);
	printf("};\n");
	return 0;
}
