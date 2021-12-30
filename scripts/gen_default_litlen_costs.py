#!/usr/bin/env python3
#
# This script computes the array 'used_lits_to_default_litlen_cost'.

from math import log2

BIT_COST = 16 # Must match BIT_COST in deflate_compress.c
NUM_LEN_SLOTS = 29

print('{', end='')
for num_used_literals in range(0, 257):
    if num_used_literals == 0:
        num_used_literals = 1
    prob = 1 / (num_used_literals + NUM_LEN_SLOTS)
    cost = int(-log2(prob) * BIT_COST)
    print(f'{cost}, ', end='')
print('}')
