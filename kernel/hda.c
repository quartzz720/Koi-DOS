#include "hda.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "timer.h"
#include "paging.h"
#include "cpu.h"

/* Intel High Definition Audio.
 *
 * Built in slices with something observable at the end of each, the same way
 * the storage drivers were, because a sound card that is set up incorrectly
 * does not complain - it plays silence, and silence is what a card that is not
 * set up at all also plays. The slices were: the codec answering with its
 * vendor id, the widget walk printing a path, and a sine wave in a file.
 *
 * Two rings and a stream is the whole of it:
 *
 *   CORB    commands to the codec, a ring the controller reads
 *   RIRB    responses from it, a ring the controller writes
 *   stream  one buffer descriptor list pointing at memory, played in a loop
 *
 * The codec is not the controller. The controller moves bytes; the codec is a
 * little graph of widgets - converters, mixers, selectors and the physical
 * jacks - and getting sound out means finding a route through that graph and
 * unmuting every step of it. A single muted amplifier anywhere along the way
 * is indistinguishable from a driver that never ran.
 */

/* ---- Controller registers, at BAR0 --------------------------------------- */

#define REG_GCAP 0x00           /* word: how many streams, and of which kind */
#define REG_VMIN 0x02
#define REG_VMAJ 0x03
#define REG_GCTL 0x08
#define REG_WAKEEN 0x0C
#define REG_STATESTS 0x0E       /* word: one bit per codec that answered */
#define REG_INTCTL 0x20
#define REG_INTSTS 0x24
/* Global and controller interrupt enables. Nothing is routed to a handler, so
   nothing is delivered - but the RIRB already taught us that a controller can
   treat "enabled" as part of how it runs rather than only as where it
   reports, and that lesson cost an afternoon. */
#define INTCTL_GLOBAL 0x80000000U
#define INTCTL_CONTROLLER 0x40000000U
#define REG_CORBLBASE 0x40
#define REG_CORBUBASE 0x44
#define REG_CORBWP 0x48         /* word */
#define REG_CORBRP 0x4A         /* word */
#define REG_CORBCTL 0x4C        /* byte */
#define REG_CORBSIZE 0x4E       /* byte */
#define REG_RIRBLBASE 0x50
#define REG_RIRBUBASE 0x54
#define REG_RIRBWP 0x58         /* word */
#define REG_RINTCNT 0x5A        /* word */
#define REG_RIRBCTL 0x5C        /* byte */
#define REG_RIRBSTS 0x5D        /* byte */
#define REG_RIRBSIZE 0x5E       /* byte */
#define REG_DPLBASE 0x70        /* DMA position buffer, low half plus enable */
#define REG_DPUBASE 0x74

#define GCTL_RESET 0x00000001U          /* held low is reset; 1 is running */

#define CORBRP_RESET 0x8000U
#define CORBCTL_RUN 0x02U
#define RIRBCTL_RUN 0x02U
#define RIRBCTL_RESPONSE_INTERRUPT 0x01U
#define RIRBSTS_RESPONSE 0x01U
#define RIRBWP_RESET 0x8000U

/* Stream descriptors follow the global block, 0x20 bytes each, input streams
   first and then output. Which output stream is the first one therefore
   depends on how many input streams this controller has. */
#define STREAM_BASE 0x80
#define STREAM_STRIDE 0x20

#define SD_CTL 0x00             /* three bytes, and the top one is stream id */
#define SD_STS 0x03
#define SD_LPIB 0x04            /* where the controller is reading, in bytes */
#define SD_CBL 0x08             /* cyclic buffer length */
#define SD_LVI 0x0C             /* last valid buffer descriptor index */
#define SD_FIFOS 0x10
#define SD_FMT 0x12
#define SD_BDPL 0x18
#define SD_BDPU 0x1C

#define SD_CTL_RESET 0x01U
#define SD_CTL_RUN 0x02U
/* Completion, FIFO-error and descriptor-error interrupts. Enabled even though
   no interrupt is delivered anywhere, because the RIRB taught us that a
   controller can treat "enabled" as part of how it runs rather than only as
   where it reports - and they cost nothing with INTCTL clear. */
#define SD_CTL_INTERRUPTS 0x1CU

/* The DMA position buffer: eight bytes per stream descriptor, saying where
   each one has got to, written by the controller into memory.
 *
 * The same answer as SDnLPIB, by a different route, and worth having both:
 * LPIB is the obvious one and there are controllers where it does not move,
 * which is indistinguishable from a stream that never started. */
#define DPLBASE_ENABLE 0x00000001U

/* 48 kHz, 16-bit, two channels. Bit 14 clear selects the 48 kHz base, the
   divider and multiplier fields are 1:1, bits 6:4 are the sample width and
   bits 3:0 the channel count less one. */
#define FORMAT_48K_16_STEREO 0x0011U

/* ---- Codec verbs ---------------------------------------------------------
 *
 * A verb is one dword: codec address, node, and then either a 12-bit verb
 * with an 8-bit payload or a 4-bit verb with a 16-bit one. */
#define VERB_GET_PARAMETER 0xF00
#define VERB_GET_CONNECTION_SELECT 0xF01
#define VERB_SET_CONNECTION_SELECT 0x701
#define VERB_GET_CONNECTION_LIST 0xF02
#define VERB_SET_POWER_STATE 0x705
#define VERB_SET_STREAM_CHANNEL 0x706
#define VERB_SET_PIN_CONTROL 0x707
#define VERB_GET_CONFIG_DEFAULT 0xF1C
#define VERB_GET_POWER_STATE 0xF05
#define VERB_GET_STREAM_CHANNEL 0xF06
#define VERB_GET_PIN_CONTROL 0xF07
#define VERB_GET_FORMAT 0xA00           /* 4-bit verb 0xA, reads SET_FORMAT */
#define VERB_GET_PIN_SENSE 0xF09
#define VERB_SET_PIN_SENSE 0x709        /* take a fresh measurement */
#define VERB_SET_EAPD 0x70C
#define VERB_SET_FORMAT 0x200           /* 4-bit verb 0x2, 16-bit payload */
#define VERB_SET_AMP 0x300              /* 4-bit verb 0x3, 16-bit payload */

#define PARAM_VENDOR_ID 0x00
#define PARAM_NODE_COUNT 0x04
#define PARAM_FUNCTION_TYPE 0x05
#define PARAM_WIDGET_CAP 0x09
#define PARAM_PCM_SIZES 0x0A            /* which rates and widths it can do */
#define PARAM_STREAM_FORMATS 0x0B
#define PARAM_PIN_CAP 0x0C
#define PARAM_CONNECTION_COUNT 0x0E
#define PARAM_OUT_AMP_CAP 0x12

#define FUNCTION_AUDIO 0x01

