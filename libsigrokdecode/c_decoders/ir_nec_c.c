#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum nec_state {
    STATE_IDLE,
    STATE_ADDRESS,
    STATE_ADDRESS_INV,
    STATE_COMMAND,
    STATE_COMMAND_INV,
    STATE_STOP,
};

enum nec_ann {
    ANN_BIT = 0,
    ANN_AGC,
    ANN_LONG_PAUSE,
    ANN_SHORT_PAUSE,
    ANN_STOP_BIT,
    ANN_LEADER_CODE,
    ANN_ADDR,
    ANN_ADDR_INV,
    ANN_CMD,
    ANN_CMD_INV,
    ANN_REPEAT_CODE,
    ANN_REMOTE,
    ANN_WARN,
};

#define IR_CH 0

#define TIME_TOL 10
#define TIME_IDLE 20.0
#define TIME_LC 13.5
#define TIME_RC 11.25
#define TIME_ONE 2.25
#define TIME_ZERO 1.125
#define TIME_STOP 0.562

typedef struct {
    enum nec_state state;
    uint64_t ss_bit;
    uint64_t ss_start;
    uint64_t ss_other_edge;
    uint64_t ss_remote;
    uint8_t data[32];
    int data_len;
    uint16_t addr;
    uint8_t cmd;
    int active;
    int is_extended;
    int want_addr_len;
    uint64_t lc;
    uint64_t rc;
    uint64_t dazero;
    uint64_t daone;
    uint64_t stop;
    uint64_t idle_to;
    double tolerance;
    int out_ann;
    uint64_t samplerate;
    int cd_freq;
    uint64_t cd_count;
    int have_cd_count;
    int prev_ir;
    int polarity_auto_done;
} nec_state;

static struct srd_channel nec_channels[] = {
    { "ir", "IR", "Data line", 0, SRD_CHANNEL_SDATA, "dec_ir_nec_chan_ir" },
};

static struct srd_decoder_option nec_options_arr[3];

static const char* nec_ann_labels[][3] = {
    { "", "bit", "Bit" },
    { "", "agc-pulse", "AGC pulse" },
    { "", "longpause", "Long pause" },
    { "", "shortpause", "Short pause" },
    { "", "stop-bit", "Stop bit" },
    { "", "leader-code", "Leader code" },
    { "", "addr", "Address" },
    { "", "addr-inv", "Address#" },
    { "", "cmd", "Command" },
    { "", "cmd-inv", "Command#" },
    { "", "repeat-code", "Repeat code" },
    { "", "remote", "Remote" },
    { "", "warning", "Warning" },
};

static const int nec_row_bits_classes[] = { ANN_BIT, ANN_AGC, ANN_LONG_PAUSE, ANN_SHORT_PAUSE, ANN_STOP_BIT, -1 };
static const int nec_row_fields_classes[] = { ANN_LEADER_CODE, ANN_ADDR, ANN_ADDR_INV, ANN_CMD, ANN_CMD_INV, ANN_REPEAT_CODE, -1 };
static const int nec_row_remote_classes[] = { ANN_REMOTE, -1 };
static const int nec_row_warnings_classes[] = { ANN_WARN, -1 };
static const struct srd_c_ann_row nec_ann_rows[] = {
    { "bits", "Bits", nec_row_bits_classes, 5 },
    { "fields", "Fields", nec_row_fields_classes, 6 },
    { "remote-vals", "Remote", nec_row_remote_classes, 1 },
    { "warnings", "Warnings", nec_row_warnings_classes, 1 },
};

static const char* nec_inputs[] = { "logic", NULL };
static const char* nec_outputs[] = { NULL };
static const char* nec_tags[] = { "IR", NULL };

/* Device name lookup table (from ir_nec/lists.py) */
typedef struct {
    uint16_t addr;
    const char* name;
} nec_address_entry;

static const nec_address_entry nec_address_table[] = {
    { 0x40, "Matsui TV" },
};

#define NEC_ADDRESS_TABLE_SIZE (sizeof(nec_address_table) / sizeof(nec_address_table[0]))

/* Command name lookup table (from ir_nec/lists.py) */
typedef struct {
    uint8_t cmd;
    const char* name_long;
    const char* name_short;
} nec_command_entry;

