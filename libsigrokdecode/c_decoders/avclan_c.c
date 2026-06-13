/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2023 Maciej Grela <enki@fsck.pl>
 * Copyright (C) 2024 C port
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* Annotations */
enum {
    ANN_ADDRESS = 0,
    ANN_FUNCTION,
    ANN_CTRL_OPCODE,
    ANN_SEQUENCE_NO,
    ANN_ADVERTISED_FUNC,
    ANN_CMD_OPCODE,
    ANN_CD_OPCODE,
    ANN_CD_STATE,
    ANN_CD_FLAGS,
    ANN_DISC_NUMBER,
    ANN_TRACK_NUMBER,
    ANN_TRACK_COUNT,
    ANN_DISC_TITLE,
    ANN_TRACK_TITLE,
    ANN_PLAYBACK_TIME,
    ANN_DISC_SLOTS,
    ANN_AUDIO_OPCODE,
    ANN_AUDIO_FLAGS,
    ANN_VOLUME,
    ANN_BASS,
    ANN_TREBLE,
    ANN_FADE,
    ANN_BALANCE,
    ANN_RADIO_OPCODE,
    ANN_RADIO_STATE,
    ANN_RADIO_MODE,
    ANN_RADIO_FLAGS,
    ANN_BAND,
    ANN_CHANNEL,
    ANN_FREQ,
    ANN_WARNING,
    NUM_ANN,
};

/* State machine */
enum avclan_state {
    AVC_IDLE,
    AVC_MASTER_ADDRESS,
    AVC_SLAVE_ADDRESS,
    AVC_CONTROL,
    AVC_DATA_LENGTH,
    AVC_DATA,
};

/* Lookup table entry */
typedef struct {
    int value;
    const char *name;
} name_entry;

/* Data byte with sample positions */
typedef struct {
    uint8_t b;
    uint64_t ss;
    uint64_t es;
} data_byte_t;

/* Decoder private state */
typedef struct {
    enum avclan_state state;
    int out_ann;
    uint64_t ss;
    uint64_t es;

    int broadcast_bit;
    uint16_t master_addr;
    uint16_t slave_addr;
    uint8_t control;
    uint8_t data_length;

    data_byte_t data_bytes[256];
    int num_data_bytes;

    int from_function;
    int to_function;
} avclan_state;

/* --- Lookup Tables --- */

static const name_entry hw_addresses[] = {
    {0x110, "EMV"},       {0x120, "AVX"},       {0x128, "DIN1_TV"},
    {0x140, "AVN"},       {0x144, "G_BOOK"},    {0x160, "AUDIO_HU1"},
    {0x178, "NAVI"},      {0x17C, "MONET"},     {0x17D, "TEL"},
    {0x180, "Rr_TV"},     {0x190, "AUDIO_HU2"}, {0x1A0, "DVD_P"},
    {0x1D6, "CLOCK"},     {0x1AC, "CAMERA_C"},  {0x1C0, "Rr_CONT"},
    {0x1C2, "TV_TUNER2"}, {0x1C4, "PANEL"},     {0x1C6, "GW"},
    {0x1C8, "FM_M_LCD"},  {0x1CC, "ST_WHEEL_CTRL"}, {0x1D8, "GW_TRIP"},
    {0x1EC, "BODY"},      {0x1F0, "RADIO_TUNER"}, {0x1F1, "XM"},
    {0x1F2, "SIRIUS"},    {0x1F4, "RSA"},       {0x1F6, "RSE"},
    {0x1FF, "GROUP_AUDIO"}, {0x230, "TV_TUNER"}, {0x240, "CD_CH2"},
    {0x250, "DVD_CH"},    {0x280, "CAMERA"},    {0x360, "CD_CH1"},
    {0x3A0, "MD_CH"},     {0x440, "DSP_AMP"},   {0x480, "AMP"},
    {0x530, "ETC"},       {0x5C8, "MAYDAY"},    {0xFFF, "BROADCAST"},
};
#define HW_ADDR_COUNT (sizeof(hw_addresses) / sizeof(hw_addresses[0]))

static const name_entry function_ids[] = {
    {0x01, "COMM_CTRL"},       {0x12, "COMMUNICATION"},   {0x21, "SW"},
    {0x23, "SW_NAME"},         {0x24, "SW_CONVERTING"},   {0x25, "CMD_SW"},
    {0x28, "BEEP_HU"},         {0x29, "BEEP_SPEAKERS"},   {0x34, "FRONT_PSNG_MONITOR"},
    {0x43, "CD_CHANGER2"},     {0x55, "BLUETOOTH_TEL"},   {0x56, "INFO_DRAWING"},
    {0x58, "NAV_ECU"},         {0x5C, "CAMERA"},          {0x5D, "CLIMATE_DRAWING"},
    {0x5E, "AUDIO_DRAWING"},   {0x5F, "TRIP_INFO_DRAWING"}, {0x60, "TUNER"},
    {0x61, "TAPE_DECK"},       {0x62, "CD"},              {0x63, "CD_CHANGER"},
    {0x74, "AUDIO_AMP"},       {0x80, "GPS"},             {0x85, "VOICE_CTRL"},
    {0xE0, "CLIMATE_CTRL_DEV"}, {0xE5, "TRIP_INFO"},
};
#define FUNC_ID_COUNT (sizeof(function_ids) / sizeof(function_ids[0]))

