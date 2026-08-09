#ifndef WAV_H
#define WAV_H

#include "koi.h"

/* Reading a RIFF WAVE file.
 *
 * Shared, because there are two readers now - the console player and the one
 * in Mizu's window - and a file format parsed twice is a file format parsed
 * two different ways. The pad byte on odd-length chunks is the one everybody
 * gets wrong the first time, and getting it wrong in only one of two copies
 * is worse than getting it wrong in both.
 *
 * RIFF is a box of labelled chunks. Two matter: `fmt ` says what the samples
 * are, `data` holds them. Everything else is stepped over, which is what makes
 * the format survivable - and `data` does not have to follow `fmt `.
 */
#define WAV_FORMAT_PCM 1

typedef struct {
    unsigned short format;
    unsigned short channels;
    unsigned int rate;
    unsigned int bytes_per_second;
    unsigned short block_align;
    unsigned short bits;
} WAV_FORMAT;

/* Fills `format` and points `data_at` at the samples within `file`. Returns
   the number of bytes of samples, or 0 when this is not a WAVE this can read.
   `why` is set to something a person can act on when it returns 0. */
unsigned int wav_parse(const unsigned char* file, unsigned int size,
                       WAV_FORMAT* format, unsigned int* data_at,
                       const char** why);

#endif
