#include "hda.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "timer.h"
#include "paging.h"

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
#define VERB_SET_EAPD 0x70C
#define VERB_SET_FORMAT 0x200           /* 4-bit verb 0x2, 16-bit payload */
#define VERB_SET_AMP 0x300              /* 4-bit verb 0x3, 16-bit payload */

#define PARAM_VENDOR_ID 0x00
#define PARAM_NODE_COUNT 0x04
#define PARAM_FUNCTION_TYPE 0x05
#define PARAM_WIDGET_CAP 0x09
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
static boot_uint32_t stream_offset;     /* the output stream descriptor */

static boot_uint8_t codec_address;
static boot_uint32_t codec_vendor;
static const char* codec_name = "none";
static int ready;

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
        if (timer_expired(start, 100)) {
            log("HDA: the controller would not enter reset\n");
            return 0;
        }
    }
    timer_wait(1);

    write32(REG_GCTL, read32(REG_GCTL) | GCTL_RESET);
    start = timer_ticks();
    while (!(read32(REG_GCTL) & GCTL_RESET)) {
        if (timer_expired(start, 100)) {
            log("HDA: the controller would not leave reset\n");
            return 0;
        }
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

/* How much we want a given jack. Line out first, then headphones, then the
   built-in speaker: on a desk the first is what the speakers are in, and on a
   laptop only the last two exist. */
static int pin_rank(boot_uint32_t configuration) {
    boot_uint32_t connectivity = (configuration >> 30) & 0x3;
    boot_uint32_t device = (configuration >> 20) & 0xF;

    if (connectivity == 1) return 0;    /* no physical connection at all */
    switch (device) {
    case 0x0: return 4;                 /* line out */
    case 0x2: return 3;                 /* headphone out */
    case 0x1: return 2;                 /* speaker */
    default: return 0;                  /* an input, or something digital */
    }
}

static int find_path(boot_uint8_t first_widget, boot_uint32_t widget_count) {
    int node;
    int last = (int)first_widget + (int)widget_count;
    boot_uint8_t best = 0;
    int best_rank = 0;
    boot_uint32_t best_configuration = 0;

    if (last > 256) last = 256;
    for (node = first_widget; node < last; node++) {
        boot_uint32_t capabilities = parameter((boot_uint8_t)node,
                                               PARAM_WIDGET_CAP);
        boot_uint32_t pin_capabilities;
        boot_uint32_t configuration;
        int rank;

        if (WIDGET_TYPE(capabilities) != WIDGET_PIN) continue;
        if (capabilities & WIDGET_CAP_DIGITAL) continue;
        pin_capabilities = parameter(node, PARAM_PIN_CAP);
        if (!(pin_capabilities & PIN_CAP_OUTPUT)) continue;

        configuration = 0;
        command(codec_address, node, VERB_GET_CONFIG_DEFAULT, 0,
                &configuration);
        rank = pin_rank(configuration);
        if (rank > best_rank) {
            best_rank = rank;
            best = node;
            best_configuration = configuration;
        }
    }

    if (!best_rank) {
        log("HDA: the codec has no analogue output jack\n");
        return 0;
    }

    path_pin = best;
    path_dac = 0;
    if (!trace(best, 0)) {
        log("HDA: no converter feeds the output jack\n");
        return 0;
    }

    {
        boot_uint32_t capabilities = parameter(path_pin, PARAM_WIDGET_CAP);
        boot_uint32_t pin_capabilities = parameter(path_pin, PARAM_PIN_CAP);
        boot_uint32_t control = PIN_CONTROL_OUT;

        if (pin_capabilities & PIN_CAP_HEADPHONE)
            control |= PIN_CONTROL_HEADPHONE;
        power_up(path_pin, capabilities);
        command(codec_address, path_pin, VERB_SET_PIN_CONTROL, control, 0);
        unmute_output(path_pin, capabilities);
        /* External amplifiers on laptops are off until told otherwise, and a
           machine whose speakers are wired through one is silent without
           this while every register reads correct. */
        if (pin_capabilities & PIN_CAP_EAPD)
            command(codec_address, path_pin, VERB_SET_EAPD, 0x02, 0);
    }

    log("HDA: jack at node ");
    log_dec(path_pin);
    log(" (configuration ");
    log_hex(best_configuration);
    log(") fed by converter at node ");
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

            widgets = parameter(function, PARAM_NODE_COUNT);
            return find_path((boot_uint8_t)((widgets >> 16) & 0xFF),
                             widgets & 0xFF);
        }
    }

    log("HDA: no codec answered (state ");
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