#define WIDGET_TYPE(cap) (((cap) >> 20) & 0xF)
#define WIDGET_DAC 0x0
#define WIDGET_ADC 0x1
#define WIDGET_MIXER 0x2
#define WIDGET_SELECTOR 0x3
#define WIDGET_PIN 0x4

#define WIDGET_CAP_IN_AMP 0x0002U
#define WIDGET_CAP_OUT_AMP 0x0004U
#define WIDGET_CAP_CONNECTION_LIST 0x0100U
#define WIDGET_CAP_DIGITAL 0x0200U
#define WIDGET_CAP_POWER 0x0400U

#define PIN_CAP_TRIGGER 0x00000002U     /* the measurement must be asked for */
#define PIN_CAP_PRESENCE 0x00000004U
#define PIN_CAP_OUTPUT 0x00000010U
#define PIN_CAP_HEADPHONE 0x00000008U
#define PIN_CAP_EAPD 0x00010000U

#define PIN_CONTROL_OUT 0x40U
#define PIN_CONTROL_HEADPHONE 0x80U

/* Setting an amplifier: which half, which channels, which index, and then
   mute plus gain. 0xB000 is "the output amp, both channels, unmuted". */
#define AMP_SET_OUTPUT 0x8000U
#define AMP_SET_INPUT 0x4000U
#define AMP_SET_LEFT 0x2000U
#define AMP_SET_RIGHT 0x1000U
#define AMP_INDEX_SHIFT 8
#define AMP_MUTE 0x0080U

/* Ring sizes. 256 entries is the largest every controller supports and the
   only size worth asking for: the whole CORB is 1 KiB. */
#define CORB_ENTRIES 256
#define RIRB_ENTRIES 256

/* The ring is split across this many buffer descriptors. More than one
   because the spec requires at least two, and four because that is a
   convenient quarter to think about. */
#define BDL_ENTRIES 4

#define HDA_WINDOW_SIZE 0x4000

/* The stream tag. Any non-zero value; the codec is told the same one. */
#define STREAM_TAG 1

typedef struct __attribute__((packed)) {
    boot_uint64_t address;
    boot_uint32_t length;
    boot_uint32_t flags;        /* bit 0 asks for an interrupt we do not use */
} BDL_ENTRY;

static volatile boot_uint8_t* registers;
static boot_uint32_t* corb;             /* 256 dwords */
static boot_uint64_t* rirb;             /* 256 quadwords */
static boot_uint16_t corb_write;
static boot_uint16_t rirb_read;

static BDL_ENTRY* bdl;
static short* ring;                     /* HDA_RING_FRAMES * 2 samples */
static boot_uint32_t* positions;        /* the DMA position buffer */
static boot_uint32_t stream_offset;     /* the output stream descriptor */
static boot_uint32_t stream_index;      /* which descriptor that is */
static int position_source;             /* 1 LPIB, 2 the position buffer */

static const PCI_DEVICE* audio_controller;
static boot_uint8_t codec_address;
static boot_uint8_t function_group;
static boot_uint32_t codec_vendor;
static const char* codec_name = "none";
static void log(const char* text);
static const char* failure = "no controller on the PCI bus";
static int ready;

/* Where initialisation stopped.
 *
 * Every one of these messages used to go to COM1 and nowhere else, which is
 * fine on a machine with a serial port and useless on a laptop - and a laptop
 * is exactly where an HD Audio controller behaves in ways QEMU never will. The
 * reason is kept so that `sound` can print it. */
static int give_up(const char* reason) {
    failure = reason;
    log("HDA: ");
    log(reason);
    log("\n");
    return 0;
}

static void log(const char* text) { serial_write(text); }
static void log_dec(boot_uint64_t value) { serial_write_dec(value); }
static void log_hex(boot_uint64_t value) { serial_write_hex(value); }

static boot_uint8_t read8(boot_uint32_t offset) {
    return *(volatile boot_uint8_t*)(registers + offset);
}

static void write8(boot_uint32_t offset, boot_uint8_t value) {
    *(volatile boot_uint8_t*)(registers + offset) = value;
}

static boot_uint16_t read16(boot_uint32_t offset) {
    return *(volatile boot_uint16_t*)(registers + offset);
}

static void write16(boot_uint32_t offset, boot_uint16_t value) {
    *(volatile boot_uint16_t*)(registers + offset) = value;
}

static boot_uint32_t read32(boot_uint32_t offset) {
    return *(volatile boot_uint32_t*)(registers + offset);
}

static void write32(boot_uint32_t offset, boot_uint32_t value) {
    *(volatile boot_uint32_t*)(registers + offset) = value;
}

/* ---- Bringing the controller up ------------------------------------------ */

static int controller_reset(void) {
    boot_uint64_t start;

    /* Held in reset, then let go. Both directions are waited for: a
       controller that is still resetting accepts register writes and forgets
       them, which looks exactly like a driver writing to the wrong offset. */
    write32(REG_GCTL, read32(REG_GCTL) & ~GCTL_RESET);
    start = timer_ticks();
    while (read32(REG_GCTL) & GCTL_RESET) {
        if (timer_expired(start, 100))
            return give_up("the controller would not enter reset");
    }
    timer_wait(1);

    write32(REG_GCTL, read32(REG_GCTL) | GCTL_RESET);
    start = timer_ticks();
    while (!(read32(REG_GCTL) & GCTL_RESET)) {
        if (timer_expired(start, 100))
            return give_up("the controller would not leave reset");
    }

    /* The codecs need a moment after the link comes up before they will say
       they are there. The spec asks for 521 microseconds; a millisecond is
       the finest this clock measures and is safely more. */
    timer_wait(2);
    return 1;
}

