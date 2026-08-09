#include "koi.h"
#include "wav.h"

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


static void say(const char* text) { koi_print(text); koi_print("\n"); }

int main(void) {
    const char* name = koi_arguments();
    long handle;
    long size;
    unsigned char* file;
    WAV_FORMAT format;
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

    {
        const char* why;
        data_size = wav_parse(file, (unsigned int)size, &format, &data_at, &why);
        if (!data_size) { say(why); return 1; }
    }
    /* The reasons a file is refused are the shared reader's to give, because
       it is the one that looked. Repeating the checks here was repeating the
       chance of the two disagreeing. */
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
