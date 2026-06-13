#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum sirc_ann {
    ANN_BIT = 0,
    ANN_AGC,
    ANN_PAUSE,
    ANN_START,
    ANN_CMD,
    ANN_ADDR,
    ANN_EXT,
    ANN_REMOTE,
    ANN_WARN,
    NUM_ANN,
};

#define IR_CH 0

#define AGC_USEC 2400.0
#define ONE_USEC 1200.0
#define ZERO_USEC 600.0
#define PAUSE_USEC 600.0
#define TOLERANCE 0.30

/* Device name lookup table from lists.py */
typedef struct {
    uint8_t address;
    int8_t has_extended;
    uint16_t extended;
    const char* name_long;
    const char* name_short;
} sirc_device_entry;

typedef struct {
    uint8_t cmd;
    const char* key_name;
} sirc_cmd_entry;

typedef struct {
    const sirc_device_entry* device;
    const sirc_cmd_entry* cmds;
    int num_cmds;
} sirc_address_table;

/* Numbers (shared across all devices) */
static const sirc_cmd_entry sirc_numbers[] = {
    { 0x00, "1" },
    { 0x01, "2" },
    { 0x02, "3" },
    { 0x03, "4" },
    { 0x04, "5" },
    { 0x05, "6" },
    { 0x06, "7" },
    { 0x07, "8" },
    { 0x08, "9" },
    { 0x09, "0/10" },
};

/* TV (0x01, no extended) */
static const sirc_cmd_entry sirc_tv_cmds[] = {
    { 0x15, "Power" },
    { 0x25, "Input" },
    { 0x33, "Right" },
    { 0x34, "Left" },
    { 0x3A, "Display" },
    { 0x60, "Home" },
    { 0x65, "Enter" },
    { 0x74, "Up" },
    { 0x75, "Down" },
};

/* Video (0x0B, no extended) */
static const sirc_cmd_entry sirc_video_cmds[] = {
    { 0x18, "Stop" },
    { 0x19, "Pause" },
    { 0x1A, "Play" },
    { 0x1B, "Rewind" },
    { 0x1C, "Fast Forward" },
    { 0x42, "Up" },
    { 0x43, "Down" },
    { 0x4D, "Home" },
    { 0x51, "Enter" },
    { 0x5A, "Display" },
    { 0x61, "Right" },
    { 0x62, "Left" },
};

/* BlueRay Input (0x10, ext 0x28) */
static const sirc_cmd_entry sirc_br_input_cmds[] = {
    { 0x16, "BlueRay" },
};

/* Playback (0x10, ext 0x08) */
static const sirc_cmd_entry sirc_playback_cmds[] = {
    { 0x2A, "Shuffle" },
    { 0x2C, "Repeat" },
    { 0x2E, "Folder Down" },
    { 0x2F, "Folder Up" },
    { 0x30, "Previous" },
    { 0x31, "Next" },
    { 0x32, "Play" },
    { 0x33, "Rewind" },
    { 0x34, "Fast Forward" },
    { 0x38, "Stop" },
    { 0x39, "Pause" },
    { 0x73, "Options" },
    { 0x7D, "Return" },
};

/* CD (0x11, no extended) */
static const sirc_cmd_entry sirc_cd_cmds[] = {
    { 0x28, "Display" },
    { 0x30, "Previous" },
    { 0x31, "Next" },
    { 0x32, "Play" },
    { 0x33, "Rewind" },
    { 0x34, "Fast Forward" },
    { 0x38, "Stop" },
    { 0x39, "Pause" },
};

/* BD (0x1A, ext 0xE2) */
static const sirc_cmd_entry sirc_bd_cmds[] = {
    { 0x18, "Stop" },
    { 0x19, "Pause" },
    { 0x1A, "Play" },
    { 0x1B, "Rewind" },
    { 0x1C, "Fast Forward" },
    { 0x29, "Menu" },
    { 0x2C, "Top Menu" },
    { 0x39, "Up" },
    { 0x3A, "Down" },
    { 0x3B, "Left" },
    { 0x3C, "Right" },
    { 0x3D, "Enter" },
    { 0x3F, "Options" },
    { 0x41, "Display" },
    { 0x42, "Home" },
    { 0x43, "Return" },
    { 0x56, "Next" },
    { 0x57, "Previous" },
};