static const name_entry comm_ctrl_opcodes[] = {
    {0x00, "LIST_FUNCTIONS_REQ"},   {0x10, "LIST_FUNCTIONS_RESP"},
    {0x01, "RESTART_LAN"},          {0x08, "LANCHECK_END_REQ"},
    {0x18, "LANCHECK_END_RESP"},    {0x0a, "LANCHECK_SCAN_REQ"},
    {0x1a, "LANCHECK_SCAN_RESP"},   {0x0c, "LANCHECK_REQ"},
    {0x1c, "LANCHECK_RESP"},        {0x20, "PING_REQ"},
    {0x30, "PING_RESP"},            {0x43, "DISABLE_FUNCTION_REQ"},
    {0x53, "DISABLE_FUNCTION_RESP"},{0x42, "ENABLE_FUNCTION_REQ"},
    {0x52, "ENABLE_FUNCTION_RESP"}, {0x45, "ADVERTISE_FUNCTION"},
    {0x46, "GENERAL_QUERY"},
};
#define COMM_CTRL_OPCODE_COUNT (sizeof(comm_ctrl_opcodes) / sizeof(comm_ctrl_opcodes[0]))

static const name_entry cd_opcodes[] = {
    {0x50, "INSERTED_CD"},   {0x51, "REMOVED_CD"},
    {0xe2, "REQUEST_PLAYBACK2"}, {0xe4, "REQUEST_LOADER2"},
    {0xed, "REQUEST_TRACK_NAME"},
    {0xf1, "REPORT_PLAYBACK"},   {0xf2, "REPORT_PLAYBACK2"},
    {0xf3, "REPORT_LOADER"},     {0xf4, "REPORT_LOADER2"},
    {0xf9, "REPORT_TOC"},        {0xfd, "REPORT_TRACK_NAME"},
};
#define CD_OPCODE_COUNT (sizeof(cd_opcodes) / sizeof(cd_opcodes[0]))

static const name_entry cmd_sw_opcodes[] = {
    {0x80, "EJECT"},         {0x90, "DISC_UP"},       {0x91, "DISC_DOWN"},
    {0x9c, "PWRVOL_KNOB_RIGHTHAND_TURN"}, {0x9d, "PWRVOL_KNOB_LEFTHAND_TURN"},
    {0x94, "TRACK_SEEK_UP"}, {0x95, "TRACK_SEEK_DOWN"},
    {0xa6, "CD_ENABLE_SCAN"},  {0xa7, "CD_DISABLE_SCAN"},
    {0xa0, "CD_ENABLE_REPEAT"}, {0xa1, "CD_DISABLE_REPEAT"},
    {0xb0, "CD_ENABLE_RANDOM"}, {0xb1, "CD_DISABLE_RANDOM"},
};
#define CMD_SW_OPCODE_COUNT (sizeof(cmd_sw_opcodes) / sizeof(cmd_sw_opcodes[0]))

static const name_entry audio_amp_opcodes[] = {
    {0xf1, "REPORT"},
};
#define AUDIO_AMP_OPCODE_COUNT (sizeof(audio_amp_opcodes) / sizeof(audio_amp_opcodes[0]))

static const name_entry tuner_opcodes[] = {
    {0xf1, "REPORT"},
};
#define TUNER_OPCODE_COUNT (sizeof(tuner_opcodes) / sizeof(tuner_opcodes[0]))

static const name_entry tuner_states[] = {
    {0x01, "ON"},  {0x00, "OFF"},
};
#define TUNER_STATE_COUNT (sizeof(tuner_states) / sizeof(tuner_states[0]))

static const name_entry tuner_modes[] = {
    {0x27, "MANUAL"}, {0x0a, "AST_SEARCH"},
    {0x07, "SCAN_DOWN"}, {0x06, "SCAN_UP"},
    {0x01, "READY"},  {0x00, "OFF"},
};
#define TUNER_MODE_COUNT (sizeof(tuner_modes) / sizeof(tuner_modes[0]))

/* CD state codes (bitmask) */
static const name_entry cd_state_bits[] = {
    {0x01, "OPEN"},     {0x02, "ERR1"},    {0x04, "BIT2"},
    {0x08, "SEEKING"},  {0x10, "PLAYBACK"}, {0x20, "SEEKING_TRACK"},
    {0x40, "BIT6"},     {0x80, "LOADING"},
};

/* CD flags (bitmask) */
static const name_entry cd_flag_bits[] = {
    {0x01, "BIT0"},       {0x02, "DISK_RANDOM"}, {0x04, "RANDOM"},
    {0x08, "DISK_REPEAT"}, {0x10, "REPEAT"},     {0x20, "DISK_SCAN"},
    {0x40, "SCAN"},       {0x80, "BIT7"},
};

/* CD slots (bitmask) */
static const name_entry cd_slot_bits[] = {
    {0x01, "SLOT1"}, {0x02, "SLOT2"}, {0x04, "SLOT3"},
    {0x08, "SLOT4"}, {0x10, "SLOT5"}, {0x20, "SLOT6"},
};

/* Audio amp flags (bitmask) */
static const name_entry audio_amp_flag_bits[] = {
    {0x01, "BIT0"}, {0x02, "BIT1"}, {0x04, "MUTE"},
    {0x08, "BIT3"}, {0x10, "BIT4"}, {0x20, "BIT5"},
    {0x40, "BIT6"}, {0x80, "BIT7"},
};

/* Tuner flags (bitmask) */
static const name_entry tuner_flag_bits[] = {
    {0x01, "BIT0"}, {0x02, "BIT1"}, {0x04, "TP"},
    {0x08, "TA"},   {0x10, "REG"},  {0x20, "BIT5"},
    {0x40, "AF"},   {0x80, "BIT7"},
};

/* --- Helper functions --- */

static const char *find_name(const name_entry *table, int count, int value)
{
    for (int i = 0; i < count; i++)
        if (table[i].value == value) return table[i].name;
    return NULL;
}

static void format_bitmask(const name_entry *bits, int bit_count, uint8_t val, char *buf, int bufsize)
{
    int pos = 0;
    int first = 1;
    buf[0] = '\0';
    for (int i = 0; i < bit_count; i++) {
        if (val & bits[i].value) {
            if (!first) pos += snprintf(buf + pos, bufsize - pos, "|");
            pos += snprintf(buf + pos, bufsize - pos, "%s", bits[i].name);
            first = 0;
        }
    }
    if (first)
        snprintf(buf, bufsize, "0x%02X", val);
}

