#!/usr/bin/env python3
#
# This script computes LITLEN_ENOUGH, OFFSET_ENOUGH, and PRECODE_ENOUGH: the
# worst-case number of entries in libdeflate's Huffman decode tables.  It gives
# the same results as the 'enough' program from zlib.
#
# Background: a decode table has two levels.  The main table has
# 2**table_bits entries, indexed by the next table_bits bits of input.  Each
# codeword of length <= table_bits fills all main table entries that begin with
# it.  A main table entry (a "slot") whose prefix starts codewords longer than
# table_bits instead points to a subtable.  build_decode_table() gives each
# such slot a subtable of 2**(L - table_bits) entries, where L is the length of
# the longest codeword in the slot.  The table size is 2**table_bits plus the
# sizes of all the subtables.
#
# Because the code is canonical, the table size depends only on how many
# codewords have each length, not on which symbols they belong to.  Canonical
# codewords are assigned in order of increasing length, each one directly after
# the previous one.  So the codespace fills up from left to right, and since
# slots are contiguous ranges of the codespace, they also fill up from left to
# right.  This means a slot's longest codeword is the last one placed in it.
#
# So the worst case can be found by building the code one length at a time,
# from shortest to longest, choosing how many codewords to give each length,
# and adding a slot's subtable size when its last codeword is placed.  This is
# a dynamic programming search: the state is (symbols left to place, free codes
# at the current length, current length), and the result for each state is
# memoized.
#
# Only complete codes (codes that fill the whole codespace) are considered.
# The only incomplete codes libdeflate accepts have 0 or 1 codewords.  For
# those it fills only the main table, which is never larger than the result.

import argparse
from functools import cache


def max_decode_table_size(max_syms, table_bits, max_codeword_len):
    """Return the largest decode table size possible for any complete code
    that uses at most max_syms symbols and has codewords no longer than
    max_codeword_len bits, given a main table with 2**table_bits entries."""

    # Return the largest possible total size of the subtables still to be
    # finished, given that:
    #
    #   - `syms` symbols still need codewords, all of length >= `length`
    #     (where table_bits < length <= max_codeword_len);
    #   - `left` codes of length `length` are still free.  These are always
    #     the last `left` codes of that length (the right end of the
    #     codespace).
    #
    # Return -1 if the remaining symbols can't form a complete code.
    #
    # Here a slot covers `step = 2**(length - table_bits)` codes of length
    # `length`.  The free codes overlap ceil(left / step) slots: possibly a
    # partly filled slot, followed by slots that are still empty.
    @cache
    def max_subtable_entries(syms, left, length):
        step = 1 << (length - table_bits)

        if length == max_codeword_len:
            # Last length: every free code has to get a symbol, and every
            # slot that overlaps the free codes ends here.
            if syms != left:
                return -1
            return ((left + step - 1) // step) * step

        best = -1
        shift = max_codeword_len - length
        # Try every number `use` of codewords to give length `length`.  The
        # first `use` free codes go to those codewords, and the other
        # `new_left` codes each split into two codes of length `length + 1`.
        #
        # Lower bound: no code may be left without a symbol, so each code that
        # isn't used here needs at least 2 symbols below it:
        #     syms - use >= 2 * (left - use)  =>  use >= 2*left - syms
        min_use = max(0, 2 * left - syms)
        for use in range(min_use, min(syms, left) + 1):
            new_left = left - use
            # Upper bound: each code that isn't used here can hold at most
            # 2**shift symbols, by splitting it all the way down to
            # max_codeword_len.  Increasing `use` only makes this worse, so
            # stop as soon as it fails.
            if syms - use > (new_left << shift):
                break
            # Slots whose last code was just used end at this length, so each
            # needs a subtable of `step` entries.
            closed = (left + step - 1) // step - (new_left + step - 1) // step
            if syms - use == 0:
                # All symbols placed.  The lower bound makes sure this only
                # happens when new_left == 0, i.e. the code is complete.
                best = max(best, closed * step)
            else:
                res = max_subtable_entries(syms - use, new_left * 2, length + 1)
                if res >= 0:
                    best = max(best, closed * step + res)
        return best

    root_size = 1 << table_bits
    best = root_size  # Size with no subtables.
    if table_bits < max_codeword_len:
        # Codewords of length <= table_bits don't affect any subtable sizes,
        # so the only thing that matters is how many slots they leave free.
        # Try every number `r` of slots left for longer codewords.  Each of
        # these slots needs at least 2 symbols, so 2*r can't exceed max_syms.
        for r in range(1, min(root_size, max_syms // 2) + 1):
            used_slots = root_size - r
            # Each codeword of length <= table_bits fills a power-of-2 number
            # of slots, so filling `used_slots` slots needs at least
            # popcount(used_slots) symbols.  Symbols don't all need to be used,
            # so the subtables can get any number of symbols from 2*r up to
            # what's left.
            min_root_syms = bin(used_slots).count("1")
            for sub_syms in range(2 * r, max_syms - min_root_syms + 1):
                # The r free slots are 2*r free codes of length table_bits + 1.
                sub = max_subtable_entries(sub_syms, 2 * r, table_bits + 1)
                if sub >= 0:
                    best = max(best, root_size + sub)
    return best


def main():
    parser = argparse.ArgumentParser(
        description="Compute the worst-case number of entries in a Huffman "
        "decode table."
    )
    parser.add_argument(
        "max_syms", type=int, help="maximum number of symbols in the code"
    )
    parser.add_argument("table_bits", type=int, help="number of main table bits")
    parser.add_argument(
        "max_codeword_len", type=int, help="maximum allowed codeword length"
    )
    args = parser.parse_args()

    if args.max_syms < 2:
        parser.error("max_syms must be >= 2")
    if args.table_bits < 1 or args.max_codeword_len < 1:
        parser.error("table_bits and max_codeword_len must be >= 1")

    print(max_decode_table_size(args.max_syms, args.table_bits, args.max_codeword_len))


if __name__ == "__main__":
    main()