/* DVD (0x1A, ext 0x49) */
static const sirc_cmd_entry sirc_dvd_cmds[] = {
    { 0x0B, "Enter" },
    { 0x0E, "Return" },
    { 0x0F, "Clear" },
    { 0x17, "Options" },
    { 0x1A, "Top Menu" },
    { 0x1B, "Menu" },
    { 0x1F, "Program" },
    { 0x28, "Time" },
    { 0x2A, "A-B" },
    { 0x2C, "Repeat" },
    { 0x30, "Previous" },
    { 0x31, "Next" },
    { 0x32, "Play" },
    { 0x33, "Rewind" },
    { 0x34, "Fast Forward" },
    { 0x35, "Shuffle" },
    { 0x38, "Stop" },
    { 0x39, "Pause" },
    { 0x54, "Display" },
    { 0x60, "Slow Reverse" },
    { 0x61, "Slow Forward" },
    { 0x63, "Subtitle" },
    { 0x64, "Audio" },
    { 0x65, "Angle" },
    { 0x79, "Up" },
    { 0x7A, "Down" },
    { 0x7B, "Left" },
    { 0x7C, "Right" },
};

/* PS2 (0x1A, ext 0xDA) */
static const sirc_cmd_entry sirc_ps2_cmds[] = {
    { 0x15, "Reset" },
    { 0x16, "Eject" },
    { 0x50, "Select" },
    { 0x51, "L3" },
    { 0x52, "R3" },
    { 0x53, "Start" },
    { 0x54, "Up" },
    { 0x55, "Right" },
    { 0x56, "Down" },
    { 0x57, "Left" },
    { 0x58, "L2" },
    { 0x59, "R2" },
    { 0x5A, "L1" },
    { 0x5B, "R1" },
    { 0x5C, "Triangle" },
    { 0x5D, "Circle" },
    { 0x5E, "Cross" },
    { 0x5F, "Square" },
};

/* Keypad (0x30, no extended) */
static const sirc_cmd_entry sirc_keypad_cmds[] = {
    { 0x0C, "Enter" },
    { 0x12, "Volume Up" },
    { 0x13, "Volume Down" },
    { 0x14, "Mute" },
    { 0x15, "Power" },
    { 0x21, "Tuner" },
    { 0x22, "Video" },
    { 0x25, "CD" },
    { 0x4D, "Home" },
    { 0x4B, "Display" },
    { 0x60, "Sleep" },
    { 0x6A, "TV" },
    { 0x53, "Home" },
    { 0x7C, "Game" },
    { 0x7D, "DVD" },
};

/* Arrows (0xB0, no extended) */
static const sirc_cmd_entry sirc_arrows_cmds[] = {
    { 0x7A, "Left" },
    { 0x7B, "Right" },
    { 0x78, "Up" },
    { 0x79, "Down" },
    { 0x77, "Amp Menu" },
};

/* TV Extra (0x97, no extended) */
static const sirc_cmd_entry sirc_tv_extra_cmds[] = {
    { 0x23, "Return" },
    { 0x36, "Options" },
};

/* Device entries */
static const sirc_device_entry sirc_devices[] = {
    /* TV */
    { 0x01, 0, 0,    "TV: ",      "TV:" },
    /* Video */
    { 0x0B, 0, 0,    "Video: ",   "V:" },
    /* BlueRay Input */
    { 0x10, 1, 0x28, "BlueRay: ", "BR:" },
    /* Playback */
    { 0x10, 1, 0x08, "Playback: ","PB:" },
    /* CD */
    { 0x11, 0, 0,    "CD: ",      "CD:" },
    /* BD */
    { 0x1A, 1, 0xE2, "BlueRay: ", "BD:" },
    /* DVD */
    { 0x1A, 1, 0x49, "DVD: ",     "DVD:" },
    /* PS2 */
    { 0x1A, 1, 0xDA, "PS2: ",     "PS2:" },
    /* Keypad */
    { 0x30, 0, 0,    "Keypad: ",  "KP:" },
    /* Arrows */
    { 0xB0, 0, 0,    "Arrows: ",  "Ar:" },
    /* TV Extra */
    { 0x97, 0, 0,    "TV Extra",  "TV:" },
};

