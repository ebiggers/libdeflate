#!/usr/bin/env python3
#
# This script computes the default litlen symbol costs for the near-optimal
# parser.

from math import log2

BIT_COST = 16 # Must match BIT_COST in deflate_compress.c
NUM_LEN_SLOTS = 29

print("""static const struct {
	u8 used_lits_to_lit_cost[257];
	u8 len_sym_cost;
} default_litlen_costs[] = {""")
MATCH_PROBS = [0.25, 0.50, 0.75]
for i, match_prob in enumerate(MATCH_PROBS):
    len_prob = match_prob / NUM_LEN_SLOTS
    len_sym_cost = int(-log2(len_prob) * BIT_COST)
    if i == 0:
        print('\t{', end='')
    print(f' /* match_prob = {match_prob} */')
    print('\t\t.used_lits_to_lit_cost = {')

    j = 0
    for num_used_literals in range(0, 257):
        if num_used_literals == 0:
            num_used_literals = 1
        lit_prob = (1 - match_prob) / num_used_literals
        lit_cost = int(-log2(lit_prob) * BIT_COST)
        if j == 0:
            print('\t\t\t', end='')
        if j == 7 or num_used_literals == 256:
            print(f'{lit_cost},')
            j = 0
        else:
            print(f'{lit_cost}, ', end='')
            j += 1
    print('\t\t},')
    print(f'\t\t.len_sym_cost = {len_sym_cost},')
    if i < len(MATCH_PROBS) - 1:
        print('\t}, {', end='')
    else:
        print('\t},')
print('};')