static int bcd2dec(int b)
{
    return ((b >> 4) & 0x0F) * 10 + (b & 0x0F);
}

static void map_left_right(int value, int center, const char *neg_tag, const char *pos_tag, char *buf, int bufsize)
{
    value -= center;
    if (value < 0)
        snprintf(buf, bufsize, "%s%d", neg_tag, -value);
    else if (value > 0)
        snprintf(buf, bufsize, "%s%d", pos_tag, value);
    else
        snprintf(buf, bufsize, "0");
}

/* --- Decoder metadata --- */

static const char *avclan_inputs[] = {"iebus", NULL};
static const char *avclan_tags[] = {"Automotive", NULL};

static const char *avclan_ann_labels[][3] = {
    {"", "address", "Device Address"},
    {"", "function", "Function"},
    {"", "ctrl-opcode", "Opcode"},
    {"", "sequence-no", "Sequence No."},
    {"", "advertised-function", "Function"},
    {"", "cmd-opcode", "Opcode"},
    {"", "cd-opcode", "Opcode"},
    {"", "cd-state", "State"},
    {"", "cd-flags", "Flags"},
    {"", "disc-number", "Disc Number"},
    {"", "track-number", "Track Number"},
    {"", "track-count", "Track Count"},
    {"", "disc-title", "Disc Name"},
    {"", "track-title", "Track Name"},
    {"", "playback-time", "Playback time"},
    {"", "disc-slots", "Disc Slots"},
    {"", "audio-opcode", "Opcode"},
    {"", "audio-flags", "Audio Flags"},
    {"", "volume", "Volume"},
    {"", "bass", "Bass"},
    {"", "treble", "Treble"},
    {"", "fade", "Fade"},
    {"", "balance", "Balance"},
    {"", "radio-opcode", "Opcode"},
    {"", "radio-state", "State"},
    {"", "radio-mode", "Mode"},
    {"", "radio-flags", "Flags"},
    {"", "band", "Band"},
    {"", "channel", "Channel"},
    {"", "freq", "Frequency"},
    {"", "warning", "Warning"},
};

static const int row_devices_classes[] = {ANN_ADDRESS, ANN_FUNCTION, -1};
static const int row_control_classes[] = {ANN_CTRL_OPCODE, ANN_SEQUENCE_NO, ANN_ADVERTISED_FUNC, -1};
static const int row_cmd_classes[] = {ANN_CMD_OPCODE, -1};
static const int row_cd_classes[] = {ANN_CD_OPCODE, ANN_CD_STATE, ANN_CD_FLAGS, ANN_DISC_NUMBER,
                                      ANN_TRACK_NUMBER, ANN_TRACK_COUNT, ANN_DISC_TITLE,
                                      ANN_TRACK_TITLE, ANN_PLAYBACK_TIME, ANN_DISC_SLOTS, -1};
static const int row_audio_classes[] = {ANN_AUDIO_OPCODE, ANN_AUDIO_FLAGS, ANN_VOLUME,
                                         ANN_BASS, ANN_TREBLE, ANN_FADE, ANN_BALANCE, -1};
static const int row_radio_classes[] = {ANN_RADIO_OPCODE, ANN_RADIO_STATE, ANN_RADIO_MODE,
                                         ANN_RADIO_FLAGS, ANN_BAND, ANN_CHANNEL, ANN_FREQ, -1};
static const int row_warnings_classes[] = {ANN_WARNING, -1};

static const struct srd_c_ann_row avclan_ann_rows[] = {
    {"devices", "Device Addresses and Functions", row_devices_classes, 2},
    {"control", "Network Control", row_control_classes, 3},
    {"cmd", "HU Commands", row_cmd_classes, 1},
    {"cd", "CD Player", row_cd_classes, 10},
    {"audio", "Audio Amplifier", row_audio_classes, 7},
    {"radio", "Radio Tuner", row_radio_classes, 7},
    {"warnings", "Warnings", row_warnings_classes, 1},
};

/* --- Packet handler functions --- */

static int pkt_comm_ctrl(struct srd_decoder_inst *di, avclan_state *s)
{
    if (s->num_data_bytes < 1) return 0;

    uint8_t opcode_val = s->data_bytes[0].b;
    const char *opcode_name = find_name(comm_ctrl_opcodes, COMM_CTRL_OPCODE_COUNT, opcode_val);

    if (opcode_name) {
        char t[128];
        snprintf(t, sizeof(t), "Opcode: %s", opcode_name);
        c_put(di, s->data_bytes[0].ss, s->data_bytes[0].es, s->out_ann, ANN_CTRL_OPCODE, t, opcode_name);

        if (opcode_val == 0x45) { /* ADVERTISE_FUNCTION */
            if (s->num_data_bytes >= 2) {
                int logic_id = s->data_bytes[1].b;
                const char *lname = find_name(function_ids, FUNC_ID_COUNT, logic_id);
                char t2[256];
                if (lname)
                    snprintf(t2, sizeof(t2), "Function: %s", lname);
                else
                    snprintf(t2, sizeof(t2), "Function: 0x%02X", logic_id);
                c_put(di, s->data_bytes[1].ss, s->data_bytes[1].es, s->out_ann, ANN_ADVERTISED_FUNC, t2, lname ? lname : "Func");
            }
        } else if (opcode_val == 0x20 || opcode_val == 0x30) { /* PING_REQ / PING_RESP */
            if (s->num_data_bytes >= 2) {
                int seq = s->data_bytes[1].b;
                char t2[64];
                snprintf(t2, sizeof(t2), "Sequence Number: %d", seq);
                c_put(di, s->data_bytes[1].ss, s->data_bytes[1].es, s->out_ann, ANN_SEQUENCE_NO, t2);
            }
        } else if (opcode_val == 0x10) { /* LIST_FUNCTIONS_RESP */
            for (int idx = 1; idx < s->num_data_bytes; idx++) {
                int logical_addr = s->data_bytes[idx].b;
                const char *lname = find_name(function_ids, FUNC_ID_COUNT, logical_addr);
                char t2[256];
                if (lname)
                    snprintf(t2, sizeof(t2), "Function: %s", lname);
                else
                    snprintf(t2, sizeof(t2), "Function: 0x%02x", logical_addr);
                c_put(di, s->data_bytes[idx].ss, s->data_bytes[idx].es, s->out_ann, ANN_ADVERTISED_FUNC, t2, lname ? lname : "Func");
            }
        }
        return 1;
    }
    return 0;
}