/* Command tables corresponding to each device entry */
static const sirc_cmd_entry* sirc_device_cmds[] = {
    sirc_tv_cmds,
    sirc_video_cmds,
    sirc_br_input_cmds,
    sirc_playback_cmds,
    sirc_cd_cmds,
    sirc_bd_cmds,
    sirc_dvd_cmds,
    sirc_ps2_cmds,
    sirc_keypad_cmds,
    sirc_arrows_cmds,
    sirc_tv_extra_cmds,
};

static const int sirc_device_cmd_counts[] = {
    sizeof(sirc_tv_cmds) / sizeof(sirc_tv_cmds[0]),
    sizeof(sirc_video_cmds) / sizeof(sirc_video_cmds[0]),
    sizeof(sirc_br_input_cmds) / sizeof(sirc_br_input_cmds[0]),
    sizeof(sirc_playback_cmds) / sizeof(sirc_playback_cmds[0]),
    sizeof(sirc_cd_cmds) / sizeof(sirc_cd_cmds[0]),
    sizeof(sirc_bd_cmds) / sizeof(sirc_bd_cmds[0]),
    sizeof(sirc_dvd_cmds) / sizeof(sirc_dvd_cmds[0]),
    sizeof(sirc_ps2_cmds) / sizeof(sirc_ps2_cmds[0]),
    sizeof(sirc_keypad_cmds) / sizeof(sirc_keypad_cmds[0]),
    sizeof(sirc_arrows_cmds) / sizeof(sirc_arrows_cmds[0]),
    sizeof(sirc_tv_extra_cmds) / sizeof(sirc_tv_extra_cmds[0]),
};

#define NUM_SIRC_DEVICES (sizeof(sirc_devices) / sizeof(sirc_devices[0]))
#define NUM_SIRC_NUMBERS (sizeof(sirc_numbers) / sizeof(sirc_numbers[0]))

static const sirc_device_entry* find_device(uint16_t address, int has_extended, uint16_t extended)
{
    int i;
    for (i = 0; i < (int)NUM_SIRC_DEVICES; i++) {
        if (sirc_devices[i].address == (uint8_t)address) {
            if (sirc_devices[i].has_extended) {
                if (has_extended && sirc_devices[i].extended == (uint8_t)extended)
                    return &sirc_devices[i];
            } else {
                if (!has_extended)
                    return &sirc_devices[i];
            }
        }
    }
    return NULL;
}

static const char* find_key_name(const sirc_device_entry* device, uint8_t cmd)
{
    int i;
    int dev_idx = (int)(device - sirc_devices);

    /* Search device-specific commands */
    const sirc_cmd_entry* cmds = sirc_device_cmds[dev_idx];
    int num_cmds = sirc_device_cmd_counts[dev_idx];
    for (i = 0; i < num_cmds; i++) {
        if (cmds[i].cmd == cmd)
            return cmds[i].key_name;
    }

    /* Search shared number commands */
    for (i = 0; i < (int)NUM_SIRC_NUMBERS; i++) {
        if (sirc_numbers[i].cmd == cmd)
            return sirc_numbers[i].key_name;
    }

    return NULL;
}

typedef struct {
    uint64_t samplerate;
    double snum_per_us;
    int active;
    int out_ann;
} sirc_state;

static struct srd_channel sirc_channels[] = {
    { "ir", "IR", "IR data line", 0, SRD_CHANNEL_SDATA, NULL },
};

static struct srd_decoder_option sirc_options[] = {
    { "polarity", NULL, "Polarity", NULL, NULL },
};

static const char* sirc_ann_labels[][3] = {
    { "", "bit", "Bit" },
    { "", "agc", "AGC" },
    { "", "pause", "Pause" },
    { "", "start", "Start" },
    { "", "command", "Command" },
    { "", "address", "Address" },
    { "", "extended", "Extended" },
    { "", "remote", "Remote" },
    { "", "warning", "Warning" },
};

