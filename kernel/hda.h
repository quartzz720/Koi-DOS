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

/* Every analogue output the codec describes, and which one is in use.
 *
 * This exists because "there is no sound" has several completely different
 * causes that look identical from a chair: no controller, a controller with no
 * codec, a codec whose only outputs are digital, a headphone jack with nothing
 * plugged into it, or the right jack chosen and muted somewhere. The `sound`
 * command prints this, and that is the difference between guessing and
 * knowing. */
#define HDA_PINS_MAX 16

#define HDA_DEVICE_LINE_OUT 0
#define HDA_DEVICE_SPEAKER 1
#define HDA_DEVICE_HEADPHONE 2

#define HDA_SENSE_UNKNOWN 0    /* the pin cannot report whether it is in use */
#define HDA_SENSE_EMPTY 1
#define HDA_SENSE_PRESENT 2
#define HDA_SENSE_FIXED 3      /* built in - there is nothing to plug in */

typedef struct {
    boot_uint8_t node;
    boot_uint8_t device;           /* HDA_DEVICE_*, or something else */
    boot_uint8_t connectivity;     /* 0 jack, 1 none, 2 fixed, 3 both */
    boot_uint8_t sense;            /* HDA_SENSE_* */
    boot_uint8_t chosen;
    boot_uint32_t configuration;
    /* Kept so the log can carry what the codec actually said rather than what
       this driver concluded from it. A jack that reports nothing plugged in
       and a jack that was never asked look identical in a summary, and they
       are not the same problem. */
    boot_uint32_t pin_capabilities;
    boot_uint32_t sense_response;
    int rank;
} HDA_PIN;

boot_uint32_t hda_pin_count(void);
const HDA_PIN* hda_pin(boot_uint32_t index);

/* Ask every jack again, now.
 *
 * The first measurement is taken while the machine is starting, which is the
 * one moment somebody is definitely not holding a pair of headphones. Without
 * this, testing a socket costs a reboot per attempt - and the raw answers go
 * to the log, so a machine that gets it wrong can say what it was told. */
void hda_rescan(void);

/* Send the sound somewhere else on purpose. Returns 0 when there is no such
   pin or no converter reaches it.
 *
 * Deliberate rather than automatic: a rescan that moved the output on its own
 * would, on a codec whose jack detection does not work, move it to a socket
 * with nothing in it and call that an improvement. Choosing by hand is also
 * the measurement that separates "the jack cannot be sensed" from "the jack
 * cannot be driven", which nothing else here can tell apart. */
int hda_select(boot_uint8_t node);

/* The converter feeding the chosen pin, and what kind of output it is. */
boot_uint8_t hda_converter(void);
const char* hda_output_name(void);

/* Where initialisation stopped, when it did. The reason used to go to COM1 and
   nowhere else, which is fine on a machine with a serial port and useless on a
   laptop - and a laptop is exactly where the hardware behaves in ways QEMU
   never will. */
const char* hda_failure(void);

/* The ring the controller is reading from, as interleaved left/right pairs.
   Writing to it is how sound is made; there is no other path. */
short* hda_ring(void);

/* Which frame of the ring the controller is playing now. Everything before it
   has already gone out; everything after it is still ours to change. */
boot_uint32_t hda_position(void);

#endif
