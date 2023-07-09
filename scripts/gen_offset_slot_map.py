#!/usr/bin/env python3
#
# This script generates the deflate_offset_slot[] array, which maps
# 'offset - 1 => offset_slot' for offset <= 256.

DEFLATE_OFFSET_SLOT_BASE = [
	1    , 2    , 3    , 4     , 5     , 7     , 9     , 13    ,
	17   , 25   , 33   , 49    , 65    , 97    , 129   , 193   ,
	257  , 385  , 513  , 769   , 1025  , 1537  , 2049  , 3073  ,
	4097 , 6145 , 8193 , 12289 , 16385 , 24577 ,
]

offset_slot_map = [0] * 256
offset_slot = -1
for offset in range(1, len(offset_slot_map) + 1):
    if offset >= DEFLATE_OFFSET_SLOT_BASE[offset_slot + 1]:
        offset_slot += 1
    offset_slot_map[offset - 1] = offset_slot

print(f'static const u8 deflate_offset_slot[{len(offset_slot_map)}] = {{')
for i in range(0, len(offset_slot_map), 16):
    print('\t', end='')
    for j, v in enumerate(offset_slot_map[i:i+16]):
        print(f'{v},', end='')
        if j == 15:
            print('')
        else:
            print(' ', end='')
print('};')
