#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_INFO = 0,
    ANN_TRANSFER_BLOCK,
    ANN_RAW_VALUE,
    ANN_DATA_VAL_POS,
    ANN_DEBUG,
    ANN_DEBUG2,
    ANN_DATA_VAL_NEG,
    ANN_SENDER_PLAYER,
    ANN_SENDER_REMOTE,
    ANN_DATA_FIELD_NAME,
    ANN_ERROR,
    ANN_WARNING,
    ANN_DATA_UNUSED,
    ANN_DATA_UNKNOWN,
    ANN_DATA_STATIC,
    ANN_COMMAND,
    NUM_ANN,
};

#define SONY_MD_MAX_VALUES 16
#define SONY_MD_MAX_BITS 256

typedef struct {
    int out_ann;
    /* Message state */
    uint64_t msg_start_ss;
    uint64_t msg_end_es;
    /* Bit data from protocol */
    uint64_t bit_ss[SONY_MD_MAX_BITS];
    uint64_t bit_es[SONY_MD_MAX_BITS];
    int bit_val[SONY_MD_MAX_BITS];
    int num_bits;
    /* Values extracted from bit data */
    uint8_t values[SONY_MD_MAX_VALUES];
    int value_count;
    /* Checksum */
    uint8_t checksum;
    /* Shift-JIS carryover */
    uint8_t sjis_carryover;
    /* Debug output */
    char debug_hex[256];
} sony_md_state;

static const char *sony_md_inputs[] = {"sony_md", NULL};
static const char *sony_md_outputs[] = {"sony_md_decode", NULL};
static const char *sony_md_tags[] = {"", NULL};

static const char *sony_md_ann_labels[][3] = {
    {"", "Info", "Info"},
    {"", "Transfer block", "Transfer block"},
    {"", "Raw Value", "Raw Value"},
    {"", "Data Field Value (Positive)", "Data Field Value (Positive)"},
    {"", "Debug", "Debug"},
    {"", "Debug2", "Debug2"},
    {"", "Data Field Value (Negative)", "Data Field Value (Negative)"},
    {"", "Transfer Block From Player", "Transfer Block From Player"},
    {"", "Transfer Block From Remote", "Transfer Block From Remote"},
    {"", "Data Field Name", "Data Field Name"},
    {"", "Error", "Error"},
    {"", "Warning", "Warning"},
    {"", "Data Field (Unused)", "Data Field (Unused)"},
    {"", "Data Field (Unknown)", "Data Field (Unknown)"},
    {"", "Data Field (Static)", "Data Field (Static)"},
    {"", "Command", "Command"},
};

static const int sony_md_row_info_classes[] = {ANN_INFO, -1};
static const int sony_md_row_transfer_classes[] = {ANN_TRANSFER_BLOCK, -1};
static const int sony_md_row_senders_classes[] = {ANN_SENDER_PLAYER, ANN_SENDER_REMOTE, -1};
static const int sony_md_row_commands_classes[] = {ANN_COMMAND, -1};
static const int sony_md_row_raw_classes[] = {ANN_RAW_VALUE, -1};
static const int sony_md_row_names_classes[] = {ANN_DATA_FIELD_NAME, -1};
static const int sony_md_row_values_classes[] = {
    ANN_DATA_VAL_POS, ANN_DATA_VAL_NEG, ANN_DATA_UNUSED, ANN_DATA_UNKNOWN, ANN_DATA_STATIC, -1
};
static const int sony_md_row_debugs_classes[] = {ANN_DEBUG, -1};
static const int sony_md_row_debugs2_classes[] = {ANN_DEBUG2, -1};
static const int sony_md_row_errors_classes[] = {ANN_ERROR, -1};
static const int sony_md_row_warnings_classes[] = {ANN_WARNING, -1};

static const struct srd_c_ann_row sony_md_ann_rows[] = {
    {"informational", "Informational", sony_md_row_info_classes, 1},
    {"transfer-blocks", "Data Transfer Blocks", sony_md_row_transfer_classes, 1},
    {"senders", "Block Sender", sony_md_row_senders_classes, 2},
    {"commands", "Commands", sony_md_row_commands_classes, 1},
    {"raw-values", "Raw Values", sony_md_row_raw_classes, 1},
    {"data-field-names", "Data Field Names", sony_md_row_names_classes, 1},
    {"data-field-values", "Data Field Values", sony_md_row_values_classes, 5},
    {"debugs", "Debugs", sony_md_row_debugs_classes, 1},
    {"debugs-two", "Debugs 2", sony_md_row_debugs2_classes, 1},
    {"errors", "Errors", sony_md_row_errors_classes, 1},
    {"warnings", "Warnings", sony_md_row_warnings_classes, 1},
};

