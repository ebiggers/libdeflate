#!/usr/bin/env python3
#
# This script generates the deflate_offset_slot[] array, which is a condensed
# map from offsets to offset slots.

DEFLATE_OFFSET_SLOT_BASE = [
	1    , 2    , 3    , 4     , 5     , 7     , 9     , 13    ,
	17   , 25   , 33   , 49    , 65    , 97    , 129   , 193   ,
	257  , 385  , 513  , 769   , 1025  , 1537  , 2049  , 3073  ,
	4097 , 6145 , 8193 , 12289 , 16385 , 24577 ,
]

DEFLATE_EXTRA_OFFSET_BITS = [
	0    , 0    , 0    , 0     , 1     , 1     , 2     , 2     ,
	3    , 3    , 4    , 4     , 5     , 5     , 6     , 6     ,
	7    , 7    , 8    , 8     , 9     , 9     , 10    , 10    ,
	11   , 11   , 12   , 12    , 13    , 13    ,
]

offset_slot_map = [0] * 512

for offset_slot, offset_base in enumerate(DEFLATE_OFFSET_SLOT_BASE):
    num_extra_bits = DEFLATE_EXTRA_OFFSET_BITS[offset_slot]
    offset_end = offset_base + (1 << num_extra_bits)
    if offset_base <= 256:
        for offset in range(offset_base, offset_end):
            offset_slot_map[offset] = offset_slot
    else:
        for offset in range(offset_base, offset_end, 128):
            offset_slot_map[256 + ((offset - 1) >> 7)] = offset_slot

print('static const u8 deflate_offset_slot_map[512] = {')
for i in range(0, len(offset_slot_map), 16):
    print('\t', end='')
    for j, v in enumerate(offset_slot_map[i:i+16]):
        print(f'{v},', end='')
        if j == 15:
            print('')
        else:
            print(' ', end='')
print('};')