static const nec_command_entry nec_command_table_0x40[] = {
    { 0, "0", "0" },
    { 1, "1", "1" },
    { 2, "2", "2" },
    { 3, "3", "3" },
    { 4, "4", "4" },
    { 5, "5", "5" },
    { 6, "6", "6" },
    { 7, "7", "7" },
    { 8, "8", "8" },
    { 9, "9", "9" },
    { 11, "-/--", "-/--" },
    { 16, "Mute", "M" },
    { 18, "Standby", "StBy" },
    { 26, "Volume up", "Vol+" },
    { 27, "Program up", "P+" },
    { 30, "Volume down", "Vol-" },
    { 31, "Program down", "P-" },
    { 68, "AV", "AV" },
};

#define NEC_COMMAND_TABLE_0X40_SIZE (sizeof(nec_command_table_0x40) / sizeof(nec_command_table_0x40[0]))

typedef struct {
    uint16_t addr;
    const nec_command_entry* commands;
    int num_commands;
} nec_command_map;

static const nec_command_map nec_command_maps[] = {
    { 0x40, nec_command_table_0x40, NEC_COMMAND_TABLE_0X40_SIZE },
};

#define NEC_COMMAND_MAPS_SIZE (sizeof(nec_command_maps) / sizeof(nec_command_maps[0]))

static const char* nec_lookup_address(uint16_t addr)
{
    int i;
    for (i = 0; i < (int)NEC_ADDRESS_TABLE_SIZE; i++) {
        if (nec_address_table[i].addr == addr)
            return nec_address_table[i].name;
    }
    return "Unknown device";
}

static int nec_lookup_command(uint16_t addr, uint8_t cmd, const char** name_long, const char** name_short)
{
    int i, j;
    for (i = 0; i < (int)NEC_COMMAND_MAPS_SIZE; i++) {
        if (nec_command_maps[i].addr == addr) {
            for (j = 0; j < nec_command_maps[i].num_commands; j++) {
                if (nec_command_maps[i].commands[j].cmd == cmd) {
                    *name_long = nec_command_maps[i].commands[j].name_long;
                    *name_short = nec_command_maps[i].commands[j].name_short;
                    return 1;
                }
            }
        }
    }
    *name_long = "Unknown";
    *name_short = "Unk";
    return 0;
}

static int compare_with_tolerance(nec_state* s, uint64_t measured, uint64_t base)
{
    return (measured >= (uint64_t)(base * (1.0 - s->tolerance))
        && measured <= (uint64_t)(base * (1.0 + s->tolerance)));
}

static uint8_t bitpack(uint8_t* bits, int count)
{
    uint8_t val = 0;
    int i;
    for (i = 0; i < count && i < 8; i++)
        val |= (bits[i] << i);
    return val;
}

static uint16_t bitpack16(uint8_t* bits, int count)
{
    uint16_t val = 0;
    int i;
    for (i = 0; i < count && i < 16; i++)
        val |= ((uint16_t)bits[i] << i);
    return val;
}

static void calc_rate(nec_state* s);

static void nec_metadata(struct srd_decoder_inst* di, int key, uint64_t value)
{
    nec_state* s = (nec_state*)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        calc_rate(s);
    }
}

static void nec_reset(struct srd_decoder_inst* di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(nec_state)));
    }
    nec_state* s = (nec_state*)c_decoder_get_private(di);
    memset(s, 0, sizeof(nec_state));
    s->state = STATE_IDLE;
    s->active = 1;
    s->prev_ir = -1;
}

static void nec_start(struct srd_decoder_inst* di)
{
    nec_state* s = (nec_state*)c_decoder_get_private(di);

    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ir_nec");

    const char* polarity = c_opt_str(di, "polarity", "active-low");
    if (polarity && strcmp(polarity, "active-high") == 0)
        s->active = 1;
    else if (polarity && strcmp(polarity, "active-low") == 0)
        s->active = 0;
    else
        s->active = -1;

    s->cd_freq = (int)c_opt_int(di, "cd_freq", 0);

    const char* extended = c_opt_str(di, "extended", "no");
    s->is_extended = (extended && strcmp(extended, "yes") == 0);
    s->want_addr_len = s->is_extended ? 16 : 8;

    s->samplerate = c_samplerate(di);
}

static void calc_rate(nec_state* s)
{
    if (s->samplerate == 0)
        return;

    s->tolerance = TIME_TOL / 100.0;
    s->lc = (uint64_t)(s->samplerate * TIME_LC / 1000.0) - 1;
    s->rc = (uint64_t)(s->samplerate * TIME_RC / 1000.0) - 1;
    s->dazero = (uint64_t)(s->samplerate * TIME_ZERO / 1000.0) - 1;
    s->daone = (uint64_t)(s->samplerate * TIME_ONE / 1000.0) - 1;
    s->stop = (uint64_t)(s->samplerate * TIME_STOP / 1000.0) - 1;
    s->idle_to = (uint64_t)(s->samplerate * TIME_IDLE / 1000.0) - 1;

    if (s->cd_freq > 0) {
        s->cd_count = (uint64_t)(s->samplerate / s->cd_freq) + 1;
        s->have_cd_count = 1;
    } else {
        s->have_cd_count = 0;
    }
}