static int sony_md_get_value_lsb(sony_md_state *s, int start_bit, int num_bits)
{
    int value = 0;
    int shift = 0;
    for (int i = start_bit; i < start_bit + num_bits && i < s->num_bits; i++) {
        value |= (s->bit_val[i] & 1) << shift;
        shift++;
    }
    s->checksum ^= (uint8_t)value;
    if (s->value_count < SONY_MD_MAX_VALUES)
        s->values[s->value_count++] = (uint8_t)value;
    return value;
}

static void sony_md_put_value_lsb(struct srd_decoder_inst *di, sony_md_state *s,
                                   int start_bit, int num_bits)
{
    int value = sony_md_get_value_lsb(s, start_bit, num_bits);
    if (start_bit + num_bits - 1 >= s->num_bits)
        return;

    char buf[64];
    if (num_bits % 8 == 0) {
        snprintf(buf, sizeof(buf), "Value: 0x%02X", value);
        char h[8];
        snprintf(h, sizeof(h), "0x%02X ", value);
        strcat(s->debug_hex, h);
    } else if (num_bits % 9 == 0) {
        snprintf(buf, sizeof(buf), "Value: 0o%03o", value);
        char h[8];
        snprintf(h, sizeof(h), "0o%03o ", value);
        strcat(s->debug_hex, h);
    } else {
        snprintf(buf, sizeof(buf), "Value (Low %d bits): 0x%X", num_bits, value);
        char h[8];
        snprintf(h, sizeof(h), "0x%X ", value);
        strcat(s->debug_hex, h);
    }
    c_put(di, s->bit_ss[start_bit], s->bit_es[start_bit + num_bits - 1],
              s->out_ann, ANN_RAW_VALUE, buf);
}

static void sony_md_put_static_byte(struct srd_decoder_inst *di, sony_md_state *s,
                                     int current_bit, uint8_t value, uint8_t expected)
{
    if (current_bit + 7 >= s->num_bits)
        return;
    c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
              s->out_ann, ANN_DATA_FIELD_NAME, "Static?");
    c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
              s->out_ann, ANN_DATA_STATIC, "Static?");
    if (value != expected) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Previously static, expected 0x%02X!", expected);
        c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
                  s->out_ann, ANN_ERROR, buf);
    }
}


static void sony_md_put_unknown_byte(struct srd_decoder_inst *di, sony_md_state *s,
                                      int current_bit, uint8_t value)
{
    if (current_bit + 7 >= s->num_bits)
        return;
    c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
              s->out_ann, ANN_DATA_FIELD_NAME, "Unknown?");
    char buf[64];
    snprintf(buf, sizeof(buf), "Unknown: 0x%02X", value);
    c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
              s->out_ann, ANN_DATA_UNKNOWN, buf);
    if (value != 0x00) {
        c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
                  s->out_ann, ANN_ERROR, "Unknown byte has non-zero value!");
    }
}

