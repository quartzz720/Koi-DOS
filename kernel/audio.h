#ifndef KERNEL_AUDIO_H
#define KERNEL_AUDIO_H

#include "../include/bootinfo.h"

/* Sound, with no hardware in it.
 *
 * The device driver plays one endless stream of 48 kHz stereo and knows
 * nothing else. Everything that makes that stream interesting is here: voices,
 * resampling, volume and the mixing itself. Splitting it this way means a
 * second sound device would be a file that fills a ring, not a second copy of
 * a mixer - and it means the arithmetic that is easy to get wrong lives in one
 * place where it can be read.
 *
 * There is no floating point. Positions inside a sound are 32.32 fixed point,
 * which is more resolution than any sample rate needs and costs one 64-bit
 * add per frame per voice.
 */

#define AUDIO_VOICES 16

/* How far ahead of the hardware the mixer keeps the ring filled. This is the
 * delay between asking for a sound and hearing it, so it is as small as it can
 * be while still surviving a late refill: the tick is every millisecond, and
 * thirty of them is a wide margin for a machine that is busy drawing. */
#define AUDIO_LEAD_FRAMES 1440U         /* 30 ms at 48 kHz */

/* Sample formats a voice may be given. Anything else is converted by whoever
   is asking - these two are what sound effects in the wild actually are. */
#define AUDIO_U8 8                      /* unsigned, 0x80 is silence */
#define AUDIO_S16 16                    /* signed, little endian */

int audio_init(void);
int audio_ready(void);

/* What is playing it, for the boot log and `dosfetch`. */
const char* audio_device_name(void);

/* Start a sound and return a handle, or -1 when every voice is busy.
 *
 * The samples are NOT copied: the buffer must stay put until the sound ends
 * or is stopped. That is deliberate - a sound effect is a lump already in
 * memory and copying it per shot would be the most expensive part of playing
 * it.
 *
 * `volume` is 0 to 255. `pan` is 0 (left) to 255 (right), 128 centred. */
int audio_play(const void* samples, boot_uint32_t frames, boot_uint32_t rate,
               int bits, int channels, int volume, int pan, int loop);

/* A tone, generated rather than sampled. What `beep` is made of. */
int audio_tone(boot_uint32_t hertz, boot_uint32_t milliseconds, int volume);

/* Change a sound that is already playing. Pass -1 for either to leave it as
   it is. Returns 0, or -1 if that voice has already finished - which is not
   an error, only an answer. */
int audio_set_params(int voice, int volume, int pan);

void audio_stop(int voice);
void audio_stop_all(void);
int audio_active(int voice);
boot_uint32_t audio_voices_playing(void);

/* 0 to 255, applied to everything. */
void audio_set_volume(int volume);
int audio_volume(void);

/* Mix ahead of where the hardware is reading. Called from the timer tick, so
   sound keeps playing through work that never stops to poll. */
void audio_service(void);

#endif