static int rings_init(void) {
    boot_uint64_t page;
    boot_uint8_t size;

    /* CORB and RIRB share one page: 1 KiB and 2 KiB, both needing 128-byte
       alignment, which a page start and a 1 KiB offset both satisfy. */
    page = (boot_uint64_t)(unsigned long long)alloc_page();
    if (!page) return 0;
    memset((void*)(unsigned long long)page, 0, PAGE_SIZE);
    corb = (boot_uint32_t*)(unsigned long long)page;
    rirb = (boot_uint64_t*)(unsigned long long)(page + 1024);

    /* Size field 2 is 256 entries. The capability bits above it say which
       sizes this controller has; every one of them supports 256. */
    size = read8(REG_CORBSIZE);
    write8(REG_CORBSIZE, (boot_uint8_t)((size & 0xF0) | 0x2));
    size = read8(REG_RIRBSIZE);
    write8(REG_RIRBSIZE, (boot_uint8_t)((size & 0xF0) | 0x2));

    write32(REG_CORBLBASE, (boot_uint32_t)page);
    write32(REG_CORBUBASE, (boot_uint32_t)(page >> 32));
    write32(REG_RIRBLBASE, (boot_uint32_t)(page + 1024));
    write32(REG_RIRBUBASE, (boot_uint32_t)((page + 1024) >> 32));

    /* Reset the read pointer by setting the bit and waiting for it to read
       back, then clearing it and waiting again. Controllers that skip the
       handshake exist, so neither wait is fatal. */
    write16(REG_CORBRP, CORBRP_RESET);
    timer_wait(1);
    write16(REG_CORBRP, 0);
    timer_wait(1);
    write16(REG_CORBWP, 0);
    corb_write = 0;

    /* The RIRB write pointer resets by writing the bit; there is nothing to
       wait for and nothing to clear. */
    write16(REG_RIRBWP, RIRBWP_RESET);
    rirb_read = 0;

    /* One response per "interrupt", and the response interrupt enabled even
       though no interrupt is delivered anywhere.
     *
     * This looks like a contradiction and is the single thing that had this
     * driver reading exactly one answer and then hanging. The controller
     * stops fetching commands once it has produced RINTCNT responses, and
     * what releases it is software clearing the response bit in RIRBSTS. But
     * the controller only ever sets that bit if the response interrupt is
     * enabled - so with it disabled the bit never appears, clearing it does
     * nothing, and the command ring stalls for good after the first verb.
     *
     * Enabling it is harmless here because the global interrupt enable in
     * INTCTL is off: the bit gets set, we clear it, the count resets, and no
     * interrupt is ever raised. */
    write16(REG_RINTCNT, 1);

    write8(REG_CORBCTL, CORBCTL_RUN);
    write8(REG_RIRBCTL, RIRBCTL_RUN | RIRBCTL_RESPONSE_INTERRUPT);
    return 1;
}

/* Send one verb and wait for its answer.
 *
 * Polled, not interrupt-driven, and that is not a placeholder: every verb in
 * this driver is sent while setting up, where there is nothing else to do,
 * and after that none are sent at all. */
static int command(boot_uint8_t codec, boot_uint8_t node,
                   boot_uint32_t verb, boot_uint32_t payload,
                   boot_uint32_t* response) {
    boot_uint32_t value;
    boot_uint64_t start;
    boot_uint16_t written;
    int wide = (verb == VERB_SET_FORMAT || verb == VERB_SET_AMP);

    /* The dword is the same shape either way - codec, node, then twenty bits
       of verb and payload. A twelve-bit verb leaves eight bits for its
       payload and a four-bit verb leaves sixteen, and shifting the verb left
       by eight puts both in the right place. Writing the constants with their
       trailing zeroes is what makes that true of both. */
    value = ((boot_uint32_t)codec << 28) | ((boot_uint32_t)node << 20)
          | (verb << 8) | (wide ? (payload & 0xFFFF) : (payload & 0xFF));

    corb_write = (boot_uint16_t)((corb_write + 1) % CORB_ENTRIES);
    corb[corb_write] = value;
    write16(REG_CORBWP, corb_write);

    start = timer_ticks();
    for (;;) {
        written = (boot_uint16_t)(read16(REG_RIRBWP) & 0xFF);
        if (written != rirb_read) break;
        if (timer_expired(start, 100)) return 0;
    }

    rirb_read = (boot_uint16_t)((rirb_read + 1) % RIRB_ENTRIES);
    if (response) *response = (boot_uint32_t)rirb[rirb_read];
    /* Not housekeeping: this is what lets the next verb be fetched at all.
       See the comment on RINTCNT above. */
    write8(REG_RIRBSTS, RIRBSTS_RESPONSE);
    return 1;
}

/* One verb whose answer is wanted rather than discarded. */
static boot_uint32_t codec_ask(boot_uint8_t node, boot_uint32_t verb,
                               boot_uint32_t payload) {
    boot_uint32_t value = 0;
    if (!command(codec_address, node, verb, payload, &value)) return 0xFFFFFFFFU;
    return value;
}

static boot_uint32_t parameter(boot_uint8_t node, boot_uint32_t which) {
    boot_uint32_t value = 0;
    if (!command(codec_address, node, VERB_GET_PARAMETER, which, &value))
        return 0;
    return value;
}

/* ---- Finding a way out ---------------------------------------------------
 *
 * From a physical jack back to a converter. The graph runs the other way -
 * every widget lists what feeds it - so the search starts at the pin and ends
 * at a DAC, and unmutes each widget on the way back down. */

static boot_uint8_t path_dac;
static boot_uint8_t path_pin;

static void unmute_output(boot_uint8_t node, boot_uint32_t capabilities) {
    if (!(capabilities & WIDGET_CAP_OUT_AMP)) return;
    /* Gain 0x7F rather than a considered number: the amplifier's range is
       whatever this codec says it is, the mixer applies volume in software
       where it is exact, and a hardware amplifier left low is a machine that
       is silent for no visible reason. */
    command(codec_address, node, VERB_SET_AMP,
            AMP_SET_OUTPUT | AMP_SET_LEFT | AMP_SET_RIGHT | 0x7F, 0);
}

/* Silence a pin we are leaving.
 *
 * Two outputs on one converter both play it, so switching to the headphones
 * without doing this is not switching - it is adding. On a laptop that is the
 * speaker still going while somebody is wearing headphones, which is the
 * loudest possible way to be wrong. */
static void silence_output(boot_uint8_t node) {
    boot_uint32_t capabilities = parameter(node, PARAM_WIDGET_CAP);

    if (capabilities & WIDGET_CAP_OUT_AMP)
        command(codec_address, node, VERB_SET_AMP,
                AMP_SET_OUTPUT | AMP_SET_LEFT | AMP_SET_RIGHT | AMP_MUTE, 0);
    command(codec_address, node, VERB_SET_PIN_CONTROL, 0, 0);
}

static void unmute_input(boot_uint8_t node, boot_uint32_t capabilities,
                         boot_uint32_t index) {
    if (!(capabilities & WIDGET_CAP_IN_AMP)) return;
    command(codec_address, node, VERB_SET_AMP,
            AMP_SET_INPUT | AMP_SET_LEFT | AMP_SET_RIGHT
            | (index << AMP_INDEX_SHIFT) | 0x7F, 0);
}

static void power_up(boot_uint8_t node, boot_uint32_t capabilities) {
    if (!(capabilities & WIDGET_CAP_POWER)) return;
    command(codec_address, node, VERB_SET_POWER_STATE, 0, 0);
}

/* The list of widgets feeding this one. Short form packs four per response,
   long form two; both are read here because both exist in the wild. */
