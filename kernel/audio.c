#include "audio.h"
#include "hda.h"
#include "pci.h"
#include "cpu.h"
#include "serial.h"
#include "string.h"

/* The mixer.
 *
 * One job: keep the ring the sound card is reading from filled a little way
 * ahead of where it has got to, and never let it catch up. Everything else -
 * how many sounds are playing, what rate they were recorded at, how loud they
 * are - is arithmetic on the way to that.
 *
 * It runs from the timer interrupt. That is not for precision; it is because
 * the alternative is running it from wherever the system happens to be
 * looping, and the moment a program stops making system calls - a game
 * rendering a frame, a file copy grinding through a large file - the sound
 * would stop with it. A millisecond tick refills 48 frames, which is nothing,
 * and it happens whatever the machine is otherwise doing.
 *
 * No floating point: programs and the kernel are both compiled
 * -mgeneral-regs-only because nothing configures SSE state after
 * ExitBootServices. Positions are 32.32 fixed point, which at 48 kHz can
 * address a sound four billion samples long to a resolution far finer than
 * one sample.
 */

#define VOICE_SAMPLES 0
#define VOICE_TONE 1

typedef struct {
    const void* samples;
    boot_uint32_t frames;           /* in the source, not the output */
    boot_uint64_t position;         /* 32.32 into the source */
    boot_uint64_t step;             /* 32.32 source frames per output frame */
    boot_uint32_t remaining;        /* output frames left, for a tone */
    boot_uint16_t generation;       /* so a stale handle cannot stop a new sound */
    boot_uint8_t kind;
    boot_uint8_t bits;
    boot_uint8_t channels;
    boot_uint8_t loop;
    boot_uint8_t left_gain;         /* 0-255, from the pan */
    boot_uint8_t right_gain;
    boot_uint8_t volume;
    volatile boot_uint8_t active;   /* written last, so a half-built voice is
                                       never seen by the interrupt */
    /* How many programs were resident when this was started. A voice reads
       the samples where they lie in the program's own memory, so it must stop
       when that program's memory goes back - and only then. Stopping every
       voice instead silenced the desktop's music whenever anything was run
       from it. */
    boot_uint8_t owner;
} VOICE;

static VOICE voices[AUDIO_VOICES];
static boot_uint16_t next_generation = 1;
static int master_volume = 200;
static int ready;
static const char* device_name = "none";

/* Where the mixer has written up to, and where the hardware has got to, both
   counted from the beginning rather than modulo the ring: the difference
   between them is the only quantity that matters and a wrapped counter makes
   it ambiguous. */
static boot_uint64_t written;
static boot_uint64_t played;
static boot_uint32_t last_position;

/* One period of a sine, 256 points. Generated rather than computed: there is
   no libm here, and a table this size is inaudible from the real thing at the
   frequencies a beep uses. */
static const short sine[256] = {
         0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
      6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
     12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
     18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
     23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
     27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
     30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
     32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
     32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
     32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
     30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
     27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
     23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
     18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
     12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
      6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
         0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
     -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
    -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
    -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
    -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
    -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
     -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804
};

int audio_init(void) {
    boot_uint32_t index;

    ready = 0;
    device_name = "none";

    /* Every HD Audio controller on the machine, in the order the bus scan
       found them, until one comes up. A desktop with a graphics card has two:
       the card's, which carries sound over HDMI to a monitor that may not be
       plugged in, and the chipset's, which is where the speakers are. Trying
       each in turn and keeping the first that has an analogue jack behind it
       picks the right one without knowing anything about either.
       Walked by index rather than with pci_find(), which answers "at or after"
       and would hand back the same failed controller for ever. */
    for (index = 0; index < pci_device_count(); index++) {
        const PCI_DEVICE* controller = pci_device(index);

        if (!controller) continue;
        if (controller->class_code != PCI_CLASS_MULTIMEDIA) continue;
        if (controller->subclass != PCI_SUBCLASS_HD_AUDIO) continue;
        if (!hda_init(controller)) continue;

        ready = 1;
        device_name = hda_codec_name();
        written = 0;
        played = 0;
        last_position = hda_position();
        return 1;
    }
    serial_write("AUDIO: no usable sound device\n");
    return 0;
}

int audio_ready(void) { return ready; }
const char* audio_device_name(void) { return device_name; }
const char* audio_failure(void) { return ready ? "none" : hda_failure(); }

void audio_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;
    master_volume = volume;
}