static int pkt_from_25(struct srd_decoder_inst *di, avclan_state *s)
{
    if (s->num_data_bytes < 1) return 0;

    uint8_t opcode_val = s->data_bytes[0].b;
    const char *opcode_name = find_name(cmd_sw_opcodes, CMD_SW_OPCODE_COUNT, opcode_val);

    if (opcode_name) {
        char t[128];
        snprintf(t, sizeof(t), "Opcode: %s", opcode_name);
        c_put(di, s->data_bytes[0].ss, s->data_bytes[0].es, s->out_ann, ANN_CMD_OPCODE, t, opcode_name);
    }
    return 0;
}

static int pkt_from_60(struct srd_decoder_inst *di, avclan_state *s)
{
    if (s->num_data_bytes < 1) return 0;

    uint8_t opcode_val = s->data_bytes[0].b;
    const char *opcode_name = find_name(tuner_opcodes, TUNER_OPCODE_COUNT, opcode_val);

    if (opcode_name) {
        char t[128];
        snprintf(t, sizeof(t), "Opcode: %s", opcode_name);
        c_put(di, s->data_bytes[0].ss, s->data_bytes[0].es, s->out_ann, ANN_RADIO_OPCODE, t, opcode_name);

        if (opcode_val == 0xf1 && s->num_data_bytes >= 9) { /* REPORT */
            /* State */
            int tuner_state_val = s->data_bytes[1].b;
            const char *state_name = find_name(tuner_states, TUNER_STATE_COUNT, tuner_state_val);
            char t2[256];
            if (state_name)
                snprintf(t2, sizeof(t2), "State: %s", state_name);
            else
                snprintf(t2, sizeof(t2), "State: 0x%02X", tuner_state_val);
            c_put(di, s->data_bytes[1].ss, s->data_bytes[1].es, s->out_ann, ANN_RADIO_STATE, t2, state_name ? state_name : "State");

            /* Mode */
            int tuner_mode_val = s->data_bytes[2].b;
            const char *mode_name = find_name(tuner_modes, TUNER_MODE_COUNT, tuner_mode_val);
            if (mode_name)
                snprintf(t2, sizeof(t2), "Mode: %s", mode_name);
            else
                snprintf(t2, sizeof(t2), "Mode: 0x%02X", tuner_mode_val);
            c_put(di, s->data_bytes[2].ss, s->data_bytes[2].es, s->out_ann, ANN_RADIO_MODE, t2, mode_name ? mode_name : "Mode");

            /* Band */
            int band_type = s->data_bytes[3].b & 0xF0;
            int band_number = s->data_bytes[3].b & 0x0F;
            const char *band_str;
            double freq_start, freq_step;
            const char *freq_unit;
            if (band_type == 0x80) {
                band_str = "FM"; freq_start = 87.5; freq_step = 0.05; freq_unit = "MHz";
            } else if (band_type == 0xC0) {
                band_str = "AM"; freq_start = 153; freq_step = 1; freq_unit = "kHz";
            } else {
                band_str = "AM"; freq_start = 522; freq_step = 9; freq_unit = "kHz";
            }
            snprintf(t2, sizeof(t2), "Band: %s %d", band_str, band_number);
            c_put(di, s->data_bytes[3].ss, s->data_bytes[3].es, s->out_ann, ANN_BAND, t2, band_str, "Band");

            /* Frequency */
            int freq_raw = (s->data_bytes[4].b << 8) | s->data_bytes[5].b;
            double freq = freq_start + (freq_raw - 1) * freq_step;
            char t3[64];
            snprintf(t3, sizeof(t3), "Freq: %.2f %s", freq, freq_unit);
            snprintf(t2, sizeof(t2), "%.2f %s", freq, freq_unit);
            c_put(di, s->data_bytes[4].ss, s->data_bytes[5].es, s->out_ann, ANN_FREQ, t3, t2, "Freq");

            /* Channel */
            int channel = s->data_bytes[6].b;
            if (channel > 0) {
                snprintf(t2, sizeof(t2), "CH #%d", channel);
                c_put(di, s->data_bytes[6].ss, s->data_bytes[6].es, s->out_ann, ANN_CHANNEL, t2, "CH");
            }

            /* Flags */
            char flag_buf[128];
            format_bitmask(tuner_flag_bits, 8, s->data_bytes[7].b, flag_buf, sizeof(flag_buf));
            snprintf(t2, sizeof(t2), "Flags: %s", flag_buf);
            c_put(di, s->data_bytes[7].ss, s->data_bytes[7].es, s->out_ann, ANN_RADIO_FLAGS, t2, "Flags", "F");

            format_bitmask(tuner_flag_bits, 8, s->data_bytes[8].b, flag_buf, sizeof(flag_buf));
            snprintf(t2, sizeof(t2), "Flags: %s", flag_buf);
            c_put(di, s->data_bytes[8].ss, s->data_bytes[8].es, s->out_ann, ANN_RADIO_FLAGS, t2, "Flags", "F");
        }
        return 1;
    }
    return 0;
}