static boot_uint32_t connections(boot_uint8_t node, boot_uint8_t* into,
                                 boot_uint32_t room) {
    boot_uint32_t count;
    boot_uint32_t long_form;
    boot_uint32_t index;
    boot_uint32_t found = 0;

    count = parameter(node, PARAM_CONNECTION_COUNT);
    long_form = (count & 0x80) ? 1 : 0;
    count &= 0x7F;
    if (count > room) count = room;

    /* The top bit of an entry marks a range rather than a single widget - the
       list is allowed to say "everything from the last one to this". Masking
       it off leaves the widget at the end of the range, which is a real
       widget and the only one we could have named anyway. */
    for (index = 0; index < count; index++) {
        boot_uint32_t response = 0;
        if (long_form) {
            if (!command(codec_address, node, VERB_GET_CONNECTION_LIST,
                         index & ~1u, &response)) break;
            into[found++] = (boot_uint8_t)((index & 1)
                ? ((response >> 16) & 0xFF) : (response & 0xFF));
        } else {
            if (!command(codec_address, node, VERB_GET_CONNECTION_LIST,
                         index & ~3u, &response)) break;
            into[found++] = (boot_uint8_t)
                ((response >> ((index & 3) * 8)) & 0x7F);
        }
    }
    return found;
}

/* Depth-first from `node` towards a converter, unmuting behind us. Depth is
   capped because a codec that describes a cycle would otherwise take the
   kernel with it, and no real path is longer than a handful of steps. */
static int trace(boot_uint8_t node, int depth) {
    boot_uint32_t capabilities;
    boot_uint8_t inputs[16];
    boot_uint32_t count;
    boot_uint32_t index;

    if (depth > 8) return 0;
    capabilities = parameter(node, PARAM_WIDGET_CAP);
    if (!capabilities) return 0;

    if (WIDGET_TYPE(capabilities) == WIDGET_DAC) {
        path_dac = node;
        power_up(node, capabilities);
        unmute_output(node, capabilities);
        return 1;
    }
    if (!(capabilities & WIDGET_CAP_CONNECTION_LIST)) return 0;

    count = connections(node, inputs, 16);
    for (index = 0; index < count; index++) {
        if (!trace(inputs[index], depth + 1)) continue;
        power_up(node, capabilities);
        unmute_output(node, capabilities);
        unmute_input(node, capabilities, index);
        /* A selector passes exactly one of its inputs; say which. A mixer
           passes all of them and has no such control. */
        if (WIDGET_TYPE(capabilities) == WIDGET_SELECTOR)
            command(codec_address, node, VERB_SET_CONNECTION_SELECT, index, 0);
        return 1;
    }
    return 0;
}

/* Whether anything is plugged into a pin.
 *
 * A pin that is built in - a laptop's own speakers - has nothing to plug in
 * and always counts as usable. A jack may or may not be able to tell; the ones
 * that cannot are not unusual, and guessing "empty" for those would silence a
 * machine that works. */
static boot_uint8_t pin_sense(boot_uint8_t node, boot_uint32_t configuration,
                              boot_uint32_t pin_capabilities,
                              boot_uint32_t* raw) {
    boot_uint32_t connectivity = (configuration >> 30) & 0x3;
    boot_uint32_t capabilities = parameter(node, PARAM_WIDGET_CAP);
    boot_uint32_t response = 0;

    if (raw) *raw = 0;
    if (connectivity == 2) return HDA_SENSE_FIXED;
    if (!(pin_capabilities & PIN_CAP_PRESENCE)) return HDA_SENSE_UNKNOWN;

    /* A sleeping pin answers, and answers "nothing there".
     *
     * Only the pin that was chosen used to be powered up, and the choosing
     * happens after this - so every socket was asked while it was still in
     * whatever state the firmware left it, and a laptop with headphones in it
     * reported an empty jack. The measurement is a physical one; the circuit
     * that makes it has to be switched on first. */
    power_up(node, capabilities);
    if (capabilities & WIDGET_CAP_POWER) timer_wait(1);

    /* Some pins do not measure continuously - the reading is whatever was
       taken last, which on a machine that has just started is nothing at all
       until it is asked for. */
    if (pin_capabilities & PIN_CAP_TRIGGER) {
        command(codec_address, node, VERB_SET_PIN_SENSE, 0, 0);
        timer_wait(2);
    }

    if (!command(codec_address, node, VERB_GET_PIN_SENSE, 0, &response))
        return HDA_SENSE_UNKNOWN;

    /* A pin that is switched off does not measure, and says so as "nothing
     * there" rather than as an error.
     *
     * Found on a VIA codec in a laptop with headphones plugged in: every jack
     * reported empty at startup, and the same jack reported occupied the
     * moment it was chosen as the output - which is the one step between the
     * two readings. The socket had not changed. The circuit that watches it
     * had been switched on.
     *
     * So: if the pin says nothing is there, turn its output on, ask again,
     * and put the control register back exactly as it was. Codecs that answer
     * the first time never reach this and are not poked. */
    if (!(response & 0x80000000U) && (pin_capabilities & PIN_CAP_OUTPUT)) {
        boot_uint32_t previous = 0;
        boot_uint32_t control = PIN_CONTROL_OUT;
        boot_uint32_t second = 0;

        if (pin_capabilities & PIN_CAP_HEADPHONE)
            control |= PIN_CONTROL_HEADPHONE;
        if (command(codec_address, node, VERB_GET_PIN_CONTROL, 0, &previous) &&
            (previous & PIN_CONTROL_OUT) == 0) {
            command(codec_address, node, VERB_SET_PIN_CONTROL,
                    previous | control, 0);
            /* Long enough for a comparator to settle, and it has to be: the
               working reading on the machine this was written for came tens of
               milliseconds after the pin was switched on, not two. Thirty per
               jack is nothing at startup and the alternative is a measurement
               taken before the thing being measured exists. */
            timer_wait(30);
            /* Asked for whether or not the pin claims to need it. A codec that
               measures continuously ignores the request; one that lied about
               needing to be asked is the case this is here for. */
            command(codec_address, node, VERB_SET_PIN_SENSE, 0, 0);
            timer_wait(30);
            if (command(codec_address, node, VERB_GET_PIN_SENSE, 0, &second))
                response = second;
            command(codec_address, node, VERB_SET_PIN_CONTROL, previous, 0);

            log("HDA: node ");
            log_dec(node);
            log(" said nothing was there while switched off; switched on it says ");
            log_hex(response);
            log("\n");
        }
    }

    if (raw) *raw = response;
    return (response & 0x80000000U) ? HDA_SENSE_PRESENT : HDA_SENSE_EMPTY;
}

/* How much we want a given output, which is mostly a question about laptops.
 *
 * Ranking by what the jack is called - line out, then headphones, then the
 * built-in speaker - is right on a desk and wrong on a laptop, where it picks
 * the headphone socket and leaves the machine silent with nothing plugged into
 * it. What actually matters is whether anything is connected, so that comes
 * first and the name only breaks ties:
 *
 *   a jack with something in it        7   headphones win when they are worn
 *   the built-in speaker               4   always there, so always usable
 *   a jack that cannot report          3   guessing empty would silence a
 *                                          machine that works
 *   a jack reporting nothing in it     0   there is no point sending sound
 *                                          somewhere nobody is listening
 */
