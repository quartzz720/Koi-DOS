#include "wav.h"

static unsigned int read32(const unsigned char* at) {
    return (unsigned int)at[0] | ((unsigned int)at[1] << 8) |
           ((unsigned int)at[2] << 16) | ((unsigned int)at[3] << 24);
}

static unsigned short read16(const unsigned char* at) {
    return (unsigned short)((unsigned int)at[0] | ((unsigned int)at[1] << 8));
}

static int tag_is(const unsigned char* at, const char* name) {
    return at[0] == (unsigned char)name[0] && at[1] == (unsigned char)name[1] &&
           at[2] == (unsigned char)name[2] && at[3] == (unsigned char)name[3];
}

/* Walk the chunks, filling in the format and finding the samples. Returns the
   number of bytes of sample data, or 0. */
static unsigned int walk(const unsigned char* file, unsigned int size,
                         WAV_FORMAT* format, unsigned int* data_at) {
    unsigned int at = 12;          /* past "RIFF", the size, and "WAVE" */
    int have_format = 0;
    unsigned int data_size = 0;

    if (size < 12 || !tag_is(file, "RIFF") || !tag_is(file + 8, "WAVE"))
        return 0;

    while (at + 8 <= size) {
        unsigned int length = read32(file + at + 4);
        const unsigned char* body = file + at + 8;

        if (length > size - at - 8) length = size - at - 8;

        if (tag_is(file + at, "fmt ") && length >= 16) {
            format->format = read16(body);
            format->channels = read16(body + 2);
            format->rate = read32(body + 4);
            format->bytes_per_second = read32(body + 8);
            format->block_align = read16(body + 12);
            format->bits = read16(body + 14);
            have_format = 1;
        } else if (tag_is(file + at, "data")) {
            *data_at = at + 8;
            data_size = length;
            /* Not stopping here: a file may carry chunks after the samples,
               and one of them may be the `fmt ` we still need. */
        }

        at += 8 + length;
        if (length & 1) at++;      /* the pad byte, which is not in the length */
    }

    return have_format ? data_size : 0;
}


unsigned int wav_parse(const unsigned char* file, unsigned int size,
                       WAV_FORMAT* format, unsigned int* data_at,
                       const char** why) {
    unsigned int data_size = walk(file, size, format, data_at);

    *why = 0;
    if (!data_size) { *why = "not a WAVE file this can read"; return 0; }
    if (format->format != WAV_FORMAT_PCM) {
        *why = "compressed; only uncompressed PCM is understood";
        return 0;
    }
    if (format->bits != 8 && format->bits != 16) {
        *why = "not 8 or 16 bits per sample";
        return 0;
    }
    if (format->channels != 1 && format->channels != 2) {
        *why = "neither mono nor stereo";
        return 0;
    }
    return data_size;
}
