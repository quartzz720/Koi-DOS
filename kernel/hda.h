#ifndef KERNEL_HDA_H
#define KERNEL_HDA_H

#include "pci.h"

/* Intel High Definition Audio.
 *
 * The device-facing half of sound: it finds a codec, traces a path from a
 * physical output jack back to a converter, and leaves one stream running
 * forever over a ring of memory. Nothing here knows what a sound is. The
 * mixer in audio.c writes frames into that ring ahead of where the controller
 * is reading, and the controller never stops - a stream that is started and
 * stopped per sound clicks, and a stream that is always running does not.
 *
 * HD Audio rather than AC'97 because AC'97 is not in any machine this could
 * run on: the audio device in this desk is an AMD HDA controller, and every
 * laptop made since about 2005 is the same. QEMU emulates both, which makes
 * the easy one a trap.
 */

/* Frames the ring holds, and the format everything is mixed to. Fixed on
   purpose: a converter that can be told 48 kHz stereo 16-bit is every
   converter, and resampling belongs in one place rather than in a driver. */
#define HDA_RATE 48000U
#define HDA_CHANNELS 2U
#define HDA_RING_FRAMES 4096U          /* 85 ms - the ceiling, not the delay */

int hda_init(const PCI_DEVICE* controller);
int hda_ready(void);

/* What was found, for `dosfetch` and for the boot log. */
const char* hda_codec_name(void);
boot_uint32_t hda_codec_id(void);

/* The ring the controller is reading from, as interleaved left/right pairs.
   Writing to it is how sound is made; there is no other path. */
short* hda_ring(void);

/* Which frame of the ring the controller is playing now. Everything before it
   has already gone out; everything after it is still ours to change. */
boot_uint32_t hda_position(void);

#endif