static int pin_rank(boot_uint32_t configuration, boot_uint8_t sense) {
    boot_uint32_t connectivity = (configuration >> 30) & 0x3;
    boot_uint32_t device = (configuration >> 20) & 0xF;
    int base;

    if (connectivity == 1) return 0;    /* no physical connection at all */
    switch (device) {
    case HDA_DEVICE_LINE_OUT: base = 3; break;
    case HDA_DEVICE_HEADPHONE: base = 3; break;
    case HDA_DEVICE_SPEAKER: base = 2; break;
    default: return 0;                  /* an input, or something digital */
    }

    switch (sense) {
    case HDA_SENSE_PRESENT: return base + 4;
    case HDA_SENSE_FIXED: return base + 2;
    case HDA_SENSE_UNKNOWN: return base;
    default: return 0;
    }
}

/* The same ranking with the question of what is plugged in removed, for the
   fall-back below: better to drive a socket nobody is using than to report no
   output at all on a machine that plainly has one. */
static int pin_rank_regardless(boot_uint32_t configuration) {
    return pin_rank(configuration, HDA_SENSE_UNKNOWN);
}

static HDA_PIN pins[HDA_PINS_MAX];
static boot_uint32_t pin_count;

static const char* device_name(boot_uint32_t device) {
    switch (device) {
    case HDA_DEVICE_LINE_OUT: return "line out";
    case HDA_DEVICE_SPEAKER: return "speaker";
    case HDA_DEVICE_HEADPHONE: return "headphones";
    default: return "other";
    }
}

/* One line per jack, in the codec's own words.
 *
 * The summary on the screen says what this driver concluded. This says what it
 * was told, which is the only thing worth having when the conclusion is wrong
 * on somebody else's machine and the machine is not here. */
static void log_pin(const HDA_PIN* pin) {
    log("HDA: pin node ");
    log_dec(pin->node);
    log(" ");
    log(device_name(pin->device));
    log(", config ");
    log_hex(pin->configuration);
    log(", pincap ");
    log_hex(pin->pin_capabilities);
    log(", sense ");
    log_hex(pin->sense_response);
    log(pin->sense == HDA_SENSE_PRESENT ? " -> something plugged in" :
        pin->sense == HDA_SENSE_EMPTY ? " -> nothing plugged in" :
        pin->sense == HDA_SENSE_FIXED ? " -> built in" :
        " -> cannot tell");
    if (!(pin->pin_capabilities & PIN_CAP_PRESENCE))
        log(" (no presence detect)");
    if (pin->connectivity == 1) log(" (not wired to anything)");
    log("\n");
}

/* Point the output at one particular pin, and bind the stream to whatever
   converter feeds it. Used once while starting and again by hda_select. */
static int configure_pin(boot_uint32_t index) {
    boot_uint8_t previous_dac = path_dac;
    boot_uint8_t previous_pin = path_pin;
    boot_uint32_t capabilities;
    boot_uint32_t pin_capabilities;
    boot_uint32_t control = PIN_CONTROL_OUT;

    if (index >= pin_count) return 0;
    path_pin = pins[index].node;
    path_dac = 0;
    /* Tracing first, because it is the step that can fail, and a machine that
       has just been silenced is the wrong place to discover that. */
    if (!trace(path_pin, 0)) {
        path_pin = previous_pin;
        path_dac = previous_dac;
        return 0;
    }
    if (previous_pin && previous_pin != path_pin) silence_output(previous_pin);

    capabilities = parameter(path_pin, PARAM_WIDGET_CAP);
    pin_capabilities = parameter(path_pin, PARAM_PIN_CAP);
    if (pin_capabilities & PIN_CAP_HEADPHONE) control |= PIN_CONTROL_HEADPHONE;
    power_up(path_pin, capabilities);
    command(codec_address, path_pin, VERB_SET_PIN_CONTROL, control, 0);
    unmute_output(path_pin, capabilities);
    /* External amplifiers on laptops are off until told otherwise, and a
       machine whose speakers are wired through one is silent without this
       while every register reads correct. */
    if (pin_capabilities & PIN_CAP_EAPD)
        command(codec_address, path_pin, VERB_SET_EAPD, 0x02, 0);

    for (boot_uint32_t other = 0; other < pin_count; other++)
        pins[other].chosen = (other == index);

    /* A stream and a converter are paired by a number they both carry, so a
       different converter has to be given it - and the one that had it has to
       give it up, because two converters answering to one stream tag is not a
       described thing and not worth finding out about. */
    if (previous_dac && path_dac != previous_dac)
        command(codec_address, previous_dac, VERB_SET_STREAM_CHANNEL, 0, 0);
    command(codec_address, path_dac, VERB_SET_FORMAT, FORMAT_48K_16_STEREO, 0);
    command(codec_address, path_dac, VERB_SET_STREAM_CHANNEL,
            (STREAM_TAG << 4) | 0, 0);
    return 1;
}

void hda_rescan(void) {
    if (!ready) return;
    log("HDA: asking every jack again\n");
    for (boot_uint32_t index = 0; index < pin_count; index++) {
        HDA_PIN* pin = &pins[index];
        pin->sense = pin_sense(pin->node, pin->configuration,
                               pin->pin_capabilities, &pin->sense_response);
        pin->rank = pin_rank(pin->configuration, pin->sense);
        log_pin(pin);
    }
}

int hda_select(boot_uint8_t node) {
    if (!ready) return 0;
    for (boot_uint32_t index = 0; index < pin_count; index++) {
        if (pins[index].node != node) continue;
        if (!configure_pin(index)) return 0;
        log("HDA: output moved to node ");
        log_dec(path_pin);
        log(", fed by the converter at node ");
        log_dec(path_dac);
        log("\n");
        return 1;
    }
    return 0;
}