int audio_volume(void) { return master_volume; }

/* ---- Starting and stopping ----------------------------------------------- */

static int claim(VOICE** into, boot_uint64_t* flags) {
    int index;

    /* Interrupts held off for the search and the fill: the mixer runs from the
       tick and would otherwise be free to walk this table halfway through a
       voice being built. They stay held until publish() sets `active`, which
       is what makes a finished voice appear all at once. */
    *flags = cpu_hold_interrupts();
    for (index = 0; index < AUDIO_VOICES; index++) {
        if (!voices[index].active) {
            VOICE* voice = &voices[index];
            memset(voice, 0, sizeof(*voice));
            voice->generation = next_generation++;
            if (!next_generation) next_generation = 1;
            *into = voice;
            return index | (voice->generation << 8);
        }
    }
    cpu_release_interrupts(*flags);
    return -1;
}

static void publish(VOICE* voice, boot_uint64_t flags) {
    voice->active = 1;
    cpu_release_interrupts(flags);
}

/* Pan without a hole in the middle: centred is full on both sides, and moving
   away turns one side down rather than turning the other up. */
static void set_pan(VOICE* voice, int pan) {
    if (pan < 0) pan = 0;
    if (pan > 255) pan = 255;
    if (pan <= 128) {
        voice->left_gain = 255;
        voice->right_gain = (boot_uint8_t)(pan * 255 / 128);
    } else {
        voice->left_gain = (boot_uint8_t)((255 - pan) * 255 / 127);
        voice->right_gain = 255;
    }
}

int audio_play(const void* samples, boot_uint32_t frames, boot_uint32_t rate,
               int bits, int channels, int volume, int pan, int loop) {
    VOICE* voice;
    boot_uint64_t flags;
    int handle;

    if (!ready || !samples || !frames || !rate) return -1;
    if (bits != AUDIO_U8 && bits != AUDIO_S16) return -1;
    if (channels != 1 && channels != 2) return -1;
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;

    handle = claim(&voice, &flags);
    if (handle < 0) return -1;

    voice->kind = VOICE_SAMPLES;
    voice->samples = samples;
    voice->frames = frames;
    voice->position = 0;
    /* How far into the source one output frame moves. A sound effect recorded
       at 11025 Hz advances just under a quarter of a sample per output frame,
       which is the whole of the resampling. */
    voice->step = ((boot_uint64_t)rate << 32) / HDA_RATE;
    voice->bits = (boot_uint8_t)bits;
    voice->channels = (boot_uint8_t)channels;
    voice->volume = (boot_uint8_t)volume;
    voice->loop = (boot_uint8_t)(loop ? 1 : 0);
    set_pan(voice, pan);
    publish(voice, flags);
    return handle;
}

int audio_tone(boot_uint32_t hertz, boot_uint32_t milliseconds, int volume) {
    VOICE* voice;
    boot_uint64_t flags;
    int handle;

    if (!ready || !hertz || !milliseconds) return -1;
    if (hertz > HDA_RATE / 2) return -1;   /* above this it is not a tone */
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;

    handle = claim(&voice, &flags);
    if (handle < 0) return -1;

    voice->kind = VOICE_TONE;
    voice->position = 0;
    /* Position is the fraction of one period, so the whole 64-bit value wraps
       exactly when the wave does and there is nothing to reset. */
    voice->step = ((boot_uint64_t)hertz << 32) / HDA_RATE;
    voice->remaining = (boot_uint32_t)((boot_uint64_t)milliseconds
                                       * HDA_RATE / 1000ULL);
    voice->volume = (boot_uint8_t)volume;
    voice->left_gain = 255;
    voice->right_gain = 255;
    publish(voice, flags);
    return handle;
}

int audio_set_params(int voice, int volume, int pan) {
    boot_uint64_t flags;
    int index = voice & 0xFF;
    boot_uint16_t generation = (boot_uint16_t)((voice >> 8) & 0xFFFF);
    int changed = 0;

    if (voice < 0 || index >= AUDIO_VOICES) return -1;
    flags = cpu_hold_interrupts();
    if (voices[index].active && voices[index].generation == generation) {
        if (volume >= 0) {
            if (volume > 255) volume = 255;
            voices[index].volume = (boot_uint8_t)volume;
        }
        if (pan >= 0) set_pan(&voices[index], pan);
        changed = 1;
    }
    cpu_release_interrupts(flags);
    return changed ? 0 : -1;
}