static void putpause(struct srd_decoder_inst* di, nec_state* s, uint64_t es, int is_long)
{
    c_put(di, s->ss_start, s->ss_other_edge, s->out_ann, ANN_AGC,
        "AGC pulse", "AGC", "A");

    if (is_long) {
        c_put(di, s->ss_other_edge, es, s->out_ann, ANN_LONG_PAUSE,
            "Long pause", "L-pause", "LP", "P");
    } else {
        c_put(di, s->ss_other_edge, es, s->out_ann, ANN_SHORT_PAUSE,
            "Short pause", "S-pause", "SP", "P");
    }
}

static void putd(struct srd_decoder_inst* di, nec_state* s, uint64_t es, uint16_t data_val, int ann_class, const char* name, const char* short_name, const char* shortest, int bit_count)
{
    char long_str[64];
    char mid_str[32];
    char mid2_str[16];
    char short_str[8];

    if (bit_count <= 8) {
        snprintf(long_str, sizeof(long_str), "%s: 0x%02X", name, (uint8_t)data_val);
        snprintf(mid_str, sizeof(mid_str), "%s: 0x%02X", short_name, (uint8_t)data_val);
        snprintf(mid2_str, sizeof(mid2_str), "%s: 0x%02X", shortest, (uint8_t)data_val);
        snprintf(short_str, sizeof(short_str), "%s", shortest);
    } else {
        snprintf(long_str, sizeof(long_str), "%s: 0x%04X", name, data_val);
        snprintf(mid_str, sizeof(mid_str), "%s: 0x%04X", short_name, data_val);
        snprintf(mid2_str, sizeof(mid2_str), "%s: 0x%04X", shortest, data_val);
        snprintf(short_str, sizeof(short_str), "%s", shortest);
    }

    c_put(di, s->ss_start, es, s->out_ann, ann_class, long_str, mid_str, mid2_str, short_str);
}

static void putremote(struct srd_decoder_inst* di, nec_state* s)
{
    const char* dev = nec_lookup_address(s->addr);
    const char* btn_long;
    const char* btn_short;
    nec_lookup_command(s->addr, s->cmd, &btn_long, &btn_short);

    char str1[128];
    char str2[128];
    char str3[64];
    snprintf(str1, sizeof(str1), "%s: %s", dev, btn_long);
    snprintf(str2, sizeof(str2), "%s: %s", dev, btn_short);
    snprintf(str3, sizeof(str3), "%s", btn_short);

    c_put(di, s->ss_remote, s->ss_bit + s->stop, s->out_ann, ANN_REMOTE, str1, str2, str3);
}

static void handle_bit(struct srd_decoder_inst* di, nec_state* s, uint64_t es, uint64_t width)
{
    int ret = -1;
    if (compare_with_tolerance(s, width, s->dazero))
        ret = 0;
    else if (compare_with_tolerance(s, width, s->daone))
        ret = 1;

    if (ret == 0 || ret == 1) {
        char bit_str[16];
        snprintf(bit_str, sizeof(bit_str), "%d", ret);
        c_put(di, s->ss_bit, es, s->out_ann, ANN_BIT, bit_str);
        if (s->data_len < 32)
            s->data[s->data_len++] = ret;
    }
}