static int find_path(boot_uint8_t first_widget, boot_uint32_t widget_count) {
    int node;
    int last = (int)first_widget + (int)widget_count;
    int best = -1;
    int best_rank = 0;
    boot_uint32_t index;

    pin_count = 0;
    if (last > 256) last = 256;
    for (node = first_widget; node < last; node++) {
        boot_uint32_t capabilities = parameter((boot_uint8_t)node,
                                               PARAM_WIDGET_CAP);
        boot_uint32_t pin_capabilities;
        boot_uint32_t configuration = 0;
        HDA_PIN* pin;

        if (WIDGET_TYPE(capabilities) != WIDGET_PIN) continue;
        if (capabilities & WIDGET_CAP_DIGITAL) continue;
        pin_capabilities = parameter((boot_uint8_t)node, PARAM_PIN_CAP);
        if (!(pin_capabilities & PIN_CAP_OUTPUT)) continue;
        if (pin_count >= HDA_PINS_MAX) break;

        command(codec_address, (boot_uint8_t)node, VERB_GET_CONFIG_DEFAULT, 0,
                &configuration);

        pin = &pins[pin_count++];
        pin->node = (boot_uint8_t)node;
        pin->configuration = configuration;
        pin->pin_capabilities = pin_capabilities;
        pin->device = (boot_uint8_t)((configuration >> 20) & 0xF);
        pin->connectivity = (boot_uint8_t)((configuration >> 30) & 0x3);
        pin->sense = pin_sense((boot_uint8_t)node, configuration,
                               pin_capabilities, &pin->sense_response);
        pin->rank = pin_rank(configuration, pin->sense);
        pin->chosen = 0;
        log_pin(pin);
    }

    for (index = 0; index < pin_count; index++)
        if (pins[index].rank > best_rank) {
            best_rank = pins[index].rank;
            best = (int)index;
        }

    /* Nothing is plugged in anywhere. Drive the best output there is rather
       than none: a socket nobody is using is silent either way, and a machine
       whose only jack cannot be sensed would otherwise never make a sound. */
    if (best < 0) {
        for (index = 0; index < pin_count; index++) {
            int rank = pin_rank_regardless(pins[index].configuration);
            if (rank > best_rank) { best_rank = rank; best = (int)index; }
        }
        if (best >= 0)
            log("HDA: nothing is plugged in; using the best output anyway\n");
    }

    if (best < 0) return give_up("the codec has no analogue output");

    if (!configure_pin((boot_uint32_t)best))
        return give_up("no converter feeds the output");

    log("HDA: ");
    log(device_name(pins[best].device));
    log(pins[best].sense == HDA_SENSE_PRESENT ? " (plugged in)"
        : pins[best].sense == HDA_SENSE_FIXED ? " (built in)" : "");
    log(" at node ");
    log_dec(path_pin);
    log(", fed by the converter at node ");
    log_dec(path_dac);
    log("\n");
    return 1;
}

/* ---- Codec discovery ----------------------------------------------------- */

static int find_codec(void) {
    boot_uint16_t present = read16(REG_STATESTS);
    boot_uint8_t address;

    for (address = 0; address < 15; address++) {
        boot_uint32_t vendor;
        boot_uint32_t nodes;
        boot_uint8_t first;
        boot_uint32_t count;
        boot_uint8_t function;

        if (!(present & (1u << address))) continue;
        codec_address = address;
        vendor = parameter(0, PARAM_VENDOR_ID);
        if (!vendor || vendor == 0xFFFFFFFFU) continue;

        nodes = parameter(0, PARAM_NODE_COUNT);
        first = (boot_uint8_t)((nodes >> 16) & 0xFF);
        count = nodes & 0xFF;

        for (function = first; count && function < first + count; function++) {
            boot_uint32_t type = parameter(function, PARAM_FUNCTION_TYPE);
            boot_uint32_t widgets;

            if ((type & 0x7F) != FUNCTION_AUDIO) continue;

            codec_vendor = vendor;
            log("HDA: codec ");
            log_dec(address);
            log(" is ");
            log_hex(vendor);
            log(", audio function group at node ");
            log_dec(function);
            log("\n");

            /* Powering the group up before walking it: a widget in a sleeping
               group answers, and answers with whatever it had last. */
            command(codec_address, function, VERB_SET_POWER_STATE, 0, 0);
            timer_wait(2);

            function_group = function;
            widgets = parameter(function, PARAM_NODE_COUNT);
            if (find_path((boot_uint8_t)((widgets >> 16) & 0xFF),
                          widgets & 0xFF))
                return 1;
            /* This one has nothing usable on it. Keep looking rather than
               giving up: a controller can carry more than one codec, and the
               first to answer is not necessarily the one the speakers are
               wired to. */
        }
    }

    log("HDA: no codec has an analogue output (state ");
    log_hex(present);
    log(", gctl ");
    log_hex(read32(REG_GCTL));
    log(", corbctl ");
    log_hex(read8(REG_CORBCTL));
    log(", rirbctl ");
    log_hex(read8(REG_RIRBCTL));
    log(", corbwp ");
    log_hex(read16(REG_CORBWP));
    log(", corbrp ");
    log_hex(read16(REG_CORBRP));
    log(", rirbwp ");
    log_hex(read16(REG_RIRBWP));
    log(")\n");
    return 0;
}

/* ---- The stream ---------------------------------------------------------- */

/* Configure one output stream descriptor and see whether it moves.
 *
 * Tried per descriptor rather than assuming the first, because a descriptor
 * the firmware left in a state we cannot see is indistinguishable from a
 * driver that has the arithmetic wrong, and the controller has four. */