void audio_stop(int voice) {
    boot_uint64_t flags;
    int index = voice & 0xFF;
    boot_uint16_t generation = (boot_uint16_t)((voice >> 8) & 0xFFFF);

    if (voice < 0 || index >= AUDIO_VOICES) return;
    flags = cpu_hold_interrupts();
    /* The generation is what stops a handle kept too long from silencing
       whatever sound happens to be in that slot now. */
    if (voices[index].generation == generation) voices[index].active = 0;
    cpu_release_interrupts(flags);
}

void audio_stop_all(void) {
    boot_uint64_t flags = cpu_hold_interrupts();
    int index;
    for (index = 0; index < AUDIO_VOICES; index++) voices[index].active = 0;
    cpu_release_interrupts(flags);
}

void audio_set_owner(int owner) {
    boot_uint64_t flags = cpu_hold_interrupts();
    for (int index = 0; index < AUDIO_VOICES; index++)
        if (voices[index].active && !voices[index].owner)
            voices[index].owner = (boot_uint8_t)owner;
    cpu_release_interrupts(flags);
}

void audio_stop_deeper_than(int depth) {
    boot_uint64_t flags = cpu_hold_interrupts();
    for (int index = 0; index < AUDIO_VOICES; index++)
        if (voices[index].owner > (boot_uint8_t)depth) {
            voices[index].active = 0;
            voices[index].owner = 0;
        }
    cpu_release_interrupts(flags);
}

int audio_active(int voice) {
    int index = voice & 0xFF;
    boot_uint16_t generation = (boot_uint16_t)((voice >> 8) & 0xFFFF);

    if (voice < 0 || index >= AUDIO_VOICES) return 0;
    return voices[index].active && voices[index].generation == generation;
}

/* How far into the sound a voice has got, in source frames.
 *
 * The mixer already knows: it walks the source in 32.32 fixed point so that a
 * sound recorded at one rate can play at another, and the whole part of that
 * number is the answer. Nothing had to be counted; it only had to be asked
 * for, which is why a progress bar was impossible yesterday and is arithmetic
 * today.
 *
 * Read without stopping the mixer. A torn read gives a position off by less
 * than one frame at 48 kHz, which is not visible on a bar and is not worth
 * disabling interrupts for. */
boot_uint32_t audio_position(int voice) {
    int index = voice & 0xFF;
    boot_uint16_t generation = (boot_uint16_t)((voice >> 8) & 0xFFFF);

    if (voice < 0 || index >= AUDIO_VOICES) return 0;
    if (!voices[index].active || voices[index].generation != generation) return 0;
    return (boot_uint32_t)(voices[index].position >> 32);
}

boot_uint32_t audio_length(int voice) {
    int index = voice & 0xFF;
    boot_uint16_t generation = (boot_uint16_t)((voice >> 8) & 0xFFFF);

    if (voice < 0 || index >= AUDIO_VOICES) return 0;
    if (!voices[index].active || voices[index].generation != generation) return 0;
    return voices[index].frames;
}

/* Move the playing point. The one write is a whole 64-bit store, so the mixer
   either sees the old position or the new one and never half of each - which
   is the only reason this does not have to stop it first. */
int audio_seek(int voice, boot_uint32_t frame) {
    int index = voice & 0xFF;
    boot_uint16_t generation = (boot_uint16_t)((voice >> 8) & 0xFFFF);

    if (voice < 0 || index >= AUDIO_VOICES) return -1;
    if (!voices[index].active || voices[index].generation != generation) return -1;
    if (frame >= voices[index].frames) frame = voices[index].frames - 1;
    voices[index].position = (boot_uint64_t)frame << 32;
    return 0;
}

boot_uint32_t audio_voices_playing(void) {
    boot_uint32_t count = 0;
    int index;
    for (index = 0; index < AUDIO_VOICES; index++)
        if (voices[index].active) count++;
    return count;
}

/* ---- Mixing -------------------------------------------------------------- */

/* One source frame, as a signed 16-bit pair. Nearest sample rather than
   interpolated: at 48 kHz output the error is inaudible against sound effects
   recorded at 11 kHz in 1993, and interpolating costs a second fetch and a
   multiply per voice per frame. */