static int stream_start(void) {
    boot_uint32_t index;
    boot_uint64_t address;
    boot_uint64_t start;
    boot_uint32_t bytes = HDA_RING_FRAMES * HDA_CHANNELS * 2;

    ring = (short*)alloc_pages((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    bdl = (BDL_ENTRY*)alloc_page();
    if (!ring || !bdl) {
        log("HDA: out of memory for the ring\n");
        return 0;
    }
    memset(ring, 0, bytes);
    memset(bdl, 0, PAGE_SIZE);

    address = (boot_uint64_t)(unsigned long long)ring;
    for (index = 0; index < BDL_ENTRIES; index++) {
        bdl[index].address = address + index * (bytes / BDL_ENTRIES);
        bdl[index].length = bytes / BDL_ENTRIES;
        bdl[index].flags = 0;
    }

    /* Reset the stream before configuring it. The run bit must be clear
       first, and the reset bit is written, waited for, cleared and waited for
       again - the same handshake the controller itself uses. */
    write8(stream_offset + SD_CTL, 0);
    timer_wait(1);
    write8(stream_offset + SD_CTL, SD_CTL_RESET);
    start = timer_ticks();
    while (!(read8(stream_offset + SD_CTL) & SD_CTL_RESET))
        if (timer_expired(start, 100)) break;
    write8(stream_offset + SD_CTL, 0);
    start = timer_ticks();
    while (read8(stream_offset + SD_CTL) & SD_CTL_RESET)
        if (timer_expired(start, 100)) break;

    write32(stream_offset + SD_CBL, bytes);
    write16(stream_offset + SD_LVI, BDL_ENTRIES - 1);
    write16(stream_offset + SD_FMT, FORMAT_48K_16_STEREO);
    write32(stream_offset + SD_BDPL,
            (boot_uint32_t)(boot_uint64_t)(unsigned long long)bdl);
    write32(stream_offset + SD_BDPU,
            (boot_uint32_t)(((boot_uint64_t)(unsigned long long)bdl) >> 32));

    /* The stream tag lives in the top byte of the control register, and the
       codec has to be told the same number: that pairing is what connects
       this buffer to that converter. */
    write8(stream_offset + SD_CTL + 2, (boot_uint8_t)(STREAM_TAG << 4));
    command(codec_address, path_dac, VERB_SET_FORMAT,
            FORMAT_48K_16_STEREO, 0);
    command(codec_address, path_dac, VERB_SET_STREAM_CHANNEL,
            (STREAM_TAG << 4) | 0, 0);

    write8(stream_offset + SD_CTL,
           (boot_uint8_t)(read8(stream_offset + SD_CTL) | SD_CTL_RUN));

    /* Proof, not faith: the position register has to move. A stream that was
       configured wrongly sits at zero, and everything up to here would have
       reported success. */
    start = timer_ticks();
    while (read32(stream_offset + SD_LPIB) == 0) {
        if (timer_expired(start, 200)) {
            log("HDA: the stream was started and did not move\n");
            return 0;
        }
    }
    return 1;
}

/* ---- Entry point --------------------------------------------------------- */

static void name_codec(void) {
    /* The vendor half of the id is the part worth showing; the rest is a part
       number that means nothing without a table nobody has. */
    const char* vendor;

    switch (codec_vendor >> 16) {
    case 0x1002: vendor = "ATI"; break;
    case 0x10DE: vendor = "NVIDIA"; break;
    case 0x10EC: vendor = "Realtek"; break;
    case 0x1102: vendor = "Creative"; break;
    case 0x11D4: vendor = "Analog Devices"; break;
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
    if (!base) {
        log("HDA: BAR0 is not a memory window\n");
        return 0;
    }
    if (!paging_map_device(base, HDA_WINDOW_SIZE)) {
        log("HDA: could not map its register window\n");
        return 0;
    }
    pci_enable_bus_mastering(controller);
    registers = (volatile boot_uint8_t*)(unsigned long long)base;

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

    if (!output_streams) {
        log("HDA: the controller has no output stream\n");
        return 0;
    }
    /* Input descriptors come first, so the first output one is past them. */
    stream_offset = STREAM_BASE + input_streams * STREAM_STRIDE;

    /* Interrupts stay off. The mixer refills from the timer tick and reads
       the position register to decide how much, which needs no interrupt and
       leaves nothing to go wrong in an unfamiliar handler. */
    write32(REG_INTCTL, 0);
    write16(REG_WAKEEN, 0);

    if (!rings_init()) {
        log("HDA: out of memory for the command rings\n");
        return 0;
    }
    if (!find_codec()) return 0;
    name_codec();
    if (!stream_start()) return 0;

    ready = 1;
    log("HDA: ready, ");
    log(codec_name);
    log(" codec, 48 kHz stereo\n");
    return 1;
}

int hda_ready(void) { return ready; }
const char* hda_codec_name(void) { return ready ? codec_name : "none"; }
boot_uint32_t hda_codec_id(void) { return codec_vendor; }
short* hda_ring(void) { return ready ? ring : 0; }

boot_uint32_t hda_position(void) {
    boot_uint32_t bytes;
    if (!ready) return 0;
    bytes = read32(stream_offset + SD_LPIB);
    return (bytes / (HDA_CHANNELS * 2)) % HDA_RING_FRAMES;
}