static const int sirc_row_bits_classes[] = { ANN_BIT, ANN_AGC, ANN_PAUSE, -1 };
static const int sirc_row_fields_classes[] = { ANN_START, ANN_CMD, ANN_ADDR, ANN_EXT, -1 };
static const int sirc_row_remotes_classes[] = { ANN_REMOTE, -1 };
static const int sirc_row_warnings_classes[] = { ANN_WARN, -1 };
static const struct srd_c_ann_row sirc_ann_rows[] = {
    { "bits", "Bits", sirc_row_bits_classes, 3 },
    { "fields", "Fields", sirc_row_fields_classes, 4 },
    { "remotes", "Remotes", sirc_row_remotes_classes, 1 },
    { "warnings", "Warnings", sirc_row_warnings_classes, 1 },
};

static const char* sirc_inputs[] = { "logic", NULL };
static const char* sirc_outputs[] = { NULL };
static const char* sirc_tags[] = { "IR", NULL };

static int tolerance_check(sirc_state* s, uint64_t ss, uint64_t es, double expected)
{
    double microseconds = (double)(es - ss) / s->snum_per_us;
    double tol = expected * TOLERANCE;
    return (microseconds > (expected - tol)) && (microseconds < (expected + tol));
}

static uint16_t bitpack_lsb(uint8_t* bits, int count)
{
    uint16_t val = 0;
    int i;
    for (i = 0; i < count; i++)
        val |= ((uint16_t)bits[i] << i);
    return val;
}

static int read_pulse(struct srd_decoder_inst* di, sirc_state* s,
    int high, double time_us, uint64_t pulse_ss,
    uint64_t* pulse_es)
{
    uint64_t max_samples = (uint64_t)(time_us * 1.30 * s->snum_per_us);

    if (high) {
        int ret = c_wait(di, CW_F(IR_CH), CW_OR, CW_SKIP(max_samples), CW_END);
        if (ret != SRD_OK)
            return -1;
    } else {
        int ret = c_wait(di, CW_R(IR_CH), CW_OR, CW_SKIP(max_samples), CW_END);
        if (ret != SRD_OK)
            return -1;
    }

    *pulse_es = di_samplenum(di);

    if (di_matched(di) & (1ULL << 1))
        return -2;

    if (!tolerance_check(s, pulse_ss, *pulse_es, time_us))
        return -2;

    return 0;
}

static int read_bit(struct srd_decoder_inst* di, sirc_state* s,
    uint64_t high_ss,
    int* bit_val, uint64_t* bit_ss, uint64_t* bit_es, int* good)
{
    uint64_t max_high_samples = (uint64_t)(2000.0 * s->snum_per_us);

    if (s->active) {
        int ret = c_wait(di, CW_F(IR_CH), CW_END);
        if (ret != SRD_OK)
            return -1;
    } else {
        int ret = c_wait(di, CW_R(IR_CH), CW_OR, CW_SKIP(max_high_samples), CW_END);
        if (ret != SRD_OK)
            return -1;
    }

    uint64_t high_es = di_samplenum(di);

    if (di_matched(di) & (1ULL << 1))
        return -2;

    if (tolerance_check(s, high_ss, high_es, ONE_USEC)) {
        *bit_val = 1;
    } else if (tolerance_check(s, high_ss, high_es, ZERO_USEC)) {
        *bit_val = 0;
    } else {
        return -2;
    }

    uint64_t low_es;
    int pause_ret = read_pulse(di, s, !s->active, PAUSE_USEC, high_es, &low_es);
    if (pause_ret == 0) {
        *good = 1;
        *bit_ss = high_ss;
        *bit_es = low_es;
    } else if (pause_ret == -2) {
        *good = 0;
        *bit_ss = high_ss;
        *bit_es = high_es + (uint64_t)(PAUSE_USEC * s->snum_per_us);
    } else {
        return -1;
    }

    {
        char bit_str[16];
        snprintf(bit_str, sizeof(bit_str), "%d", *bit_val);
        c_put(di, *bit_ss, *bit_es, s->out_ann, ANN_BIT, bit_str);
    }

    return 0;
}

static void sirc_metadata(struct srd_decoder_inst* di, int key, uint64_t value)
{
    sirc_state* s = (sirc_state*)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        s->snum_per_us = (double)value / 1e6;
    }
}

static void sirc_reset(struct srd_decoder_inst* di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(sirc_state)));
    }
    sirc_state* s = (sirc_state*)c_decoder_get_private(di);
    memset(s, 0, sizeof(sirc_state));
}

