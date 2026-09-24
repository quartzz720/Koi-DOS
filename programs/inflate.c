#include "inflate.h"

/* DEFLATE, from RFC 1951.
 *
 * ---- What the format is, in one paragraph --------------------------------
 *
 * A stream of blocks. Each block is either stored (a length and that many
 * bytes), or compressed with one of two Huffman code sets: a fixed pair the
 * standard writes out, or a pair described at the start of the block. The
 * symbols are either a literal byte, or "go back D bytes and copy L of them" -
 * and that back-reference may overlap what it is producing, which is how a run
 * of a thousand identical pixels costs four bytes.
 *
 * ---- The two things that make this small ---------------------------------
 *
 * Canonical Huffman: a code is fully described by the *lengths* of its
 * symbols, because the codes themselves follow from the lengths by a rule. So
 * a table here is an array of lengths and two small arrays built from them,
 * and decoding walks bit by bit rather than looking up a wide index. That is
 * slower than a table-driven decoder and about a fifth of the code; a picture
 * decodes in milliseconds either way on this machine.
 *
 * And bits come least-significant first within a byte, which is the one
 * detail everybody gets wrong the first time. The Huffman codes themselves are
 * packed most-significant first, which reads as a contradiction and is not:
 * the code is accumulated in the opposite order from the length fields.
 */

#define MAX_SYMBOLS 288
#define MAX_BITS 15

typedef struct {
    const koi_uint8* data;
    long length;
    long at;                    /* byte position */
    int bit;                    /* bit position within that byte */
} BITS;

typedef struct {
    /* How many codes of each length, and the symbols in canonical order. */
    unsigned short count[MAX_BITS + 1];
    unsigned short symbol[MAX_SYMBOLS];
} HUFFMAN;

static char trouble[96];

const char* inflate_trouble(void) {
    return trouble[0] ? trouble : "nothing went wrong";
}

static void blame(const char* what) {
    int at = 0;

    while (what[at] && at + 1 < (int)sizeof(trouble)) {
        trouble[at] = what[at];
        at++;
    }
    trouble[at] = 0;
}

static int next_bit(BITS* bits) {
    int value;

    if (bits->at >= bits->length) return -1;
    value = (bits->data[bits->at] >> bits->bit) & 1;
    bits->bit++;
    if (bits->bit == 8) { bits->bit = 0; bits->at++; }
    return value;
}

static long next_bits(BITS* bits, int count) {
    long value = 0;

    for (int index = 0; index < count; index++) {
        int one = next_bit(bits);

        if (one < 0) return -1;
        value |= (long)one << index;
    }
    return value;
}

/* Build the two arrays a canonical code needs from a list of lengths. */
static int build(HUFFMAN* into, const unsigned char* lengths, int count) {
    unsigned short offsets[MAX_BITS + 1];
    int left;

    for (int length = 0; length <= MAX_BITS; length++) into->count[length] = 0;
    for (int symbol = 0; symbol < count; symbol++)
        into->count[lengths[symbol]]++;
    into->count[0] = 0;

    /* A code is over-subscribed when the lengths ask for more codes than a
       tree of that depth has room for, and incomplete when they ask for
       fewer. One symbol on its own is the legal exception - a distance code
       that is never used looks exactly like that. */
    left = 1;
    for (int length = 1; length <= MAX_BITS; length++) {
        left <<= 1;
        left -= into->count[length];
        if (left < 0) { blame("the compressed data has an impossible code"); return 0; }
    }

    offsets[1] = 0;
    for (int length = 1; length < MAX_BITS; length++)
        offsets[length + 1] = (unsigned short)(offsets[length] +
                                               into->count[length]);
    for (int symbol = 0; symbol < count; symbol++)
        if (lengths[symbol])
            into->symbol[offsets[lengths[symbol]]++] = (unsigned short)symbol;
    return 1;
}

/* One symbol, walked bit by bit. `code` accumulates most-significant first;
   `first` is the smallest code of the current length and `index` where that
   length's symbols begin. */
static int decode(BITS* bits, const HUFFMAN* table) {
    int code = 0;
    int first = 0;
    int index = 0;

    for (int length = 1; length <= MAX_BITS; length++) {
        int one = next_bit(bits);
        int count;

        if (one < 0) return -1;
        code |= one;
        count = table->count[length];
        if (code - first < count) return table->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

/* The extra bits and bases for lengths and distances, straight out of the
   standard's two tables. */
static const unsigned short length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
    5, 5, 5, 5, 0
};
static const unsigned short distance_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const unsigned char distance_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
    11, 11, 12, 12, 13, 13
};