static int pkt_from_cd_player(struct srd_decoder_inst *di, avclan_state *s)
{
    if (s->num_data_bytes < 1) return 0;

    uint8_t opcode_val = s->data_bytes[0].b;
    const char *opcode_name = find_name(cd_opcodes, CD_OPCODE_COUNT, opcode_val);
    char opcode_anno[64];

    if (opcode_name)
        snprintf(opcode_anno, sizeof(opcode_anno), "%s", opcode_name);
    else
        snprintf(opcode_anno, sizeof(opcode_anno), "%02x", opcode_val);

    int ret = 0;

    if (opcode_name) {
        if ((opcode_val == 0xf1 || opcode_val == 0xf2) && s->num_data_bytes >= 8) {
            /* REPORT_PLAYBACK / REPORT_PLAYBACK2 */
            /* Skip data_bytes[1] (unknown) */
            int cd_state_val = s->data_bytes[2].b;
            char flag_buf[128];
            format_bitmask(cd_state_bits, 8, cd_state_val, flag_buf, sizeof(flag_buf));
            char t[256];
            snprintf(t, sizeof(t), "State: %s", flag_buf);
            c_put(di, s->data_bytes[2].ss, s->data_bytes[2].es, s->out_ann, ANN_CD_STATE, t, flag_buf, "State");

            int disc_number = s->data_bytes[3].b;
            if (disc_number != 0xff)
                snprintf(t, sizeof(t), "CD #%d", disc_number);
            else
                snprintf(t, sizeof(t), "CD #");
            c_put(di, s->data_bytes[3].ss, s->data_bytes[3].es, s->out_ann, ANN_DISC_NUMBER, t, "CD", "C");

            int track_number = s->data_bytes[4].b;
            if (track_number != 0xff)
                snprintf(t, sizeof(t), "Track #%d", track_number);
            else
                snprintf(t, sizeof(t), "Track #");
            c_put(di, s->data_bytes[4].ss, s->data_bytes[4].es, s->out_ann, ANN_TRACK_NUMBER, t, "Tra", "T");

            int minutes = s->data_bytes[5].b;
            int seconds = s->data_bytes[6].b;
            if (minutes != 0xff && seconds != 0xff) {
                minutes = bcd2dec(minutes);
                seconds = bcd2dec(seconds);
                snprintf(t, sizeof(t), "Time: %02d:%02d", minutes, seconds);
                char t2[16];
                snprintf(t2, sizeof(t2), "%02d:%02d", minutes, seconds);
                c_put(di, s->data_bytes[5].ss, s->data_bytes[6].es, s->out_ann, ANN_PLAYBACK_TIME, t, t2, "T");
            } else {
                c_put(di, s->data_bytes[5].ss, s->data_bytes[6].es, s->out_ann, ANN_PLAYBACK_TIME, "Time", "T");
            }

            int cd_flags_val = s->data_bytes[7].b;
            format_bitmask(cd_flag_bits, 8, cd_flags_val, flag_buf, sizeof(flag_buf));
            snprintf(t, sizeof(t), "Flags: %s", flag_buf);
            c_put(di, s->data_bytes[7].ss, s->data_bytes[7].es, s->out_ann, ANN_CD_FLAGS, t, flag_buf, "Flags");

        } else if (opcode_val == 0xfd && s->num_data_bytes >= 6) {
            /* REPORT_TRACK_NAME */
            int disc_number = s->data_bytes[1].b;
            int track_number = s->data_bytes[2].b;
            char t[64];
            if (disc_number != 0xff) {
                snprintf(t, sizeof(t), "CD #%d", disc_number);
                c_put(di, s->data_bytes[1].ss, s->data_bytes[1].es, s->out_ann, ANN_DISC_NUMBER, t, "CD #");
            }
            snprintf(t, sizeof(t), "Track #%d", track_number);
            c_put(di, s->data_bytes[2].ss, s->data_bytes[2].es, s->out_ann, ANN_TRACK_NUMBER, t, "Track #");

            /* Text from index 5 onward */
            if (s->num_data_bytes > 5) {
                char text[256] = {0};
                int tlen = 0;
                for (int i = 5; i < s->num_data_bytes && tlen < (int)sizeof(text) - 1; i++)
                    text[tlen++] = (char)s->data_bytes[i].b;
                text[tlen] = '\0';
                char t2[300];
                snprintf(t2, sizeof(t2), "Title: %s", text);
                c_put(di, s->data_bytes[5].ss, s->data_bytes[s->num_data_bytes - 1].es,
                          s->out_ann, ANN_TRACK_TITLE, t2, "Title");
            }

        } else if ((opcode_val == 0xf3 || opcode_val == 0xf4) && s->num_data_bytes >= 7) {
            /* REPORT_LOADER / REPORT_LOADER2 */
            char flag_buf[128];
            int slots_val = s->data_bytes[2].b;
            format_bitmask(cd_slot_bits, 6, slots_val, flag_buf, sizeof(flag_buf));
            char t[256];
            snprintf(t, sizeof(t), "Available: %s", flag_buf);
            c_put(di, s->data_bytes[2].ss, s->data_bytes[2].es, s->out_ann, ANN_DISC_SLOTS, t, flag_buf, "Avail");

            slots_val = s->data_bytes[4].b;
            format_bitmask(cd_slot_bits, 6, slots_val, flag_buf, sizeof(flag_buf));
            snprintf(t, sizeof(t), "Disc Present: %s", flag_buf);
            c_put(di, s->data_bytes[4].ss, s->data_bytes[4].es, s->out_ann, ANN_DISC_SLOTS, t, flag_buf, "Pres");

            slots_val = s->data_bytes[6].b;
            format_bitmask(cd_slot_bits, 6, slots_val, flag_buf, sizeof(flag_buf));
            snprintf(t, sizeof(t), "Slot-3: %s", flag_buf);
            c_put(di, s->data_bytes[6].ss, s->data_bytes[6].es, s->out_ann, ANN_DISC_SLOTS, t, flag_buf, "Pres");

        } else if (opcode_val == 0xf9 && s->num_data_bytes >= 6) {
            /* REPORT_TOC */
            int disc_number = s->data_bytes[1].b;
            char t[64];
            if (disc_number != 0xff)
                snprintf(t, sizeof(t), "CD #%d", disc_number);
            else
                snprintf(t, sizeof(t), "CD #");
            c_put(di, s->data_bytes[1].ss, s->data_bytes[1].es, s->out_ann, ANN_DISC_NUMBER, t, "CD", "C");

            int track_number = s->data_bytes[2].b;
            if (track_number != 0xff)
                snprintf(t, sizeof(t), "Track #%d", track_number);
            else
                snprintf(t, sizeof(t), "Track #");
            c_put(di, s->data_bytes[2].ss, s->data_bytes[2].es, s->out_ann, ANN_TRACK_NUMBER, t, "Tra", "T");

            int track_count = s->data_bytes[3].b;
            if (track_count != 0xff)
                snprintf(t, sizeof(t), "Track Count: %d", track_count);
            else
                snprintf(t, sizeof(t), "Track Count");
            c_put(di, s->data_bytes[3].ss, s->data_bytes[3].es, s->out_ann, ANN_TRACK_COUNT, t, "Count", "Cnt");

            int minutes = s->data_bytes[4].b;
            int seconds = s->data_bytes[5].b;
            if (minutes != 0xff && seconds != 0xff) {
                minutes = bcd2dec(minutes);
                seconds = bcd2dec(seconds);
                snprintf(t, sizeof(t), "Total Time: %02d:%02d", minutes, seconds);
                char t2[16];
                snprintf(t2, sizeof(t2), "%02d:%02d", minutes, seconds);
                c_put(di, s->data_bytes[4].ss, s->data_bytes[5].es, s->out_ann, ANN_PLAYBACK_TIME, t, t2, "T");
            } else {
                c_put(di, s->data_bytes[4].ss, s->data_bytes[5].es, s->out_ann, ANN_PLAYBACK_TIME, "Total Time", "T");
            }
        }
        ret = 1;
    }

    /* Always output opcode annotation */
    char t[256];
    snprintf(t, sizeof(t), "Opcode: %s", opcode_anno);
    c_put(di, s->data_bytes[0].ss, s->data_bytes[0].es, s->out_ann, ANN_CD_OPCODE, t, opcode_anno, "Opcode");

    return ret;
}

