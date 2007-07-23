/* -*-  indent-tabs-mode:nil; c-basic-offset:4;  -*- */
/*
 * Copyright (C) 2007 Marco Gerards <marco@gnu.org>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "dirac_arith.h"

static unsigned int arith_lookup[256] = {
    0,    2,    5,    8,    11,   15,   20,   24,
    29,   35,   41,   47,   53,   60,   67,   74,
    82,   89,   97,   106,  114,  123,  132,  141,
    150,  160,  170,  180,  190,  201,  211,  222,
    233,  244,  256,  267,  279,  291,  303,  315,
    327,  340,  353,  366,  379,  392,  405,  419,
    433,  447,  461,  475,  489,  504,  518,  533,
    548,  563,  578,  593,  609,  624,  640,  656,
    672,  688,  705,  721,  738,  754,  771,  788,
    805,  822,  840,  857,  875,  892,  910,  928,
    946,  964,  983,  1001, 1020, 1038, 1057, 1076,
    1095, 1114, 1133, 1153, 1172, 1192, 1211, 1231,
    1251, 1271, 1291, 1311, 1332, 1352, 1373, 1393,
    1414, 1435, 1456, 1477, 1498, 1520, 1541, 1562,
    1584, 1606, 1628, 1649, 1671, 1694, 1716, 1738,
    1760, 1783, 1806, 1828, 1851, 1874, 1897, 1920,
    1935, 1942, 1949, 1955, 1961, 1968, 1974, 1980,
    1985, 1991, 1996, 2001, 2006, 2011, 2016, 2021,
    2025, 2029, 2033, 2037, 2040, 2044, 2047, 2050,
    2053, 2056, 2058, 2061, 2063, 2065, 2066, 2068,
    2069, 2070, 2071, 2072, 2072, 2072, 2072, 2072,
    2072, 2071, 2070, 2069, 2068, 2066, 2065, 2063,
    2060, 2058, 2055, 2052, 2049, 2045, 2042, 2038,
    2033, 2029, 2024, 2019, 2013, 2008, 2002, 1996,
    1989, 1982, 1975, 1968, 1960, 1952, 1943, 1934,
    1925, 1916, 1906, 1896, 1885, 1874, 1863, 1851,
    1839, 1827, 1814, 1800, 1786, 1772, 1757, 1742,
    1727, 1710, 1694, 1676, 1659, 1640, 1622, 1602,
    1582, 1561, 1540, 1518, 1495, 1471, 1447, 1422,
    1396, 1369, 1341, 1312, 1282, 1251, 1219, 1186,
    1151, 1114, 1077, 1037, 995,  952,  906,  857,
    805, 750,   690,  625,  553,  471,  376,  255
};

/**
 * Initialize arithmetic decoding
 *
 * @param arith state used for further calls to the arithmetic decoder
 * @param gb GetBitContext to read from
 * @param length amount of bytes to decode
 */
void dirac_arith_init (dirac_arith_state_t arith,
                       GetBitContext *gb, int length) {
    int i;

    align_get_bits(gb);
    arith->bits_left = 8 * length - 16;
    arith->low = 0;
    arith->range = 0x10000;
    arith->code = get_bits_long(gb, 16);
    arith->gb = gb;

    /* Initialize contexts.  */
    for (i = 0; i < ARITH_CONTEXT_COUNT; i++) {
        arith->contexts[i] = 0x8000;
    }
}

/**
 * Read a single bit using the arithmetic decoder
 *
 * @param arith state of arithmetic decoder
 * @param context the context of the bit to read
 * @return the bit read
 */
int dirac_arith_get_bit (dirac_arith_state_t arith, int context) {
    GetBitContext *gb = arith->gb;
    unsigned int prob_zero = arith->contexts[context];
    unsigned int count;
    unsigned int range_times_prob;
    unsigned int ret;

    count = arith->code - arith->low;
    range_times_prob = (arith->range * prob_zero) >> 16;
    if (count >= range_times_prob) {
        ret = 1;
        arith->low += range_times_prob;
        arith->range -= range_times_prob;
    } else {
        ret = 0;
        arith->range = range_times_prob;
    }

    /* Update contexts. */
    if (ret)
        arith->contexts[context] -= arith_lookup[arith->contexts[context] >> 8];
    else
        arith->contexts[context] += arith_lookup[255 - (arith->contexts[context] >> 8)];

    while (arith->range <= 0x4000) {
        if (((arith->low + arith->range - 1)^arith->low) >= 0x8000) {
            arith->code ^= 0x4000;
            arith->low ^= 0x4000;
        }
        arith->low <<= 1;
        arith->range <<= 1;
        arith->low &= 0xFFFF;
        arith->code <<= 1;
        if (arith->bits_left > 0) {
            arith->code |= get_bits (gb, 1);
            arith->bits_left--;
        }
        else {
            /* Get default: */
            arith->code |= 1;
        }
        arith->code &= 0xffff;
    }

    return ret;
}

static unsigned inline int follow_context (int index,
                                           struct dirac_arith_context_set *context_set) {
    int pos;
    pos = FFMIN(index, context_set->follow_length - 1);
    return context_set->follow[pos];
}

/* Read an unsigned int using the arithmetic decoder
 * @param arith state of arithmetic decoder
 * @param context_set the collection of contexts to read the unsigned int
 * @return value read by arithmetic decoder
 */
unsigned int dirac_arith_read_uint (dirac_arith_state_t arith,
                                    struct dirac_arith_context_set *context_set) {
    int ret = 1;
    int index = 0;

    while (dirac_arith_get_bit (arith, follow_context(index, context_set)) == 0) {
        ret <<= 1;
        if (dirac_arith_get_bit (arith, context_set->data))
            ret++;
        index++;
    }
    ret--;
    return ret;
}

/* Read a signed int using the arithmetic decoder
 * @param arith state of arithmetic decoder
 * @param context_set the collection of contexts to read the signed int
 * @return value read by arithmetic decoder
 */
int dirac_arith_read_int (dirac_arith_state_t arith,
                          struct dirac_arith_context_set *context_set) {
    int ret = dirac_arith_read_uint (arith, context_set);
    if (ret != 0 && dirac_arith_get_bit(arith, context_set->sign))
        ret = -ret;
    return ret;
}

/* Flush the arithmetic decoder, consume all bytes up to the
 * initialized length.
 */
void dirac_arith_flush(dirac_arith_state_t arith) {
    skip_bits_long(arith->gb, arith->bits_left);
    arith->bits_left = 0;
}