static int try_stream(boot_uint32_t which, boot_uint32_t bytes) {
    boot_uint64_t start;

    stream_index = which;
    stream_offset = STREAM_BASE + which * STREAM_STRIDE;

    /* Reset the stream before configuring it. The run bit must be clear first,
       and the reset bit is written, waited for, cleared and waited for again.
       Both waits are failures rather than shrugs: a stream still held in reset
       accepts every register write below and then does nothing, which is a far
       more confusing thing to be told about later. */
    write8(stream_offset + SD_CTL, 0);
    timer_wait(1);
    write8(stream_offset + SD_CTL, SD_CTL_RESET);
    start = timer_ticks();
    while (!(read8(stream_offset + SD_CTL) & SD_CTL_RESET))
        if (timer_expired(start, 100))
            return give_up("the stream would not enter reset");
    write8(stream_offset + SD_CTL, 0);
    start = timer_ticks();
    while (read8(stream_offset + SD_CTL) & SD_CTL_RESET)
        if (timer_expired(start, 100))
            return give_up("the stream would not leave reset");

    write32(stream_offset + SD_CBL, bytes);
    write16(stream_offset + SD_LVI, BDL_ENTRIES - 1);
    write16(stream_offset + SD_FMT, FORMAT_48K_16_STEREO);
    write32(stream_offset + SD_BDPL, (boot_uint32_t)(unsigned long long)bdl);
    write32(stream_offset + SD_BDPU,
            (boot_uint32_t)(((boot_uint64_t)(unsigned long long)bdl) >> 32));

    /* The stream tag lives in the top byte of the control register, and the
       codec has to be told the same number: that pairing is what connects this
       buffer to that converter. Written as one dword rather than one byte,
       because a controller is entitled to latch the register as a whole and a
       byte write leaves the other two carrying whatever reset left there. */
    write32(stream_offset + SD_CTL,
            ((boot_uint32_t)STREAM_TAG << 20) | SD_CTL_INTERRUPTS);
    command(codec_address, path_dac, VERB_SET_FORMAT, FORMAT_48K_16_STEREO, 0);
    command(codec_address, path_dac, VERB_SET_STREAM_CHANNEL,
            (STREAM_TAG << 4) | 0, 0);
    /* Clear whatever the reset left in the status register: the error bits are
       write-one-to-clear and a stale one reads as a fresh failure. */
    write8(stream_offset + SD_STS, 0x1C);

    /* The run bit on its own, as a byte, leaving the rest of the register
       alone. A dword write here would also write the status register that
       shares its last byte, and setting a stream running is not the moment to
       find out whether a controller minds. */
    write8(stream_offset + SD_CTL,
           (boot_uint8_t)(read8(stream_offset + SD_CTL) | SD_CTL_RUN));

    /* Proof, not faith: something has to move. Both position sources are
       watched, because a controller whose LPIB does not advance is not the
       same thing as a stream that never started, and the two cannot be told
       apart if only one is consulted. */
    start = timer_ticks();
    for (;;) {
        if (read32(stream_offset + SD_LPIB)) { position_source = 1; return 1; }
        if (positions[stream_index * 2]) { position_source = 2; return 1; }
        if (timer_expired(start, 150)) break;
    }

    log("HDA: output stream ");
    log_dec(which);
    log(" did not move (ctl ");
    log_hex(read32(stream_offset + SD_CTL));
    log(", sts ");
    log_hex(read8(stream_offset + SD_STS));
    log(", cbl ");
    log_hex(read32(stream_offset + SD_CBL));
    log(", lvi ");
    log_hex(read16(stream_offset + SD_LVI));
    log(", fmt ");
    log_hex(read16(stream_offset + SD_FMT));
    log(", bdl ");
    log_hex(read32(stream_offset + SD_BDPU));
    log(":");
    log_hex(read32(stream_offset + SD_BDPL));
    log(", position ");
    log_hex(positions[which * 2]);
    log(")\n");
    /* What the codec made of what it was told.
     *
     * Everything above is the controller's side, and all of it reads back
     * correct - so the question moves to the other end of the link. A
     * converter that never took the format or the stream tag is a converter
     * that is not listening, and on a controller that idles its link when
     * nothing is streaming, that is enough to keep the DMA engine asleep.
     * Asking rather than assuming, because every one of these was written by
     * a command whose reply was thrown away. */
    log("HDA: converter ");
    log_dec(path_dac);
    log(" format ");
    log_hex(codec_ask(path_dac, VERB_GET_FORMAT, 0));
    log(", stream/channel ");
    log_hex(codec_ask(path_dac, VERB_GET_STREAM_CHANNEL, 0));
    log(", power ");
    log_hex(codec_ask(path_dac, VERB_GET_POWER_STATE, 0));
    log(", widget ");
    log_hex(parameter(path_dac, PARAM_WIDGET_CAP));
    log("\n");
    log("HDA: converter rates ");
    log_hex(parameter(path_dac, PARAM_PCM_SIZES));
    log(", formats ");
    log_hex(parameter(path_dac, PARAM_STREAM_FORMATS));
    log("; group rates ");
    log_hex(parameter(function_group, PARAM_PCM_SIZES));
    log(", power ");
    log_hex(codec_ask(function_group, VERB_GET_POWER_STATE, 0));
    log("\n");
    log("HDA: jack ");
    log_dec(path_pin);
    log(" control ");
    log_hex(codec_ask(path_pin, VERB_GET_PIN_CONTROL, 0));
    log(", power ");
    log_hex(codec_ask(path_pin, VERB_GET_POWER_STATE, 0));
    log("\n");

    /* And what the device looks like from the bus side, because a stream that
       is configured correctly and does not transfer is a question about the
       path to memory rather than about the stream. */
    if (audio_controller) {
        log("HDA: pci command ");
        log_hex(pci_config_read(audio_controller, 0x04) & 0xFFFF);
        log(", tcsel ");
        log_hex(pci_config_read8(audio_controller, 0x44));
        log(", intctl ");
        log_hex(read32(REG_INTCTL));
        log(", intsts ");
        log_hex(read32(REG_INTSTS));
        log(", gctl ");
        log_hex(read32(REG_GCTL));
        log("\n");
    }
    write8(stream_offset + SD_CTL, 0);
    return 0;
}