static void sony_md_put_remote_header(struct srd_decoder_inst *di, sony_md_state *s,
                                       int current_bit)
{
    if (current_bit + 7 >= s->num_bits)
        return;
    c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
              s->out_ann, ANN_TRANSFER_BLOCK, "Header from remote");
    c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
              s->out_ann, ANN_SENDER_REMOTE, "Remote");

    sony_md_put_value_lsb(di, s, current_bit, 8);
    uint8_t val = s->values[0];
    (void)val;

    /* Unused bit 0 */
    if (current_bit < s->num_bits) {
        c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit],
                  s->out_ann, ANN_DATA_FIELD_NAME, "Unused?");
        c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit],
                  s->out_ann, ANN_DATA_UNUSED, "Unused?");
    }

    /* Bit 1: ready for text */
    if (current_bit + 1 < s->num_bits) {
        if (s->bit_val[current_bit + 1] == 1)
            c_put(di, s->bit_ss[current_bit + 1], s->bit_es[current_bit + 1],
                      s->out_ann, ANN_DATA_VAL_POS, "Remote is ready for text");
        else
            c_put(di, s->bit_ss[current_bit + 1], s->bit_es[current_bit + 1],
                      s->out_ann, ANN_DATA_VAL_NEG, "Remote is NOT ready for text");
    }

    /* Bit 2: done scrolling */
    if (current_bit + 2 < s->num_bits) {
        if (s->bit_val[current_bit + 2] == 1) {
            c_put(di, s->bit_ss[current_bit + 2], s->bit_es[current_bit + 2],
                      s->out_ann, ANN_DATA_VAL_POS, "Remote is done scrolling text?");
            c_put(di, s->bit_ss[current_bit + 2], s->bit_es[current_bit + 2],
                      s->out_ann, ANN_DATA_FIELD_NAME, "Weird header, look here");
        } else {
            c_put(di, s->bit_ss[current_bit + 2], s->bit_es[current_bit + 2],
                      s->out_ann, ANN_DATA_VAL_NEG, "Remote is NOT done scrolling text?");
        }
    }

    /* Bit 3: unused */
    if (current_bit + 3 < s->num_bits) {
        c_put(di, s->bit_ss[current_bit + 3], s->bit_es[current_bit + 3],
                  s->out_ann, ANN_DATA_FIELD_NAME, "Unused?");
        c_put(di, s->bit_ss[current_bit + 3], s->bit_es[current_bit + 3],
                  s->out_ann, ANN_DATA_UNUSED, "Unused?");
    }

    /* Bit 4: data to send */
    if (current_bit + 4 < s->num_bits) {
        if (s->bit_val[current_bit + 4] == 1)
            c_put(di, s->bit_ss[current_bit + 4], s->bit_es[current_bit + 4],
                      s->out_ann, ANN_DATA_VAL_POS, "Remote HAS data to send");
        else
            c_put(di, s->bit_ss[current_bit + 4], s->bit_es[current_bit + 4],
                      s->out_ann, ANN_DATA_VAL_NEG, "Remote has NO data to send");
    }

    /* Bit 5: unused */
    if (current_bit + 5 < s->num_bits) {
        c_put(di, s->bit_ss[current_bit + 5], s->bit_es[current_bit + 5],
                  s->out_ann, ANN_DATA_FIELD_NAME, "Unused?");
        c_put(di, s->bit_ss[current_bit + 5], s->bit_es[current_bit + 5],
                  s->out_ann, ANN_DATA_UNUSED, "Unused?");
    }

    /* Bit 6: Kanji capable */
    if (current_bit + 6 < s->num_bits) {
        if (s->bit_val[current_bit + 6] == 1)
            c_put(di, s->bit_ss[current_bit + 6], s->bit_es[current_bit + 6],
                      s->out_ann, ANN_DATA_VAL_POS, "Remote IS Kanji-capable?");
        else
            c_put(di, s->bit_ss[current_bit + 6], s->bit_es[current_bit + 6],
                      s->out_ann, ANN_DATA_VAL_NEG, "Remote is NOT Kanji-capable?");
    }

    /* Bit 7: present */
    if (current_bit + 7 < s->num_bits) {
        if (s->bit_val[current_bit + 7] == 1)
            c_put(di, s->bit_ss[current_bit + 7], s->bit_es[current_bit + 7],
                      s->out_ann, ANN_DATA_VAL_POS, "Remote Present");
        else
            c_put(di, s->bit_ss[current_bit + 7], s->bit_es[current_bit + 7],
                      s->out_ann, ANN_DATA_VAL_NEG, "Remote NOT Present");
    }
}

static void sony_md_put_player_header(struct srd_decoder_inst *di, sony_md_state *s,
                                       int current_bit)
{
    if (current_bit + 7 >= s->num_bits)
        return;
    c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
              s->out_ann, ANN_TRANSFER_BLOCK, "Header from player");
    c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
              s->out_ann, ANN_SENDER_PLAYER, "Player");

    sony_md_put_value_lsb(di, s, current_bit, 8);

    /* Bit 0: data to send */
    if (current_bit < s->num_bits) {
        if (s->bit_val[current_bit] == 0)
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit],
                      s->out_ann, ANN_DATA_VAL_POS, "Player HAS data to send");
        else
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit],
                      s->out_ann, ANN_DATA_VAL_NEG, "Player has NO data to send");
    }

    /* Bits 1-3: unused */
    if (current_bit + 3 < s->num_bits) {
        c_put(di, s->bit_ss[current_bit + 1], s->bit_es[current_bit + 3],
                  s->out_ann, ANN_DATA_FIELD_NAME, "Unused?");
        c_put(di, s->bit_ss[current_bit + 1], s->bit_es[current_bit + 3],
                  s->out_ann, ANN_DATA_UNUSED, "Unused?");
    }

    /* Bit 4: cede bus */
    if (current_bit + 4 < s->num_bits) {
        if (s->bit_val[current_bit + 4] == 1)
            c_put(di, s->bit_ss[current_bit + 4], s->bit_es[current_bit + 4],
                      s->out_ann, ANN_DATA_VAL_POS, "Player cedes the bus to remote after header");
        else
            c_put(di, s->bit_ss[current_bit + 4], s->bit_es[current_bit + 4],
                      s->out_ann, ANN_DATA_VAL_NEG, "Player does NOT cede the bus to remote after header");
    }

    /* Bits 5-6: unused */
    if (current_bit + 6 < s->num_bits) {
        c_put(di, s->bit_ss[current_bit + 5], s->bit_es[current_bit + 6],
                  s->out_ann, ANN_DATA_FIELD_NAME, "Unused?");
        c_put(di, s->bit_ss[current_bit + 5], s->bit_es[current_bit + 6],
                  s->out_ann, ANN_DATA_UNUSED, "Unused?");
    }

    /* Bit 7: present */
    if (current_bit + 7 < s->num_bits) {
        c_put(di, s->bit_ss[current_bit + 7], s->bit_es[current_bit + 7],
                  s->out_ann, ANN_DATA_VAL_POS, "Player Present");
    }
}