static long block(BITS* bits, const HUFFMAN* literals, const HUFFMAN* distances,
                  koi_uint8* out, long out_size, long written) {
    for (;;) {
        int symbol = decode(bits, literals);

        if (symbol < 0) { blame("the compressed data ended in the middle"); return -1; }
        if (symbol < 256) {
            if (written >= out_size) { blame("the picture is larger than the room for it"); return -1; }
            out[written++] = (koi_uint8)symbol;
            continue;
        }
        if (symbol == 256) return written;      /* end of block */

        symbol -= 257;
        if (symbol >= 29) { blame("the compressed data has an impossible length"); return -1; }
        {
            long length = length_base[symbol];
            long extra = next_bits(bits, length_extra[symbol]);
            int which;
            long distance;

            if (extra < 0) { blame("the compressed data ended in the middle"); return -1; }
            length += extra;

            which = decode(bits, distances);
            if (which < 0 || which >= 30) { blame("the compressed data has an impossible distance"); return -1; }
            distance = distance_base[which];
            extra = next_bits(bits, distance_extra[which]);
            if (extra < 0) { blame("the compressed data ended in the middle"); return -1; }
            distance += extra;

            if (distance > written) { blame("the compressed data points before the start"); return -1; }
            if (written + length > out_size) { blame("the picture is larger than the room for it"); return -1; }
            /* Byte by byte on purpose: the copy is allowed to overlap what it
               is writing, and that overlap is the whole trick - a run of one
               repeated byte is a distance of one and a length of many. */
            for (long index = 0; index < length; index++) {
                out[written] = out[written - distance];
                written++;
            }
        }
    }
}

static const HUFFMAN* fixed_literals(void) {
    static HUFFMAN table;
    static int built;

    if (!built) {
        unsigned char lengths[288];

        for (int at = 0; at < 144; at++) lengths[at] = 8;
        for (int at = 144; at < 256; at++) lengths[at] = 9;
        for (int at = 256; at < 280; at++) lengths[at] = 7;
        for (int at = 280; at < 288; at++) lengths[at] = 8;
        build(&table, lengths, 288);
        built = 1;
    }
    return &table;
}

static const HUFFMAN* fixed_distances(void) {
    static HUFFMAN table;
    static int built;

    if (!built) {
        unsigned char lengths[30];

        for (int at = 0; at < 30; at++) lengths[at] = 5;
        build(&table, lengths, 30);
        built = 1;
    }
    return &table;
}

/* The code lengths of a dynamic block, which are themselves Huffman coded -
   the part of DEFLATE that reads as a joke and is simply one more level of
   the same idea. */
static int read_tables(BITS* bits, HUFFMAN* literals, HUFFMAN* distances) {
    static const unsigned char order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    unsigned char lengths[MAX_SYMBOLS + 30];
    HUFFMAN code_lengths;
    long literal_count = next_bits(bits, 5);
    long distance_count = next_bits(bits, 5);
    long code_count = next_bits(bits, 4);
    long total;
    long at = 0;

    if (literal_count < 0 || distance_count < 0 || code_count < 0) return 0;
    literal_count += 257;
    distance_count += 1;
    code_count += 4;
    total = literal_count + distance_count;

    {
        unsigned char code_bits[19];

        for (int index = 0; index < 19; index++) code_bits[index] = 0;
        for (int index = 0; index < code_count; index++) {
            long value = next_bits(bits, 3);

            if (value < 0) return 0;
            code_bits[order[index]] = (unsigned char)value;
        }
        if (!build(&code_lengths, code_bits, 19)) return 0;
    }

    while (at < total) {
        int symbol = decode(bits, &code_lengths);
        long repeat;
        unsigned char value;

        if (symbol < 0) return 0;
        if (symbol < 16) { lengths[at++] = (unsigned char)symbol; continue; }

        if (symbol == 16) {
            if (!at) { blame("the compressed data repeats a length before the first one"); return 0; }
            value = lengths[at - 1];
            repeat = next_bits(bits, 2);
            if (repeat < 0) return 0;
            repeat += 3;
        } else if (symbol == 17) {
            value = 0;
            repeat = next_bits(bits, 3);
            if (repeat < 0) return 0;
            repeat += 3;
        } else {
            value = 0;
            repeat = next_bits(bits, 7);
            if (repeat < 0) return 0;
            repeat += 11;
        }
        if (at + repeat > total) { blame("the compressed data describes too many codes"); return 0; }
        while (repeat--) lengths[at++] = value;
    }

    if (!build(literals, lengths, (int)literal_count)) return 0;
    if (!build(distances, lengths + literal_count, (int)distance_count)) return 0;
    return 1;
}