static void sirc_start(struct srd_decoder_inst* di)
{
    sirc_state* s = (sirc_state*)c_decoder_get_private(di);

    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ir_sirc");

    const char* polarity = c_opt_str(di, "polarity", "active-low");
    if (polarity && strcmp(polarity, "active-high") == 0)
        s->active = 1;
    else
        s->active = 0;

    s->samplerate = c_samplerate(di);
    if (s->samplerate > 0)
        s->snum_per_us = (double)s->samplerate / 1e6;
}

static void sirc_decode(struct srd_decoder_inst* di)
{
    sirc_state* s = (sirc_state*)c_decoder_get_private(di);
    if (!s->samplerate) {
        s->samplerate = c_samplerate(di);
        if (s->samplerate > 0)
            s->snum_per_us = (double)s->samplerate / 1e6;
    }
    if (s->samplerate == 0)
        return;

    while (1) {
        int ret;

        if (s->active)
            ret = c_wait(di, CW_H(IR_CH), CW_END);
        else
            ret = c_wait(di, CW_L(IR_CH), CW_END);
        if (ret != SRD_OK)
            return;

        uint64_t frame_ss = di_samplenum(di);

        uint64_t agc_ss = di_samplenum(di);
        uint64_t agc_es;
        ret = read_pulse(di, s, s->active, AGC_USEC, agc_ss, &agc_es);
        if (ret != 0)
            continue;

        uint64_t pause_ss = agc_es;
        uint64_t pause_es;
        ret = read_pulse(di, s, !s->active, PAUSE_USEC, pause_ss, &pause_es);
        if (ret != 0)
            continue;

        c_put(di, agc_ss, agc_es, s->out_ann, ANN_AGC, "AGC", "A");
        c_put(di, pause_ss, pause_es, s->out_ann, ANN_PAUSE, "Pause", "P");
        c_put(di, agc_ss, pause_es, s->out_ann, ANN_START, "Start", "S");

        uint8_t bits[21];
        uint64_t bit_ss_arr[21];
        uint64_t bit_es_arr[21];
        int bit_count = 0;
        int error = 0;
        uint64_t next_bit_ss = pause_es;

        while (bit_count <= 20) {
            int bval = 0;
            uint64_t bss = 0, bes = 0;
            int good = 0;

            ret = read_bit(di, s, next_bit_ss, &bval, &bss, &bes, &good);
            if (ret == -1)
                return;
            if (ret == -2) {
                /* Last bit — pause detection failed but bit value is valid.
                 * Match Python: the last bit doesn't need a following pause. */
                bits[bit_count] = bval;
                bit_ss_arr[bit_count] = bss;
                bit_es_arr[bit_count] = bes;
                bit_count++;
                break;
            }

            bits[bit_count] = bval;
            bit_ss_arr[bit_count] = bss;
            bit_es_arr[bit_count] = bes;
            bit_count++;

            if (!good)
                break;

            next_bit_ss = bes;
        }

        if (error || bit_count > 20) {
            ret = c_wait(di, CW_SKIP(0), CW_END);
            c_put(di, frame_ss, di_samplenum(di), s->out_ann, ANN_WARN,
                "Error: too many bits", "Error", "E");
            continue;
        }

        uint8_t* command_bits;
        uint8_t* address_bits;
        uint8_t* extended_bits;
        int command_count;
        int address_count;
        int extended_count;

        if (bit_count == 12) {
            command_bits = bits;
            command_count = 7;
            address_bits = bits + 7;
            address_count = 5;
            extended_bits = NULL;
            extended_count = 0;
        } else if (bit_count == 15) {
            command_bits = bits;
            command_count = 7;
            address_bits = bits + 7;
            address_count = 8;
            extended_bits = NULL;
            extended_count = 0;
        } else if (bit_count == 20) {
            command_bits = bits;
            command_count = 7;
            address_bits = bits + 7;
            address_count = 5;
            extended_bits = bits + 12;
            extended_count = 8;
        } else {
            char err_str[64];
            snprintf(err_str, sizeof(err_str), "Error: incorrect bits count %d", bit_count);
            ret = c_wait(di, CW_SKIP(0), CW_END);
            c_put(di, frame_ss, di_samplenum(di), s->out_ann, ANN_WARN, err_str, "Error", "E");
            continue;
        }

        uint8_t command_num = (uint8_t)bitpack_lsb(command_bits, command_count);
        uint16_t address_num = bitpack_lsb(address_bits, address_count);

        {
            char cmd_long[32], cmd_mid[16];
            snprintf(cmd_long, sizeof(cmd_long), "Command: 0x%02X", command_num);
            snprintf(cmd_mid, sizeof(cmd_mid), "C:0x%02X", command_num);
            c_put(di, bit_ss_arr[0], bit_es_arr[command_count - 1],
                s->out_ann, ANN_CMD, cmd_long, cmd_mid);
        }

        {
            char addr_long[32], addr_mid[16];
            int addr_hex_width = (address_count + 3) / 4;
            snprintf(addr_long, sizeof(addr_long), "Address: 0x%0*X", addr_hex_width, address_num);
            snprintf(addr_mid, sizeof(addr_mid), "A:0x%0*X", addr_hex_width, address_num);
            c_put(di, bit_ss_arr[command_count], bit_es_arr[command_count + address_count - 1],
                s->out_ann, ANN_ADDR, addr_long, addr_mid);
        }

        if (extended_count > 0 && extended_bits) {
            uint16_t extended_num = bitpack_lsb(extended_bits, extended_count);
            char ext_long[32], ext_mid[16];
            int ext_hex_width = (extended_count + 3) / 4;
            snprintf(ext_long, sizeof(ext_long), "Extended: 0x%0*X", ext_hex_width, extended_num);
            snprintf(ext_mid, sizeof(ext_mid), "E:0x%0*X", ext_hex_width, extended_num);
            c_put(di, bit_ss_arr[command_count + address_count],
                bit_es_arr[command_count + address_count + extended_count - 1],
                s->out_ann, ANN_EXT, ext_long, ext_mid);
        }

        {
            char remote_long[128], remote_mid[64];
            const sirc_device_entry* dev = find_device(address_num, extended_count > 0, extended_count > 0 ? bitpack_lsb(extended_bits, extended_count) : 0);
            if (dev) {
                const char* key = find_key_name(dev, command_num);
                const char* key_text = key ? key : "Unknown";
                snprintf(remote_long, sizeof(remote_long), "%s%s", dev->name_long, key_text);
                snprintf(remote_mid, sizeof(remote_mid), "%s%s", dev->name_short, key_text);
            } else {
                /* Match Python: unknown device outputs "Unknown Device: Unknown" / "UNK: Unknown" */
                snprintf(remote_long, sizeof(remote_long), "Unknown Device: Unknown");
                snprintf(remote_mid, sizeof(remote_mid), "UNK: Unknown");
            }
            c_put(di, frame_ss, bit_es_arr[bit_count - 1],
                s->out_ann, ANN_REMOTE, remote_long, remote_mid);
        }
    }
}