static void sony_md_expand_player_data_block(struct srd_decoder_inst *di, sony_md_state *s,
                                              int current_bit)
{
    int current_byte = 2;
    int not_done = 1;

    while (not_done) {
        if (current_byte >= 12 || current_byte >= s->value_count) {
            not_done = 0;
            continue;
        }
        if (s->values[current_byte] == 0x00) {
            not_done = 0;
            continue;
        }

        if (current_bit + 7 >= s->num_bits) {
            not_done = 0;
            continue;
        }

        c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
                  s->out_ann, ANN_DATA_FIELD_NAME, "Packet type");

        switch (s->values[current_byte]) {
        case 0x01: /* Request Remote Capabilities */
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 15],
                      s->out_ann, ANN_COMMAND, "Request Remote Capabilities");
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
                      s->out_ann, ANN_DATA_VAL_POS, "Request Remote capabilities");
            if (current_byte + 1 < s->value_count) {
                switch (s->values[current_byte + 1]) {
                case 0x01:
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_DATA_VAL_POS, "First block");
                    break;
                case 0x02:
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_DATA_VAL_POS, "Second block, LCD capabilities?");
                    break;
                case 0x05:
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_DATA_VAL_POS, "Fifth block");
                    break;
                case 0x06:
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_DATA_VAL_POS, "Sixth block?");
                    break;
                case 0x7F:
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_DATA_VAL_POS, "Unknown, seen from D-EJ955");
                    break;
                default:
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_ERROR, "UNRECOGNIZED VALUE");
                    break;
                }
            }
            current_bit += 16;
            current_byte += 2;
            break;

        case 0x02: /* Unknown initialization */
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 15],
                      s->out_ann, ANN_COMMAND, "Unknown, seems to be two bytes sent soon after initialization?");
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
                      s->out_ann, ANN_DATA_VAL_POS, "Unknown, seems to be two bytes sent soon after initialization?");
            if (current_byte + 1 < s->value_count)
                sony_md_put_static_byte(di, s, current_bit + 8, s->values[current_byte + 1], 0x80);
            current_bit += 16;
            current_byte += 2;
            break;

        case 0x03: /* Scroll Control */
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 31],
                      s->out_ann, ANN_COMMAND, "Scroll Control?");
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
                      s->out_ann, ANN_DATA_VAL_POS, "Scroll control?");
            if (current_byte + 1 < s->value_count)
                sony_md_put_static_byte(di, s, current_bit + 8, s->values[current_byte + 1], 0x80);
            if (current_byte + 3 < s->value_count) {
                if (s->values[current_byte + 2] == 0x02 && s->values[current_byte + 3] == 0x80)
                    c_put(di, s->bit_ss[current_bit + 16], s->bit_es[current_bit + 31],
                              s->out_ann, ANN_DATA_VAL_POS, "Scrolling: Enabled");
                else if (s->values[current_byte + 2] == 0x00 && s->values[current_byte + 3] == 0x00)
                    c_put(di, s->bit_ss[current_bit + 16], s->bit_es[current_bit + 31],
                              s->out_ann, ANN_DATA_VAL_POS, "Scrolling: Disabled");
                else
                    c_put(di, s->bit_ss[current_bit + 16], s->bit_es[current_bit + 31],
                              s->out_ann, ANN_ERROR, "UNRECOGNIZED VALUE");
            }
            current_bit += 32;
            current_byte += 4;
            break;

        case 0x05: /* LCD Backlight Control */
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 15],
                      s->out_ann, ANN_COMMAND, "LCD Backlight Control");
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
                      s->out_ann, ANN_DATA_VAL_POS, "LCD Backlight Control");
            if (current_byte + 1 < s->value_count) {
                if (s->values[current_byte + 1] == 0x00)
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_DATA_VAL_POS, "LCD Backlight: Off");
                else if (s->values[current_byte + 1] == 0x7F)
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_DATA_VAL_POS, "LCD Backlight: On");
                else
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_ERROR, "UNRECOGNIZED VALUE");
            }
            current_bit += 16;
            current_byte += 2;
            break;

        case 0x06: /* LCD Remote Service Mode */
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 47],
                      s->out_ann, ANN_COMMAND, "LCD Remote Service Mode?");
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
                      s->out_ann, ANN_DATA_VAL_POS, "LCD Remote Service Mode Control?");
            if (current_byte + 1 < s->value_count) {
                if (s->values[current_byte + 1] == 0x7F) {
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_DATA_VAL_POS, "LCD Remote Service Mode End");
                } else if (current_byte + 5 < s->value_count &&
                           s->values[current_byte + 1] == 0x00 &&
                           s->values[current_byte + 2] == 0x06 &&
                           s->values[current_byte + 3] == 0x01 &&
                           s->values[current_byte + 4] == 0x03 &&
                           s->values[current_byte + 5] == 0x80) {
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 47],
                              s->out_ann, ANN_DATA_VAL_POS, "LCD Remote Service Mode All Segments On?");
                } else {
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_ERROR, "UNRECOGNIZED VALUE");
                }
            }
            current_bit += 48;
            current_byte += 6;
            break;

        case 0x08: /* Pre-text update */
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 31],
                      s->out_ann, ANN_COMMAND, "Unknown, seems to be sent before 0xC8 text updates?");
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
                      s->out_ann, ANN_DATA_VAL_POS, "Unknown, seems to be sent before 0xC8 text updates");
            if (current_byte + 1 < s->value_count)
                sony_md_put_static_byte(di, s, current_bit + 8, s->values[current_byte + 1], 0x80);
            if (current_byte + 2 < s->value_count)
                sony_md_put_static_byte(di, s, current_bit + 16, s->values[current_byte + 2], 0x07);
            if (current_byte + 3 < s->value_count)
                sony_md_put_static_byte(di, s, current_bit + 24, s->values[current_byte + 3], 0x80);
            current_bit += 32;
            current_byte += 4;
            break;

        case 0x40: /* Volume Level */
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 15],
                      s->out_ann, ANN_COMMAND, "Volume Level");
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
                      s->out_ann, ANN_DATA_VAL_POS, "Volume Level");
            if (current_byte + 1 < s->value_count) {
                char buf[64];
                if (s->values[current_byte + 1] == 0xFF)
                    snprintf(buf, sizeof(buf), "Current Volume Level: 32/32");
                else if (s->values[current_byte + 1] < 32)
                    snprintf(buf, sizeof(buf), "Current Volume Level: %d/32", s->values[current_byte + 1]);
                else
                    snprintf(buf, sizeof(buf), "UNRECOGNIZED VALUE");
                c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                          s->out_ann, s->values[current_byte + 1] < 33 ? ANN_DATA_VAL_POS : ANN_ERROR, buf);
            }
            current_bit += 16;
            current_byte += 2;
            break;

        case 0x41: /* Playback Mode */
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 15],
                      s->out_ann, ANN_COMMAND, "Playback Mode");
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
                      s->out_ann, ANN_DATA_VAL_POS, "Playback Mode");
            if (current_byte + 1 < s->value_count) {
                const char *mode = NULL;
                switch (s->values[current_byte + 1]) {
                case 0x00: mode = "Normal"; break;
                case 0x01: mode = "Repeat All Tracks"; break;
                case 0x02: mode = "One Track, Stop Afterwards"; break;
                case 0x03: mode = "Repeat One Track"; break;
                case 0x04: mode = "Shuffle No Repeats"; break;
                case 0x05: mode = "Shuffle With Repeats"; break;
                case 0x06: mode = "PGM, No Repeats"; break;
                case 0x07: mode = "PGM, Repeat"; break;
                }
                if (mode) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "Current Playback Mode: %s", mode);
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_DATA_VAL_POS, buf);
                } else {
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_ERROR, "UNRECOGNIZED VALUE");
                }
            }
            current_bit += 16;
            current_byte += 2;
            break;

        case 0x42: /* Recording Indicator */
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 15],
                      s->out_ann, ANN_COMMAND, "Recording Indicator");
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
                      s->out_ann, ANN_DATA_VAL_POS, "Recording Indicator");
            if (current_byte + 1 < s->value_count) {
                if (s->values[current_byte + 1] == 0x00)
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_DATA_VAL_POS, "Recording Indicator: Off");
                else if (s->values[current_byte + 1] == 0x7F)
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_DATA_VAL_POS, "Recording Indicator: On");
                else
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_ERROR, "UNRECOGNIZED VALUE");
            }
            current_bit += 16;
            current_byte += 2;
            break;

        case 0x43: /* Battery Level Indicator */
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 15],
                      s->out_ann, ANN_COMMAND, "Battery Level Indicator");
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
                      s->out_ann, ANN_DATA_VAL_POS, "Battery Level Indicator");
            if (current_byte + 1 < s->value_count) {
                const char *batt = NULL;
                switch (s->values[current_byte + 1]) {
                case 0x00: batt = "Off"; break;
                case 0x01: batt = "1/4 bars, blinking"; break;
                case 0x7F: batt = "Charging"; break;
                case 0x80: batt = "Empty, blinking"; break;
                case 0x9F: batt = "1/4 bars"; break;
                case 0xBF: batt = "2/4 bars"; break;
                case 0xDF: batt = "3/4 bars"; break;
                case 0xFF: batt = "4/4 bars"; break;
                }
                if (batt) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "Battery Level Indicator: %s", batt);
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_DATA_VAL_POS, buf);
                } else {
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_ERROR, "UNRECOGNIZED VALUE");
                }
            }
            current_bit += 16;
            current_byte += 2;
            break;

        case 0x46: /* EQ/Sound Indicator */
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 15],
                      s->out_ann, ANN_COMMAND, "EQ/Sound Indicator");
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
                      s->out_ann, ANN_DATA_VAL_POS, "EQ/Sound Indicator");
            if (current_byte + 1 < s->value_count) {
                const char *eq = NULL;
                switch (s->values[current_byte + 1]) {
                case 0x00: eq = "Normal"; break;
                case 0x01: eq = "Bass 1?"; break;
                case 0x02: eq = "Bass 2?"; break;
                case 0x03: eq = "Sound 1"; break;
                case 0x04: eq = "Sound 2"; break;
                }
                if (eq) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "EQ/Sound Indicator: %s", eq);
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_DATA_VAL_POS, buf);
                } else {
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_ERROR, "UNRECOGNIZED VALUE");
                }
            }
            current_bit += 16;
            current_byte += 2;
            break;

        case 0x47: /* Alarm Indicator */
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 15],
                      s->out_ann, ANN_COMMAND, "Alarm Indicator");
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
                      s->out_ann, ANN_DATA_VAL_POS, "Alarm Indicator");
            if (current_byte + 1 < s->value_count) {
                if (s->values[current_byte + 1] == 0x00)
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_DATA_VAL_POS, "Alarm Indicator: Off");
                else if (s->values[current_byte + 1] == 0x7F)
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_DATA_VAL_POS, "Alarm Indicator: On");
                else
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_ERROR, "UNRECOGNIZED VALUE");
            }
            current_bit += 16;
            current_byte += 2;
            break;

        case 0xA0: /* Track number */
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 39],
                      s->out_ann, ANN_COMMAND, "Track number");
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
                      s->out_ann, ANN_DATA_VAL_POS, "Track number");
            if (current_byte + 1 < s->value_count) {
                if (s->values[current_byte + 1] == 0x00)
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_DATA_VAL_POS, "Track Number Indicator: On");
                else if (s->values[current_byte + 1] == 0x80)
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_DATA_VAL_POS, "Track Number Indicator: Off");
                else
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_ERROR, "UNRECOGNIZED VALUE");
            }
            if (current_byte + 2 < s->value_count)
                sony_md_put_static_byte(di, s, current_bit + 16, s->values[current_byte + 2], 0x00);
            if (current_byte + 3 < s->value_count)
                sony_md_put_static_byte(di, s, current_bit + 24, s->values[current_byte + 3], 0x00);
            if (current_byte + 4 < s->value_count) {
                char buf[64];
                snprintf(buf, sizeof(buf), "Current Track Number: %d", s->values[current_byte + 4]);
                c_put(di, s->bit_ss[current_bit + 32], s->bit_es[current_bit + 39],
                          s->out_ann, ANN_DATA_VAL_POS, buf);
            }
            current_bit += 40;
            current_byte += 5;
            break;

        case 0xA1: /* LCD Disc Icon Control */
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 39],
                      s->out_ann, ANN_COMMAND, "LCD Disc Icon Control");
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
                      s->out_ann, ANN_DATA_VAL_POS, "LCD Disc Icon Control");
            if (current_byte + 1 < s->value_count)
                sony_md_put_static_byte(di, s, current_bit + 8, s->values[current_byte + 1], 0x00);
            if (current_byte + 2 < s->value_count) {
                if (s->values[current_byte + 2] == 0x00)
                    c_put(di, s->bit_ss[current_bit + 16], s->bit_es[current_bit + 23],
                              s->out_ann, ANN_DATA_VAL_POS, "LCD Disc Icon Outline: Off");
                else if (s->values[current_byte + 2] == 0x7F)
                    c_put(di, s->bit_ss[current_bit + 16], s->bit_es[current_bit + 23],
                              s->out_ann, ANN_DATA_VAL_POS, "LCD Disc Icon Outline: On");
                else
                    c_put(di, s->bit_ss[current_bit + 16], s->bit_es[current_bit + 23],
                              s->out_ann, ANN_ERROR, "UNRECOGNIZED VALUE");
            }
            if (current_byte + 3 < s->value_count) {
                if (s->values[current_byte + 3] == 0x00)
                    c_put(di, s->bit_ss[current_bit + 24], s->bit_es[current_bit + 31],
                              s->out_ann, ANN_DATA_VAL_POS, "LCD Disc Icon Fill Segments: All disabled");
                else if (s->values[current_byte + 3] == 0x7F)
                    c_put(di, s->bit_ss[current_bit + 24], s->bit_es[current_bit + 31],
                              s->out_ann, ANN_DATA_VAL_POS, "LCD Disc Icon Fill Segments: All enabled");
                else
                    c_put(di, s->bit_ss[current_bit + 24], s->bit_es[current_bit + 31],
                              s->out_ann, ANN_ERROR, "UNRECOGNIZED VALUE");
            }
            if (current_byte + 4 < s->value_count) {
                const char *anim = NULL;
                switch (s->values[current_byte + 4]) {
                case 0x00: anim = "No animation, no segments displayed"; break;
                case 0x01: anim = "\"Fast Spinning\" animation"; break;
                case 0x03: anim = "\"Spinning\" animation"; break;
                case 0x7F: anim = "No animation, all segments displayed"; break;
                }
                if (anim) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "LCD Disc Icon Fill Segment Animation: %s", anim);
                    c_put(di, s->bit_ss[current_bit + 32], s->bit_es[current_bit + 39],
                              s->out_ann, ANN_DATA_VAL_POS, buf);
                } else {
                    c_put(di, s->bit_ss[current_bit + 32], s->bit_es[current_bit + 39],
                              s->out_ann, ANN_ERROR, "UNRECOGNIZED VALUE");
                }
            }
            current_bit += 40;
            current_byte += 5;
            break;

        case 0xC0: /* Player capabilities */
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 79],
                      s->out_ann, ANN_COMMAND, "Player capabilities?");
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
                      s->out_ann, ANN_DATA_VAL_POS, "Player capabilities?");
            if (current_byte + 1 < s->value_count) {
                if (s->values[current_byte + 1] == 0x05) {
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_DATA_VAL_POS, "Fifth block?");
                    for (int i = 2; i <= 9 && (current_byte + i) < s->value_count; i++)
                        sony_md_put_unknown_byte(di, s, current_bit + i * 8, s->values[current_byte + i]);
                } else {
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_ERROR, "UNRECOGNIZED VALUE");
                }
            }
            current_bit += 80;
            current_byte += 10;
            break;

        case 0xC8: /* LCD Text */
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 79],
                      s->out_ann, ANN_COMMAND, "LCD Text");
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
                      s->out_ann, ANN_DATA_VAL_POS, "LCD Text");
            if (current_byte + 1 < s->value_count) {
                if (s->values[current_byte + 1] == 0x02)
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_DATA_VAL_POS, "Non-final segment?");
                else if (s->values[current_byte + 1] == 0x01)
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_DATA_VAL_POS, "Final segment?");
                else
                    c_put(di, s->bit_ss[current_bit + 8], s->bit_es[current_bit + 15],
                              s->out_ann, ANN_ERROR, "UNRECOGNIZED VALUE");
            }
            if (current_byte + 2 < s->value_count)
                sony_md_put_static_byte(di, s, current_bit + 16, s->values[current_byte + 2], 0x00);
            /* Display character positions */
            for (int i = 0; i < 7 && (current_byte + 3 + i) < s->value_count; i++) {
                char pos_buf[32];
                snprintf(pos_buf, sizeof(pos_buf), "String position %d", i + 1);
                c_put(di, s->bit_ss[current_bit + 24 + i * 8], s->bit_es[current_bit + 31 + i * 8],
                          s->out_ann, ANN_DATA_FIELD_NAME, pos_buf);
                /* Simple character display */
                uint8_t ch = s->values[current_byte + 3 + i];
                if (ch >= 0x20 && ch <= 0x7E) {
                    char cbuf[4];
                    cbuf[0] = (char)ch;
                    cbuf[1] = '\0';
                    c_put(di, s->bit_ss[current_bit + 24 + i * 8], s->bit_es[current_bit + 31 + i * 8],
                              s->out_ann, ANN_DATA_VAL_POS, cbuf);
                } else if (ch == 0xFF) {
                    c_put(di, s->bit_ss[current_bit + 24 + i * 8], s->bit_es[current_bit + 31 + i * 8],
                              s->out_ann, ANN_DATA_VAL_POS, "<End of string>");
                } else {
                    char cbuf[32];
                    snprintf(cbuf, sizeof(cbuf), "0x%02X", ch);
                    c_put(di, s->bit_ss[current_bit + 24 + i * 8], s->bit_es[current_bit + 31 + i * 8],
                              s->out_ann, ANN_DATA_UNKNOWN, cbuf);
                }
            }
            current_bit += 88;
            current_byte += 11;
            break;

        default:
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 7],
                      s->out_ann, ANN_ERROR, "UNRECOGNIZED VALUE");
            current_bit += 8;
            current_byte += 1;
            not_done = 0;
            break;
        }
    }
}