static void fetch(const VOICE* voice, boot_uint32_t index,
                  int* left, int* right) {
    if (voice->bits == AUDIO_U8) {
        const boot_uint8_t* samples = (const boot_uint8_t*)voice->samples;
        if (voice->channels == 1) {
            int value = ((int)samples[index] - 128) << 8;
            *left = value;
            *right = value;
        } else {
            *left = ((int)samples[index * 2] - 128) << 8;
            *right = ((int)samples[index * 2 + 1] - 128) << 8;
        }
    } else {
        const short* samples = (const short*)voice->samples;
        if (voice->channels == 1) {
            *left = samples[index];
            *right = samples[index];
        } else {
            *left = samples[index * 2];
            *right = samples[index * 2 + 1];
        }
    }
}

static void mix_frame(short* out) {
    int left = 0;
    int right = 0;
    int index;

    for (index = 0; index < AUDIO_VOICES; index++) {
        VOICE* voice = &voices[index];
        int sample_left;
        int sample_right;

        if (!voice->active) continue;

        if (voice->kind == VOICE_TONE) {
            int value = sine[(voice->position >> 24) & 0xFF];
            sample_left = value;
            sample_right = value;
            voice->position += voice->step;
            if (!voice->remaining || !--voice->remaining) voice->active = 0;
        } else {
            boot_uint32_t at = (boot_uint32_t)(voice->position >> 32);
            if (at >= voice->frames) {
                if (!voice->loop) { voice->active = 0; continue; }
                voice->position -= (boot_uint64_t)voice->frames << 32;
                at = (boot_uint32_t)(voice->position >> 32);
                if (at >= voice->frames) { voice->active = 0; continue; }
            }
            fetch(voice, at, &sample_left, &sample_right);
            voice->position += voice->step;
        }

        sample_left = sample_left * voice->volume / 255;
        sample_right = sample_right * voice->volume / 255;
        left += sample_left * voice->left_gain / 255;
        right += sample_right * voice->right_gain / 255;
    }

    left = left * master_volume / 255;
    right = right * master_volume / 255;

    /* Clipping, rather than wrapping. Sixteen voices at full volume can reach
       eight times what fits, and a wrapped sum is not a loud sound - it is a
       different, extremely unpleasant one. */
    if (left > 32767) left = 32767;
    if (left < -32768) left = -32768;
    if (right > 32767) right = 32767;
    if (right < -32768) right = -32768;

    out[0] = (short)left;
    out[1] = (short)right;
}

void audio_service(void) {
    short* ring;
    boot_uint32_t position;
    boot_uint64_t target;

    if (!ready) return;
    ring = hda_ring();
    if (!ring) return;

    /* The controller reports where it is inside the ring. Turning that back
       into a count from the beginning only needs the knowledge that it cannot
       have gone round more than once since the last look - the ring is 85 ms
       long and this runs every millisecond. */
    position = hda_position();
    played += (position + HDA_RING_FRAMES - last_position) % HDA_RING_FRAMES;
    last_position = position;

    /* Fallen behind - the machine was busy with interrupts off for longer than
       the lead. Skip to the play head rather than mixing the frames that have
       already gone out, which would only make the gap longer. */
    if (written < played) written = played;

    target = played + AUDIO_LEAD_FRAMES;
    /* Never write over what is about to be played. The ring is much longer
       than the lead, so this only matters if the lead is ever raised. */
    if (target > played + HDA_RING_FRAMES - 64)
        target = played + HDA_RING_FRAMES - 64;

    {
        boot_uint64_t first = written;
        while (written < target) {
            mix_frame(ring + (written % HDA_RING_FRAMES) * HDA_CHANNELS);
            written++;
        }
        /* And out of the cache, for the same reason the descriptor list is
           flushed once: a controller reading this without snooping would keep
           playing whatever was in memory before. Two or three cache lines a
           millisecond, and nothing at all on a machine that does snoop. */
        if (written > first) {
            boot_uint32_t from = (boot_uint32_t)(first % HDA_RING_FRAMES);
            boot_uint32_t count = (boot_uint32_t)(written - first);

            if (from + count <= HDA_RING_FRAMES) {
                cpu_flush_cache(ring + from * HDA_CHANNELS,
                                (boot_uint64_t)count * HDA_CHANNELS * 2);
            } else {
                boot_uint32_t head = HDA_RING_FRAMES - from;
                cpu_flush_cache(ring + from * HDA_CHANNELS,
                                (boot_uint64_t)head * HDA_CHANNELS * 2);
                cpu_flush_cache(ring,
                                (boot_uint64_t)(count - head) * HDA_CHANNELS * 2);
            }
        }
    }
}