static int pkt_to_cd_player(struct srd_decoder_inst *di, avclan_state *s)
{
    if (s->num_data_bytes < 1) return 0;

    uint8_t opcode_val = s->data_bytes[0].b;
    const char *opcode_name = find_name(cd_opcodes, CD_OPCODE_COUNT, opcode_val);
    char opcode_anno[64];

    if (opcode_name)
        snprintf(opcode_anno, sizeof(opcode_anno), "%s", opcode_name);
    else
        snprintf(opcode_anno, sizeof(opcode_anno), "%02x", opcode_val);

    int ret = 0;

    if (opcode_name && opcode_val == 0xed && s->num_data_bytes >= 3) {
        /* REQUEST_TRACK_NAME */
        int disc_number = s->data_bytes[1].b;
        char t[64];
        if (disc_number != 0xff)
            snprintf(t, sizeof(t), "CD #%d", disc_number);
        else
            snprintf(t, sizeof(t), "CD #");
        c_put(di, s->data_bytes[1].ss, s->data_bytes[1].es, s->out_ann, ANN_DISC_NUMBER, t, "CD", "C");

        int track_number = s->data_bytes[2].b;
        if (track_number != 0xff)
            snprintf(t, sizeof(t), "Track #%d", track_number);
        else
            snprintf(t, sizeof(t), "Track #");
        c_put(di, s->data_bytes[2].ss, s->data_bytes[2].es, s->out_ann, ANN_TRACK_NUMBER, t, "Tra", "T");

        ret = 1;
    }

    char t[128];
    snprintf(t, sizeof(t), "Opcode: %s", opcode_anno);
    c_put(di, s->data_bytes[0].ss, s->data_bytes[0].es, s->out_ann, ANN_CD_OPCODE, t, opcode_anno, "Opcode");

    return ret;
}