static void sony_md_put_player_data_block(struct srd_decoder_inst *di, sony_md_state *s,
                                           int current_bit)
{
    if (current_bit + 87 >= s->num_bits)
        return;
    c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 87],
              s->out_ann, ANN_TRANSFER_BLOCK, "Player data block?");
    c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 87],
              s->out_ann, ANN_SENDER_PLAYER, "Player");

    /* Read 11 bytes (10 data + 1 checksum) */
    for (int i = 0; i < 10; i++)
        sony_md_put_value_lsb(di, s, current_bit + i * 8, 8);

    /* Checksum */
    c_put(di, s->bit_ss[current_bit + 80], s->bit_es[current_bit + 87],
              s->out_ann, ANN_DATA_FIELD_NAME, "Checksum");
    uint8_t calced = s->checksum;
    uint8_t received = (uint8_t)sony_md_get_value_lsb(s, current_bit + 80, 8);
    char chk_buf[64];
    if (calced == received)
        snprintf(chk_buf, sizeof(chk_buf), "Checksum, calculated value 0x%02X, valid!", calced);
    else
        snprintf(chk_buf, sizeof(chk_buf), "Checksum, calculated value 0x%02X, invalid!", calced);
    c_put(di, s->bit_ss[current_bit + 80], s->bit_es[current_bit + 87],
              s->out_ann, (calced == received) ? ANN_DATA_VAL_POS : ANN_DATA_VAL_NEG, chk_buf);

    sony_md_expand_player_data_block(di, s, current_bit);
}