static int data_ok(struct srd_decoder_inst* di, nec_state* s, uint64_t es, int check, int want_len)
{
    uint8_t normal = bitpack(s->data, 8);
    uint8_t inverted = bitpack(s->data + 8, 8);
    int valid = ((normal ^ inverted) == 0xFF);

    if (s->is_extended && s->state == STATE_ADDRESS) {
        uint16_t ext_addr = bitpack16(s->data, s->data_len);
        putd(di, s, es, ext_addr, ANN_ADDR, "Address", "ADDR", "A", s->data_len);
        s->addr = ext_addr;
        s->data_len = 0;
        s->ss_bit = s->ss_start = es;
        return 1;
    }

    uint8_t show;
    if (s->state == STATE_ADDRESS_INV || s->state == STATE_COMMAND_INV)
        show = inverted;
    else
        show = normal;

    int ann_class;
    const char* name;
    const char* short_name;
    const char* shortest;

    switch (s->state) {
    case STATE_ADDRESS:
        ann_class = ANN_ADDR;
        name = "Address";
        short_name = "ADDR";
        shortest = "A";
        s->addr = normal;
        break;
    case STATE_ADDRESS_INV:
        ann_class = ANN_ADDR_INV;
        name = "Address#";
        short_name = "ADDR#";
        shortest = "A#";
        break;
    case STATE_COMMAND:
        ann_class = ANN_CMD;
        name = "Command";
        short_name = "CMD";
        shortest = "C";
        s->cmd = normal;
        break;
    case STATE_COMMAND_INV:
        ann_class = ANN_CMD_INV;
        name = "Command#";
        short_name = "CMD#";
        shortest = "C#";
        break;
    default:
        ann_class = ANN_WARN;
        name = "Unknown";
        short_name = "UNK";
        shortest = "?";
        break;
    }

    if (s->data_len == want_len) {
        putd(di, s, es, show, ann_class, name, short_name, shortest, want_len);
        s->ss_start = es;
        if (s->is_extended && s->state == STATE_ADDRESS) {
            s->data_len = 0;
            s->ss_bit = s->ss_start;
        }
        return 1;
    }

    putd(di, s, es, show, ann_class, name, short_name, shortest, want_len);

    if (check && !valid) {
        char warn_str[64];
        uint16_t warn_show = bitpack(s->data, 8);
        warn_show |= (uint16_t)bitpack(s->data + 8, 8) << 8;
        snprintf(warn_str, sizeof(warn_str), "%s error: 0x%04X", name, warn_show);
        c_put(di, s->ss_start, es, s->out_ann, ANN_WARN, warn_str);
    }

    s->data_len = 0;
    s->ss_bit = s->ss_start = es;
    return valid;
}

static void nec_decode(struct srd_decoder_inst* di)
{
    nec_state* s = (nec_state*)c_decoder_get_private(di);
    if (!s->samplerate) {
        s->samplerate = c_samplerate(di);
        if (s->samplerate > 0)
            calc_rate(s);
    }
    if (s->samplerate == 0)
        return;

    calc_rate(s);

    if (s->active == -1) {
        int ret = c_wait(di, CW_SKIP(0), CW_END);
        if (ret != SRD_OK)
            return;
        uint8_t curr_level = c_pin(di, IR_CH);
        s->active = 1 - curr_level;
    }

    while (1) {
        int ret;
        int ir_val;

        if (s->have_cd_count) {
            ret = c_wait(di, CW_E(IR_CH), CW_OR, CW_SKIP(s->cd_count), CW_END);
            if (ret != SRD_OK)
                return;

            if (di_matched(di) & (1ULL << 0)) {
                ir_val = s->active;
            } else {
                ir_val = c_pin(di, IR_CH);
            }

            if (ir_val == s->prev_ir)
                continue;
            s->prev_ir = ir_val;
        } else {
            ret = c_wait(di, CW_E(IR_CH), CW_END);
            if (ret != SRD_OK)
                return;

            ir_val = c_pin(di, IR_CH);
        }

        if (ir_val != s->active) {
            s->ss_other_edge = di_samplenum(di);
            if (s->state != STATE_STOP)
                continue;
        }

        uint64_t width = di_samplenum(di) - s->ss_bit;
        if (width >= s->idle_to && s->state != STATE_STOP) {
            s->state = STATE_IDLE;
            s->data_len = 0;
            s->ss_bit = 0;
            s->ss_start = 0;
            s->ss_other_edge = 0;
            s->ss_remote = 0;
        }

        switch (s->state) {

        case STATE_IDLE: {
            if (compare_with_tolerance(s, width, s->lc)) {
                putpause(di, s, di_samplenum(di), 1);
                c_put(di, s->ss_start, di_samplenum(di), s->out_ann, ANN_LEADER_CODE,
                    "Leader code", "Leader", "LC", "L");
                s->ss_remote = s->ss_start;
                s->data_len = 0;
                s->state = STATE_ADDRESS;
            } else if (compare_with_tolerance(s, width, s->rc)) {
                putpause(di, s, di_samplenum(di), 0);
                c_put(di, di_samplenum(di), di_samplenum(di) + s->stop, s->out_ann, ANN_STOP_BIT,
                    "Stop bit", "Stop", "St", "S");
                c_put(di, s->ss_start, di_samplenum(di) + s->stop, s->out_ann, ANN_REPEAT_CODE,
                    "Repeat code", "Repeat", "RC", "R");
                s->data_len = 0;
            }
            s->ss_bit = di_samplenum(di);
            s->ss_start = di_samplenum(di);
            break;
        }

        case STATE_ADDRESS: {
            handle_bit(di, s, di_samplenum(di), width);
            if (s->data_len == s->want_addr_len) {
                if (s->is_extended) {
                    data_ok(di, s, di_samplenum(di), 0, s->want_addr_len);
                    s->state = STATE_COMMAND;
                } else {
                    data_ok(di, s, di_samplenum(di), 0, s->want_addr_len);
                    s->state = STATE_ADDRESS_INV;
                }
            }
            s->ss_bit = di_samplenum(di);
            break;
        }

        case STATE_ADDRESS_INV: {
            handle_bit(di, s, di_samplenum(di), width);
            if (s->data_len == 16) {
                data_ok(di, s, di_samplenum(di), 1, 8);
                s->state = STATE_COMMAND;
            }
            s->ss_bit = di_samplenum(di);
            break;
        }

        case STATE_COMMAND: {
            handle_bit(di, s, di_samplenum(di), width);
            if (s->data_len == 8) {
                data_ok(di, s, di_samplenum(di), 0, 8);
                s->state = STATE_COMMAND_INV;
            }
            s->ss_bit = di_samplenum(di);
            break;
        }

        case STATE_COMMAND_INV: {
            handle_bit(di, s, di_samplenum(di), width);
            if (s->data_len == 16) {
                data_ok(di, s, di_samplenum(di), 1, 8);
                s->state = STATE_STOP;
            }
            s->ss_bit = di_samplenum(di);
            break;
        }

        case STATE_STOP: {
            c_put(di, s->ss_bit, s->ss_bit + s->stop, s->out_ann, ANN_STOP_BIT,
                "Stop bit", "Stop", "St", "S");
            putremote(di, s);
            s->ss_bit = di_samplenum(di);
            s->ss_start = di_samplenum(di);
            s->state = STATE_IDLE;
            s->data_len = 0;
            break;
        }
        }
    }
}

