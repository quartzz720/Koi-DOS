#include "koi.h"

/* play - a WAV file, through the mixer that was already there.
 *
 * RIFF is a Microsoft format with a published specification, which is why this
 * is short and why it is written from the specification rather than from
 * anybody's decoder. That distinction is the whole reason the third-party
 * section of this project's licence manifest is empty, and it stays empty.
 *
 * A RIFF file is a box of labelled chunks. Two matter: `fmt ` says what the
 * samples are, `data` is the samples. Everything else - LIST, fact, cue points,
 * the name of the software that wrote it - is skipped by reading its length and
 * stepping over it, which is what makes the format survivable. A reader that
 * assumes `data` comes straight after `fmt ` works on the files it was tested
 * with and fails on the ones somebody else's editor saved.
 *
 * Chunks are padded to even lengths and the pad byte is not counted in the
 * length. Miss that and every file with an odd-sized chunk in it reads one byte
 * out of step from there on - which sounds exactly like a broken decoder rather
 * than like an arithmetic mistake.
 */

#define FORMAT_PCM 1

typedef struct {
    unsigned short format;
    unsigned short channels;
    unsigned int rate;
    unsigned int bytes_per_second;
    unsigned short block_align;
    unsigned short bits;
} WAVE_FORMAT;

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
static unsigned int parse(const unsigned char* file, unsigned int size,
                          WAVE_FORMAT* format, unsigned int* data_at) {
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

static void say(const char* text) { koi_print(text); koi_print("\n"); }

int main(void) {
    const char* name = koi_arguments();
    long handle;
    long size;
    unsigned char* file;
    WAVE_FORMAT format;
    unsigned int data_at = 0;
    unsigned int data_size;
    unsigned int frames;
    int voice;

    while (*name == ' ') name++;
    if (!*name) {
        say("play <file.wav>");
        say("");
        say("Plays a WAV file. Any key stops it.");
        return 1;
    }

    if (koi_sysinfo(KOI_INFO_AUDIO, 0) != 1) {
        say("There is no sound hardware this system could bring up.");
        return 1;
    }

    handle = koi_open(name, OPEN_READ);
    if (handle < 0) { koi_print("Cannot open "); say(name); return 1; }

    size = koi_filesize(handle);
    if (size <= 0) { koi_close(handle); say("Empty file."); return 1; }

    /* Read whole. The mixer does not copy the samples - it reads them where
       they are for as long as the sound plays - so this buffer has to outlive
       the call that starts it, and the simplest way to guarantee that is for
       it to outlive the whole program. */
    file = (unsigned char*)koi_alloc(size);
    if (!file) { koi_close(handle); say("Not enough memory for that file."); return 1; }

    {
        long got = 0;
        while (got < size) {
            long step = koi_read(handle, file + got, size - got);
            if (step <= 0) break;
            got += step;
        }
        koi_close(handle);
        if (got < size) size = got;
    }

    data_size = parse(file, (unsigned int)size, &format, &data_at);
    if (!data_size) { say("Not a WAV file this can read."); return 1; }

    if (format.format != FORMAT_PCM) {
        /* Said as a number, because "unsupported format" tells the person
           holding the file nothing they can act on. 0x11 is IMA ADPCM, 0xFFFE
           is the extensible header - both are things this could learn. */
        koi_printf("This WAV is format %u, and only uncompressed PCM (1) is\n",
                   format.format);
        say("understood so far.");
        return 1;
    }
    if (format.bits != 8 && format.bits != 16) {
        koi_printf("%u bits per sample; 8 and 16 are understood.\n", format.bits);
        return 1;
    }
    if (format.channels != 1 && format.channels != 2) {
        koi_printf("%u channels; mono and stereo are understood.\n",
                   format.channels);
        return 1;
    }

    frames = data_size / (unsigned int)(format.channels * (format.bits / 8));
    if (!frames) { say("The file has no samples in it."); return 1; }

    koi_printf("%s: %u Hz, %u-bit, %s, %u.%us\n", name, format.rate,
               format.bits, format.channels == 2 ? "stereo" : "mono",
               frames / format.rate, (frames * 10U / format.rate) % 10U);

    voice = koi_sound_play_simple(file + data_at, frames, format.rate,
                                  format.bits == 16 ? KOI_SOUND_S16
                                                    : KOI_SOUND_U8,
                                  format.channels, 255);
    if (voice < 0) { say("Every voice is busy."); return 1; }

    say("Playing. Any key stops it.");
    while (koi_sound_active(voice)) {
        if (koi_keypressed()) { (void)koi_getchar(); break; }
        koi_sleep(50);
    }
    koi_sound_stop(voice);
    return 0;
}