static void sony_md_expand_message(struct srd_decoder_inst *di, sony_md_state *s)
{
    int current_bit = 0;

    s->debug_hex[0] = '\0';
    s->value_count = 0;
    s->checksum = 0;

    sony_md_put_remote_header(di, s, current_bit);
    current_bit += 8;

    sony_md_put_player_header(di, s, current_bit);
    current_bit += 8;

    s->checksum = 0;

    if (current_bit + 87 < s->num_bits) {
        if (s->bit_val[8] == 0 && s->bit_val[12] == 0) {
            sony_md_put_player_data_block(di, s, current_bit);
        } else if (s->bit_val[12] == 1) {
            /* Remote data block - simplified */
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit + 98],
                      s->out_ann, ANN_TRANSFER_BLOCK, "Remote Data Block");
            c_put(di, s->bit_ss[current_bit], s->bit_es[current_bit],
                      s->out_ann, ANN_SENDER_PLAYER, "Player");
            c_put(di, s->bit_ss[current_bit + 1], s->bit_es[current_bit + 8],
                      s->out_ann, ANN_SENDER_REMOTE, "Remote");
        }
    }

    c_put(di, s->msg_start_ss, s->msg_end_es, s->out_ann, ANN_DEBUG, s->debug_hex);
}

static void sony_md_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    sony_md_state *s = (sony_md_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "MESSAGE") == 0) {
        /* Parse bit data from the message.
         * Format: num_bits(2 bytes LE) + bit entries (9 bytes each: ss(4) + es(4) + val(1))
         */
        if (n_fields < 2)
            return;

        
        s->num_bits = 0;
        s->value_count = 0;
        s->checksum = 0;
        s->sjis_carryover = 0;
        s->debug_hex[0] = '\0';

        int num_bits = ((int)n_fields - 2) / 9;

        if (num_bits > SONY_MD_MAX_BITS)
            num_bits = SONY_MD_MAX_BITS;

        for (int i = 0; i < num_bits; i++) {
            const c_field *p = fields + 2 + i * 9;
            uint64_t ss = 0, es = 0;
            for (int j = 0; j < 4; j++) {
                ss |= ((uint64_t)p[j].u8) << (j * 8);
                es |= ((uint64_t)p[4 + j].u8) << (j * 8);
            }
            int val = p[8].u8 & 1;
            s->bit_ss[i] = ss;
            s->bit_es[i] = es;
            s->bit_val[i] = val;
            s->num_bits++;
        }

        if (s->num_bits > 0) {
            s->msg_start_ss = s->bit_ss[0];
            s->msg_end_es = s->bit_es[s->num_bits - 1];

            /* Message start */
            c_put(di, s->msg_start_ss, s->msg_start_ss, s->out_ann, ANN_INFO, "Message Start");

            sony_md_expand_message(di, s);

            /* Message end */
            c_put(di, s->msg_end_es, s->msg_end_es, s->out_ann, ANN_INFO, "Message End");
        }
    }
}

static void sony_md_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(sony_md_state)));
    }
    sony_md_state *s = (sony_md_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(sony_md_state));
}

static void sony_md_start(struct srd_decoder_inst *di)
{
    sony_md_state *s = (sony_md_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "sony_md_decode");
}

static void sony_md_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void sony_md_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder sony_md_decode_c_decoder = {
    .id = "sony_md_decode_c",
    .name = "Sony MD Remote Decode(C)",
    .longname = "Sony MD LCD Remote Decoder (C)",
    .desc = "Sony MD LCD Remote Decoder. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = sony_md_ann_labels,
    .num_annotation_rows = 11,
    .annotation_rows = sony_md_ann_rows,
    .inputs = sony_md_inputs,
    .num_inputs = 1,
    .outputs = sony_md_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = sony_md_tags,
    .num_tags = 1,
    .reset = sony_md_reset,
    .start = sony_md_start,
    .decode = sony_md_decode,
    .destroy = sony_md_destroy,
    .decode_upper = sony_md_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &sony_md_decode_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}