static int pkt_74(struct srd_decoder_inst *di, avclan_state *s)
{
    if (s->num_data_bytes < 1) return 0;

    uint8_t opcode_val = s->data_bytes[0].b;
    const char *opcode_name = find_name(audio_amp_opcodes, AUDIO_AMP_OPCODE_COUNT, opcode_val);

    if (opcode_name) {
        char t[256];
        snprintf(t, sizeof(t), "Opcode: %s", opcode_name);
        c_put(di, s->data_bytes[0].ss, s->data_bytes[0].es, s->out_ann, ANN_AUDIO_OPCODE, t, opcode_name);

        if (opcode_val == 0xf1 && s->num_data_bytes >= 13) { /* REPORT */
            /* Volume at index 2 */
            int volume = s->data_bytes[2].b;
            snprintf(t, sizeof(t), "Volume: %d", volume);
            c_put(di, s->data_bytes[2].ss, s->data_bytes[2].es, s->out_ann, ANN_VOLUME, t, "Volume", "Vol");

            /* Balance at index 3 */
            char lr[16];
            map_left_right(s->data_bytes[3].b, 0x10, "L", "R", lr, sizeof(lr));
            snprintf(t, sizeof(t), "Balance: %s", lr);
            c_put(di, s->data_bytes[3].ss, s->data_bytes[3].es, s->out_ann, ANN_BALANCE, t, "Balance", "Bal");

            /* Fade at index 4 */
            map_left_right(s->data_bytes[4].b, 0x10, "F", "R", lr, sizeof(lr));
            snprintf(t, sizeof(t), "Fade: %s", lr);
            c_put(di, s->data_bytes[4].ss, s->data_bytes[4].es, s->out_ann, ANN_FADE, t, "Fade");

            /* Bass at index 5 */
            map_left_right(s->data_bytes[5].b, 0x10, "-", "+", lr, sizeof(lr));
            snprintf(t, sizeof(t), "Bass: %s", lr);
            c_put(di, s->data_bytes[5].ss, s->data_bytes[5].es, s->out_ann, ANN_BASS, t, "Bass");

            /* Treble at index 7 */
            map_left_right(s->data_bytes[7].b, 0x10, "-", "+", lr, sizeof(lr));
            snprintf(t, sizeof(t), "Treble: %s", lr);
            c_put(di, s->data_bytes[7].ss, s->data_bytes[7].es, s->out_ann, ANN_TREBLE, t, "Treble");

            /* Flags at index 12 */
            char flag_buf[128];
            format_bitmask(audio_amp_flag_bits, 8, s->data_bytes[12].b, flag_buf, sizeof(flag_buf));
            snprintf(t, sizeof(t), "Flags: %s", flag_buf);
            c_put(di, s->data_bytes[12].ss, s->data_bytes[12].es, s->out_ann, ANN_AUDIO_FLAGS, t, "Flags");
        }
        return 1;
    }
    return 0;
}

/* --- Dispatch mechanism --- */

typedef int (*pkt_handler_fn)(struct srd_decoder_inst *, avclan_state *);

typedef struct {
    int from_func;   /* -1 = wildcard */
    int to_func;     /* -1 = wildcard */
    pkt_handler_fn handler;
} pkt_dispatch_entry;

static const pkt_dispatch_entry dispatch_table[] = {
    /* from_to specific */
    {0x01, 0x12, pkt_comm_ctrl},
    {0x12, 0x01, pkt_comm_ctrl},
    /* to only */
    {-1, 0x01, pkt_comm_ctrl},
    {-1, 0x12, pkt_comm_ctrl},
    /* from only */
    {0x01, -1, pkt_comm_ctrl},
    {0x12, -1, pkt_comm_ctrl},
    {0x25, -1, pkt_from_25},
    {0x60, -1, pkt_from_60},
    {0x62, -1, pkt_from_cd_player},
    {-1, 0x62, pkt_to_cd_player},
    {0x63, -1, pkt_from_cd_player},
    {0x43, -1, pkt_from_cd_player},
    /* both */
    {0x74, -1, pkt_74},
    {-1, 0x74, pkt_74},
};
#define DISPATCH_COUNT (sizeof(dispatch_table) / sizeof(dispatch_table[0]))

static void dispatch_packet(struct srd_decoder_inst *di, avclan_state *s)
{
    int from = s->from_function;
    int to = s->to_function;

    /* Priority: from_to > to_only > from_only > both_wildcard */
    /* 1. Exact from_to match */
    for (int i = 0; i < (int)DISPATCH_COUNT; i++) {
        if (dispatch_table[i].from_func == from && dispatch_table[i].to_func == to) {
            if (dispatch_table[i].handler(di, s)) return;
        }
    }
    /* 2. to_only match */
    for (int i = 0; i < (int)DISPATCH_COUNT; i++) {
        if (dispatch_table[i].from_func == -1 && dispatch_table[i].to_func == to) {
            if (dispatch_table[i].handler(di, s)) return;
        }
    }
    /* 3. from_only match */
    for (int i = 0; i < (int)DISPATCH_COUNT; i++) {
        if (dispatch_table[i].from_func == from && dispatch_table[i].to_func == -1) {
            if (dispatch_table[i].handler(di, s)) return;
        }
    }
    /* 4. both wildcard */
    for (int i = 0; i < (int)DISPATCH_COUNT; i++) {
        if (dispatch_table[i].from_func == -1 && dispatch_table[i].to_func == -1) {
            if (dispatch_table[i].handler(di, s)) return;
        }
    }
}

/* --- Core recv_proto --- */