static int stream_start_from(boot_uint32_t first_output,
                             boot_uint32_t output_streams) {
    boot_uint32_t index;
    boot_uint64_t address;
    boot_uint32_t bytes = HDA_RING_FRAMES * HDA_CHANNELS * 2;

    ring = (short*)alloc_pages((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    bdl = (BDL_ENTRY*)alloc_page();
    positions = (boot_uint32_t*)alloc_page();
    if (!ring || !bdl || !positions) return give_up("out of memory for the ring");
    memset(ring, 0, bytes);
    memset(bdl, 0, PAGE_SIZE);
    memset(positions, 0, PAGE_SIZE);

    /* Everything the controller reads has to be somewhere it can reach. A
       controller that cannot address 64 bits, handed buffers above 4 GiB, does
       not fail - it transfers from the truncated address, which is somebody
       else's memory. */
    if (!(read16(REG_GCAP) & 0x0001) &&
        (((boot_uint64_t)(unsigned long long)ring >> 32) ||
         ((boot_uint64_t)(unsigned long long)bdl >> 32)))
        return give_up("its buffers are above 4 GiB and it cannot reach them");

    address = (boot_uint64_t)(unsigned long long)ring;
    for (index = 0; index < BDL_ENTRIES; index++) {
        bdl[index].address = address + index * (bytes / BDL_ENTRIES);
        bdl[index].length = bytes / BDL_ENTRIES;
        bdl[index].flags = 0;
    }

    /* Out of the cache and into memory, before anything is told to read it.
       See cpu_flush_cache: a controller whose stream traffic is not snooped
       reads what is in memory, not what is in this processor's cache, and a
       descriptor list that never left the cache reads back as zeros - which is
       a list of empty buffers, which is a stream that transfers nothing and
       reports no error at all. */
    cpu_flush_cache(bdl, BDL_ENTRIES * sizeof(BDL_ENTRY));
    cpu_flush_cache(ring, bytes);
    /* The position buffer is written by the controller and only read here, so
       what this does is get our zeroes out of the way: the line is clean
       afterwards and reading it later invalidates rather than writing back. */
    cpu_flush_cache(positions, PAGE_SIZE);

    /* Where the controller writes how far each stream has got. Set up before
       any stream starts, because the enable bit is only examined then. */
    address = (boot_uint64_t)(unsigned long long)positions;
    write32(REG_DPUBASE, (boot_uint32_t)(address >> 32));
    write32(REG_DPLBASE, (boot_uint32_t)address | DPLBASE_ENABLE);

    log("HDA: ring at ");
    log_hex((boot_uint64_t)(unsigned long long)ring);
    log(", descriptors at ");
    log_hex((boot_uint64_t)(unsigned long long)bdl);
    log(" (first entry ");
    log_hex(bdl[0].address);
    log(" for ");
    log_dec(bdl[0].length);
    log(" bytes), positions at ");
    log_hex(read32(REG_DPLBASE));
    log("\n");

    for (index = 0; index < output_streams; index++)
        if (try_stream(first_output + index, bytes)) {
            if (index)
                log("HDA: the first output stream would not run; "
                    "using a later one\n");
            if (position_source == 2)
                log("HDA: LPIB does not move on this controller; "
                    "using the position buffer\n");
            return 1;
        }

    return give_up("no output stream would start");
}

/* ---- Entry point --------------------------------------------------------- */

static void name_codec(void) {
    /* The vendor half of the id is the part worth showing; the rest is a part
       number that means nothing without a table nobody has. */
    const char* vendor;

    switch (codec_vendor >> 16) {
    case 0x1002: vendor = "ATI"; break;
    case 0x1013: vendor = "Cirrus Logic"; break;
    case 0x10DE: vendor = "NVIDIA"; break;
    case 0x10EC: vendor = "Realtek"; break;
    case 0x1102: vendor = "Creative"; break;
    /* The three below turned up on real laptops and were reported as
       "unknown", which reads as a driver that failed rather than a name it
       had never been told. */
    case 0x1106: vendor = "VIA"; break;
    case 0x111D: vendor = "IDT"; break;
    case 0x11D4: vendor = "Analog Devices"; break;
    case 0x13F6: vendor = "C-Media"; break;
    case 0x14F1: vendor = "Conexant"; break;
    case 0x1AF4: vendor = "QEMU"; break;
    case 0x8086: vendor = "Intel"; break;
    case 0x8384: vendor = "SigmaTel"; break;
    default: vendor = "unknown"; break;
    }
    codec_name = vendor;
}

int hda_init(const PCI_DEVICE* controller) {
    boot_uint64_t base;
    boot_uint16_t capabilities;
    boot_uint32_t input_streams;
    boot_uint32_t output_streams;

    ready = 0;
    if (!controller) return 0;

    base = pci_bar_address(controller, 0);
    if (!base) return give_up("BAR0 is not a memory window");
    if (!paging_map_device(base, HDA_WINDOW_SIZE))
        return give_up("could not map its register window");

    /* Power, then bus mastering, then the vendor register - in that order.
     *
     * Firmware that did not use the audio device is entitled to leave it
     * asleep, and a sleeping device decodes nothing: every register reads as
     * ones, the reset never completes, and the driver concludes the hardware
     * is broken. Nothing in QEMU is ever asleep, which is why this was missing
     * and why it only showed up on a real laptop. */
    if (!pci_power_on(controller))
        return give_up("the controller will not wake up from a low power state");
    pci_enable_bus_mastering(controller);

    /* Intel's traffic class selector, which is one byte and has to be written
       as one.
     *
     * A controller asking for a traffic class the root complex does not carry
     * does not fail - it simply never transfers anything, which is precisely
     * what a stream that starts and never moves looks like. The command rings
     * are unaffected, so the machine can hold a whole conversation with its
     * codec and still not play a note.
     *
     * Written a byte at a time because the three bytes above it are other
     * registers. The dword this used to write back put their own values back
     * too, which is fine right up until one of them is write-one-to-clear. */
    if (controller->vendor_id == 0x8086) {
        boot_uint8_t tcsel = pci_config_read8(controller, 0x44);
        if (tcsel & 0x07)
            pci_config_write8(controller, 0x44, (boot_uint8_t)(tcsel & ~0x07));
    }
    audio_controller = controller;

    registers = (volatile boot_uint8_t*)(unsigned long long)base;

    /* A device that is not decoding reads as all ones. Saying so beats
       reporting a reset timeout, which is what happens next and which sounds
       like a different problem entirely. */
    if (read32(REG_GCTL) == 0xFFFFFFFFU && read16(REG_GCAP) == 0xFFFFU)
        return give_up("the controller does not answer at its own address");

    if (!controller_reset()) return 0;

    capabilities = read16(REG_GCAP);
    input_streams = (capabilities >> 8) & 0xF;
    output_streams = (capabilities >> 12) & 0xF;

    log("HDA: version ");
    log_dec(read8(REG_VMAJ));
    log(".");
    log_dec(read8(REG_VMIN));
    log(" at ");
    log_hex(base);
    log(", ");
    log_dec(output_streams);
    log(" output stream(s)\n");

    if (!output_streams)
        return give_up("the controller has no output stream");
    /* Nothing is routed to a handler, so nothing is delivered: the mixer
       refills from the timer tick and reads the position to decide how much.
       The enables are set anyway, for the reason on their definition. */
    write32(REG_INTCTL, INTCTL_GLOBAL | INTCTL_CONTROLLER | 0xFF);
    write16(REG_WAKEEN, 0);

    if (!rings_init())
        return give_up("out of memory for the command rings");
    if (!find_codec()) return 0;
    name_codec();
    /* Input descriptors come first, so the output ones start past them. */
    if (!stream_start_from(input_streams, output_streams)) return 0;

    ready = 1;
    log("HDA: ready, ");
    log(codec_name);
    log(" codec, 48 kHz stereo\n");
    return 1;
}

int hda_ready(void) { return ready; }
const char* hda_failure(void) { return ready ? "none" : failure; }

boot_uint32_t hda_pin_count(void) { return pin_count; }

const HDA_PIN* hda_pin(boot_uint32_t index) {
    return index < pin_count ? &pins[index] : 0;
}

boot_uint8_t hda_converter(void) { return path_dac; }

const char* hda_output_name(void) {
    boot_uint32_t index;
    if (!ready) return "none";
    for (index = 0; index < pin_count; index++)
        if (pins[index].chosen) return device_name(pins[index].device);
    return "unknown";
}
const char* hda_codec_name(void) { return ready ? codec_name : "none"; }
boot_uint32_t hda_codec_id(void) { return codec_vendor; }
short* hda_ring(void) { return ready ? ring : 0; }

boot_uint32_t hda_position(void) {
    boot_uint32_t bytes;
    if (!ready) return 0;
    if (position_source == 2) {
        /* Written by the controller, so the copy in this processor's cache is
           whatever it was last time. Dropping the line first is the only way
           to see what the device actually put there. */
        cpu_flush_cache(&positions[stream_index * 2], 4);
        bytes = positions[stream_index * 2];
    } else {
        bytes = read32(stream_offset + SD_LPIB);
    }
    return (bytes / (HDA_CHANNELS * 2)) % HDA_RING_FRAMES;
}