static void nec_destroy(struct srd_decoder_inst* di)
{
    void* priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ir_nec_c_decoder = {
    .id = "ir_nec_c",
    .name = "IR NEC(C)",
    .longname = "NEC infrared remote control protocol (C)",
    .desc = "NEC IR remote control protocol decoder (C implementation)",
    .license = "gplv2+",
    .channels = nec_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = nec_options_arr,
    .num_options = 3,
    .num_annotations = 13,
    .ann_labels = nec_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = nec_ann_rows,
    .inputs = nec_inputs,
    .num_inputs = 1,
    .outputs = nec_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = nec_tags,
    .num_tags = 1,
    .metadata = nec_metadata,
    .reset = nec_reset,
    .start = nec_start,
    .decode = nec_decode,
    .destroy = nec_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder* srd_c_decoder_entry(void)
{
    GVariant* polarity_vals[] = {
        g_variant_new_string("auto"),
        g_variant_new_string("active-low"),
        g_variant_new_string("active-high"),
    };
    GSList* polarity_list = NULL;
    polarity_list = g_slist_append(polarity_list, polarity_vals[0]);
    polarity_list = g_slist_append(polarity_list, polarity_vals[1]);
    polarity_list = g_slist_append(polarity_list, polarity_vals[2]);
    nec_options_arr[0].id = "polarity";
    nec_options_arr[0].idn = "dec_ir_nec_opt_polarity";
    nec_options_arr[0].desc = "Polarity";
    nec_options_arr[0].def = g_variant_new_string("active-low");
    nec_options_arr[0].values = polarity_list;

    nec_options_arr[1].id = "cd_freq";
    nec_options_arr[1].idn = "dec_ir_nec_opt_cd_freq";
    nec_options_arr[1].desc = "Carrier Frequency";
    nec_options_arr[1].def = g_variant_new_int64(0);
    nec_options_arr[1].values = NULL;

    GVariant* extended_vals[] = {
        g_variant_new_string("yes"),
        g_variant_new_string("no"),
    };
    GSList* extended_list = NULL;
    extended_list = g_slist_append(extended_list, extended_vals[0]);
    extended_list = g_slist_append(extended_list, extended_vals[1]);
    nec_options_arr[2].id = "extended";
    nec_options_arr[2].idn = "dec_ir_nec_opt_extended";
    nec_options_arr[2].desc = "Extended NEC Protocol";
    nec_options_arr[2].def = g_variant_new_string("no");
    nec_options_arr[2].values = extended_list;

    return &ir_nec_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}