static void sirc_destroy(struct srd_decoder_inst* di)
{
    void* priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ir_sirc_c_decoder = {
    .id = "ir_sirc_c",
    .name = "IR SIRC(C)",
    .longname = "Sony IR (SIRC) (C)",
    .desc = "Sony infrared remote control protocol (SIRC). (C implementation)",
    .license = "gplv2+",
    .channels = sirc_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = sirc_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = sirc_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = sirc_ann_rows,
    .inputs = sirc_inputs,
    .num_inputs = 1,
    .outputs = sirc_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = sirc_tags,
    .num_tags = 1,
    .metadata = sirc_metadata,
    .reset = sirc_reset,
    .start = sirc_start,
    .decode = sirc_decode,
    .destroy = sirc_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder* srd_c_decoder_entry(void)
{
    GVariant* polarity_vals[] = {
        g_variant_new_string("active-low"),
        g_variant_new_string("active-high"),
    };
    GSList* polarity_list = NULL;
    polarity_list = g_slist_append(polarity_list, polarity_vals[0]);
    polarity_list = g_slist_append(polarity_list, polarity_vals[1]);
    sirc_options[0].id = "polarity";
    sirc_options[0].idn = NULL;
    sirc_options[0].desc = "Polarity";
    sirc_options[0].def = g_variant_new_string("active-low");
    sirc_options[0].values = polarity_list;

    return &ir_sirc_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}