long inflate_raw(const void* input, long input_length, void* out,
                 long out_size) {
    BITS bits;
    koi_uint8* output = (koi_uint8*)out;
    long written = 0;

    trouble[0] = 0;
    bits.data = (const koi_uint8*)input;
    bits.length = input_length;
    bits.at = 0;
    bits.bit = 0;

    for (;;) {
        long last = next_bits(&bits, 1);
        long kind = next_bits(&bits, 2);

        if (last < 0 || kind < 0) { blame("the compressed data ended in the middle"); return -1; }

        if (kind == 0) {
            long length;

            /* A stored block starts on a byte boundary. */
            if (bits.bit) { bits.bit = 0; bits.at++; }
            if (bits.at + 4 > bits.length) { blame("the compressed data ended in the middle"); return -1; }
            length = (long)bits.data[bits.at] | ((long)bits.data[bits.at + 1] << 8);
            /* The next two bytes are the length again, inverted. Checked,
               because it is free and it catches a stream that is not one. */
            {
                long check = (long)bits.data[bits.at + 2] |
                             ((long)bits.data[bits.at + 3] << 8);

                if ((length ^ 0xFFFF) != check) {
                    blame("a stored block's length does not match its check");
                    return -1;
                }
            }
            bits.at += 4;
            if (bits.at + length > bits.length) { blame("the compressed data ended in the middle"); return -1; }
            if (written + length > out_size) { blame("the picture is larger than the room for it"); return -1; }
            for (long index = 0; index < length; index++)
                output[written++] = bits.data[bits.at++];
        } else if (kind == 1) {
            written = block(&bits, fixed_literals(), fixed_distances(),
                            output, out_size, written);
            if (written < 0) return -1;
        } else if (kind == 2) {
            HUFFMAN literals;
            HUFFMAN distances;

            if (!read_tables(&bits, &literals, &distances)) {
                if (!trouble[0]) blame("the compressed data has an impossible table");
                return -1;
            }
            written = block(&bits, &literals, &distances, output, out_size,
                            written);
            if (written < 0) return -1;
        } else {
            blame("the compressed data has a block of no known kind");
            return -1;
        }

        if (last) break;
    }
    return written;
}

long inflate_zlib(const void* input, long input_length, void* out,
                  long out_size) {
    const koi_uint8* data = (const koi_uint8*)input;
    long written;

    trouble[0] = 0;
    if (input_length < 6) { blame("there is not enough data to be a stream"); return -1; }

    /* Two bytes: the method and window in the first, flags in the second, and
       the pair read as a big-endian number must be a multiple of 31. */
    if ((data[0] & 0x0F) != 8) { blame("the data is compressed by some other method"); return -1; }
    if ((((long)data[0] << 8) | data[1]) % 31) { blame("the header is damaged"); return -1; }
    if (data[1] & 0x20) { blame("the data needs a dictionary this cannot supply"); return -1; }

    written = inflate_raw(data + 2, input_length - 2, out, out_size);
    if (written < 0) return -1;

    /* Adler-32 over what came out, against the four bytes at the end. Cheap,
       and it is the difference between "this picture is wrong" and a picture
       that is quietly wrong. */
    {
        koi_uint8* output = (koi_uint8*)out;
        unsigned long a = 1;
        unsigned long b = 0;
        unsigned long wanted;
        long tail = input_length - 4;

        for (long at = 0; at < written; at++) {
            a = (a + output[at]) % 65521;
            b = (b + a) % 65521;
        }
        wanted = ((unsigned long)data[tail] << 24) |
                 ((unsigned long)data[tail + 1] << 16) |
                 ((unsigned long)data[tail + 2] << 8) |
                 (unsigned long)data[tail + 3];
        if (((b << 16) | a) != wanted) {
            blame("the data did not survive the journey - the checksum differs");
            return -1;
        }
    }
    return written;
}