static void avclan_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    avclan_state *s = (avclan_state *)c_decoder_get_private(di);
    if (!s) return;

    s->ss = start_sample;
    s->es = end_sample;

    if (strcmp(cmd, "NAK") == 0) {
        s->state = AVC_IDLE;
        return;
    }

    if (s->state == AVC_IDLE) {
        if (strcmp(cmd, "HEADER") == 0) {
            s->broadcast_bit = (fields && n_fields > 0) ? fields[0].u8 : 0;
            s->state = AVC_MASTER_ADDRESS;
        }
    } else if (s->state == AVC_MASTER_ADDRESS) {
        if (strcmp(cmd, "MASTER ADDRESS") == 0) {
            if (fields && n_fields >= 2) {
                s->master_addr = fields[0].u8 | (fields[1].u8 << 8);
                const char *name = find_name(hw_addresses, HW_ADDR_COUNT, s->master_addr);
                if (name)
                    c_put(di, start_sample, end_sample, s->out_ann, ANN_ADDRESS, name);
            }
            s->state = AVC_SLAVE_ADDRESS;
        }
    } else if (s->state == AVC_SLAVE_ADDRESS) {
        if (strcmp(cmd, "SLAVE ADDRESS") == 0) {
            if (fields && n_fields >= 2) {
                s->slave_addr = fields[0].u8 | (fields[1].u8 << 8);
                const char *name = find_name(hw_addresses, HW_ADDR_COUNT, s->slave_addr);
                if (name)
                    c_put(di, start_sample, end_sample, s->out_ann, ANN_ADDRESS, name);
            }
            s->state = AVC_CONTROL;
        }
    } else if (s->state == AVC_CONTROL) {
        if (strcmp(cmd, "CONTROL") == 0) {
            s->control = (fields && n_fields > 0) ? fields[0].u8 : 0;
            s->state = AVC_DATA_LENGTH;
        }
    } else if (s->state == AVC_DATA_LENGTH) {
        if (strcmp(cmd, "DATA LENGTH") == 0) {
            s->data_length = (fields && n_fields > 0) ? fields[0].u8 : 0;
            s->state = AVC_DATA;
        }
    } else if (s->state == AVC_DATA) {
        if (strcmp(cmd, "DATA") == 0) {
            /* Parse DATA entries: uint16_t count, then (byte_val, parity, ack, ss, es) * count */
            if (!fields || n_fields < 2) return;

            uint16_t count;
            count = fields[0].u8 | (fields[1].u8 << 8);

            const c_field *ptr = fields + 2;
            uint64_t remaining = n_fields - 2;

            for (uint16_t i = 0; i < count; i++) {
                if (remaining < 5) break; /* 5 c_fields per entry: byte_val, parity, ack, ss, es */
                uint8_t byte_val = ptr[0].u8;
                /* ptr[1] = parity_bit, ptr[2] = ack_bit */
                uint64_t entry_ss = ptr[3].u64;
                uint64_t entry_es = ptr[4].u64;

                if (s->num_data_bytes < 256) {
                    s->data_bytes[s->num_data_bytes].b = byte_val;
                    s->data_bytes[s->num_data_bytes].ss = entry_ss;
                    s->data_bytes[s->num_data_bytes].es = entry_es;
                    s->num_data_bytes++;
                }

                ptr += 5;
                remaining -= 5;
            }

            /* Decode logical device IDs */
            if (s->broadcast_bit == 1 && s->num_data_bytes >= 3) {
                /* Unicast */
                s->from_function = s->data_bytes[1].b;
                s->to_function = s->data_bytes[2].b;

                const char *from_name = find_name(function_ids, FUNC_ID_COUNT, s->from_function);
                char t[128];
                if (from_name)
                    snprintf(t, sizeof(t), "From Function: %s", from_name);
                else
                    snprintf(t, sizeof(t), "From Function");
                c_put(di, s->data_bytes[1].ss, s->data_bytes[1].es, s->out_ann, ANN_FUNCTION, t, from_name ? from_name : "From");

                const char *to_name = find_name(function_ids, FUNC_ID_COUNT, s->to_function);
                if (to_name)
                    snprintf(t, sizeof(t), "To Function: %s", to_name);
                else
                    snprintf(t, sizeof(t), "To Function");
                c_put(di, s->data_bytes[2].ss, s->data_bytes[2].es, s->out_ann, ANN_FUNCTION, t, to_name ? to_name : "To");

                /* Shift data_bytes: remove first 3 */
                int shift = s->num_data_bytes - 3;
                for (int i = 0; i < shift; i++)
                    s->data_bytes[i] = s->data_bytes[i + 3];
                s->num_data_bytes = shift;

            } else if (s->broadcast_bit == 0 && s->num_data_bytes >= 2) {
                /* Broadcast */
                s->from_function = s->data_bytes[0].b;
                s->to_function = s->data_bytes[1].b;

                const char *from_name = find_name(function_ids, FUNC_ID_COUNT, s->from_function);
                char t[128];
                if (from_name)
                    snprintf(t, sizeof(t), "From Function: %s", from_name);
                else
                    snprintf(t, sizeof(t), "From Function");
                c_put(di, s->data_bytes[0].ss, s->data_bytes[0].es, s->out_ann, ANN_FUNCTION, t, from_name ? from_name : "From");

                const char *to_name = find_name(function_ids, FUNC_ID_COUNT, s->to_function);
                if (to_name)
                    snprintf(t, sizeof(t), "To Function: %s", to_name);
                else
                    snprintf(t, sizeof(t), "To Function");
                c_put(di, s->data_bytes[1].ss, s->data_bytes[1].es, s->out_ann, ANN_FUNCTION, t, to_name ? to_name : "To");

                /* Shift data_bytes: remove first 2 */
                int shift = s->num_data_bytes - 2;
                for (int i = 0; i < shift; i++)
                    s->data_bytes[i] = s->data_bytes[i + 2];
                s->num_data_bytes = shift;
            }

            if (s->from_function >= 0 && s->to_function >= 0 && s->num_data_bytes > 0)
                dispatch_packet(di, s);

            /* Reset for next frame */
            s->state = AVC_IDLE;
            s->broadcast_bit = 0;
            s->master_addr = 0;
            s->slave_addr = 0;
            s->control = 0;
            s->data_length = 0;
            s->num_data_bytes = 0;
            s->from_function = -1;
            s->to_function = -1;
        }
    } else {
        s->state = AVC_IDLE;
    }
}

/* --- Decoder lifecycle --- */

static void avclan_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(avclan_state)));
    avclan_state *s = (avclan_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(avclan_state));
    s->state = AVC_IDLE;
    s->from_function = -1;
    s->to_function = -1;
}

static void avclan_start(struct srd_decoder_inst *di)
{
    avclan_state *s = (avclan_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "avclan");
}

static void avclan_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void avclan_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder avclan_c_decoder = {
    .id = "avclan_c",
    .name = "AVC-LAN(C)",
    .longname = "AVC-LAN Toyota Audio-Video Local Area Network (C)",
    .desc = "AVC-LAN Protocol Decoder (IEBus Mode 2 variant) (C implementation)",
    .license = "gplv3+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = avclan_ann_labels,
    .num_annotation_rows = 7,
    .annotation_rows = avclan_ann_rows,
    .inputs = avclan_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = avclan_tags,
    .num_tags = 1,
    .reset = avclan_reset,
    .start = avclan_start,
    .decode = avclan_decode,
    .destroy = avclan_destroy,
    .decode_upper = avclan_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &avclan_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}