#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum rc5_state {
    STATE_IDLE,
    STATE_MID1,
    STATE_MID0,
    STATE_START1,
    STATE_START0,
};

enum rc5_ann {
    ANN_BIT = 0,
    ANN_STARTBIT1,
    ANN_STARTBIT2,
    ANN_TOGGLEBIT0,
    ANN_TOGGLEBIT1,
    ANN_ADDRESS,
    ANN_COMMAND,
    NUM_ANN,
};

#define IR_CH 0
#define MAX_EDGES 32
#define MAX_BITS 14

struct rc5_priv {
    enum rc5_state state;
    uint64_t samplerate;
    uint64_t halfbit;
    uint64_t edges[MAX_EDGES];
    int num_edges;
    uint64_t bits_ss[MAX_BITS];
    int bits_val[MAX_BITS];
    int num_bits;
    uint64_t ss_es_bits_ss[MAX_BITS];
    uint64_t ss_es_bits_es[MAX_BITS];
    int num_ss_es_bits;
    int next_edge_is_low;
    int is_extended;
    int out_ann;
};

static struct srd_channel rc5_channels[] = {
    { "ir", "IR", "IR data line", 0, SRD_CHANNEL_SDATA, "dec_ir_rc5_chan_ir" },
};

static struct srd_decoder_option rc5_options_arr[2];

static const char* rc5_ann_labels[][3] = {
    { "", "bit", "Bit" },
    { "", "startbit1", "Startbit 1" },
    { "", "startbit2", "Startbit 2" },
    { "", "togglebit-0", "Toggle bit 0" },
    { "", "togglebit-1", "Toggle bit 1" },
    { "", "address", "Address" },
    { "", "command", "Command" },
};

static const int rc5_row_bits_classes[] = { ANN_BIT, -1 };
static const int rc5_row_fields_classes[] = { ANN_STARTBIT1, ANN_STARTBIT2, ANN_TOGGLEBIT0, ANN_TOGGLEBIT1, ANN_ADDRESS, ANN_COMMAND, -1 };
static const struct srd_c_ann_row rc5_ann_rows[] = {
    { "bits", "Bits", rc5_row_bits_classes, 1 },
    { "fields", "Fields", rc5_row_fields_classes, 6 },
};

static const char* rc5_inputs[] = { "logic", NULL };
static const char* rc5_outputs[] = { NULL };
static const char* rc5_tags[] = { "IR", NULL };

/* System name lookup table (from ir_rc5/lists.py) */
typedef struct {
    int addr;
    const char* name_long;
    const char* name_short;
} rc5_system_entry;

static const rc5_system_entry rc5_system_table[] = {
    { 0, "TV receiver 1", "TV1" },
    { 1, "TV receiver 2", "TV2" },
    { 2, "Teletext", "Txt" },
    { 3, "Extension to TV1 and TV2", "Ext TV1/TV2" },
    { 4, "LaserVision player", "LV" },
    { 5, "Video cassette recorder 1", "VCR1" },
    { 6, "Video cassette recorder 2", "VCR2" },
    { 7, "Experimental", "Exp" },
    { 8, "Satellite TV receiver 1", "Sat1" },
    { 9, "Extension to VCR1 and VCR2", "Ext VCR1/VCR2" },
    { 10, "Satellite TV receiver 2", "Sat2" },
    { 12, "Compact disc video player", "CD-Video" },
    { 13, "Camcorder", "Cam" },
    { 14, "Photo on compact disc player", "CD-Photo" },
    { 16, "Audio preamplifier 1", "Preamp1" },
    { 17, "Radio tuner", "Tuner" },
    { 18, "Analog cassette recoder 1", "Rec1" },
    { 19, "Audio preamplifier 2", "Preamp2" },
    { 20, "Compact disc player", "CD" },
    { 21, "Audio stack or record player", "Combi" },
    { 22, "Audio satellite", "Sat" },
    { 23, "Analog cassette recoder 2", "Rec2" },
    { 26, "Compact disc recorder", "CD-R" },
    { 29, "Lighting 1", "Light1" },
    { 30, "Lighting 2", "Light2" },
    { 31, "Telephone", "Phone" },
};

#define RC5_SYSTEM_TABLE_SIZE (sizeof(rc5_system_table) / sizeof(rc5_system_table[0]))

/* Command name lookup tables (from ir_rc5/lists.py) */
typedef struct {
    int cmd;
    const char* name_long;
    const char* name_short;
} rc5_command_entry;

static const rc5_command_entry rc5_tv_commands[] = {
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
    { 10, "-/--", "-/--" },
    { 11, "Channel/program", "Ch/P" },
    { 12, "Standby", "StBy" },
    { 13, "Mute", "M" },
    { 14, "Personal preferences", "PP" },
    { 15, "Display", "Disp" },
    { 16, "Volume up", "Vol+" },
    { 17, "Volume down", "Vol-" },
    { 18, "Brightness up", "Br+" },
    { 19, "Brightness down", "Br-" },
    { 20, "Saturation up", "S+" },
    { 21, "Saturation down", "S-" },
    { 32, "Program up", "P+" },
    { 33, "Program down", "P-" },
};

#define RC5_TV_COMMANDS_SIZE (sizeof(rc5_tv_commands) / sizeof(rc5_tv_commands[0]))

static const rc5_command_entry rc5_vcr_commands[] = {
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
    { 10, "-/--", "-/--" },
    { 12, "Standby", "StBy" },
    { 32, "Program up", "P+" },
    { 33, "Program down", "P-" },
    { 50, "Fast rewind", "FRW" },
    { 52, "Fast forward", "FFW" },
    { 53, "Play", "Pl" },
    { 54, "Stop", "St" },
    { 55, "Recording", "Rec" },
};

#define RC5_VCR_COMMANDS_SIZE (sizeof(rc5_vcr_commands) / sizeof(rc5_vcr_commands[0]))

static const rc5_system_entry* rc5_lookup_system(int addr)
{
    int i;
    for (i = 0; i < (int)RC5_SYSTEM_TABLE_SIZE; i++) {
        if (rc5_system_table[i].addr == addr)
            return &rc5_system_table[i];
    }
    return NULL;
}

static const rc5_command_entry* rc5_lookup_command(const rc5_command_entry* table, int table_size, int cmd)
{
    int i;
    for (i = 0; i < table_size; i++) {
        if (table[i].cmd == cmd)
            return &table[i];
    }
    return NULL;
}

static char rc5_edge_type(struct rc5_priv* s, uint64_t samplenum)
{
    (void)samplenum;
    uint64_t distance = samplenum - s->edges[s->num_edges - 1];
    uint64_t half = s->halfbit;
    uint64_t long_dist = half * 2;
    uint64_t margin = half / 2;

    if (distance >= long_dist - margin && distance <= long_dist + margin)
        return 'l';
    if (distance >= half - margin && distance <= half + margin)
        return 's';
    return 'e';
}

static void rc5_handle_bits(struct srd_decoder_inst* di, struct rc5_priv* s)
{
    int i;
    int a = 0, c = 0;
    uint64_t ss, es;

    s->num_ss_es_bits = 0;
    for (i = 0; i < s->num_bits; i++) {
        
        if (i == 0) {
            ss = (s->bits_ss[0] > s->halfbit) ? (s->bits_ss[0] - s->halfbit) : 0;
        } else {
            ss = s->ss_es_bits_es[i - 1];
        }
        es = s->bits_ss[i] + s->halfbit;
        s->ss_es_bits_ss[i] = ss;
        s->ss_es_bits_es[i] = es;
        s->num_ss_es_bits++;

        char bit_str[16];
        snprintf(bit_str, sizeof(bit_str), "%d", s->bits_val[i]);
        c_put(di, ss, es, s->out_ann, ANN_BIT, bit_str);
    }

    {
        char str1[32], str2[16], str3[8], str4[8], str5[4];
        snprintf(str1, sizeof(str1), "Startbit1: %d", s->bits_val[0]);
        snprintf(str2, sizeof(str2), "SB1: %d", s->bits_val[0]);
        snprintf(str3, sizeof(str3), "SB1");
        snprintf(str4, sizeof(str4), "S1");
        snprintf(str5, sizeof(str5), "S");
        c_put(di, s->ss_es_bits_ss[0], s->ss_es_bits_es[0], s->out_ann, ANN_STARTBIT1,
            str1, str2, str3, str4, str5);
    }

    {
        int ann_idx = ANN_STARTBIT2;
        if (s->is_extended) {
            char str1[32], str2[16], str3[8], str4[8], str5[4];
            snprintf(str1, sizeof(str1), "CMD[6]#: %d", s->bits_val[1]);
            snprintf(str2, sizeof(str2), "C6#: %d", s->bits_val[1]);
            snprintf(str3, sizeof(str3), "C6#");
            snprintf(str4, sizeof(str4), "C#");
            snprintf(str5, sizeof(str5), "C");
            ann_idx = ANN_COMMAND;
            c_put(di, s->ss_es_bits_ss[1], s->ss_es_bits_es[1], s->out_ann, ann_idx,
                str1, str2, str3, str4, str5);
        } else {
            char str1[32], str2[16], str3[8], str4[8], str5[4];
            snprintf(str1, sizeof(str1), "Startbit2: %d", s->bits_val[1]);
            snprintf(str2, sizeof(str2), "SB2: %d", s->bits_val[1]);
            snprintf(str3, sizeof(str3), "SB2");
            snprintf(str4, sizeof(str4), "S2");
            snprintf(str5, sizeof(str5), "S");
            c_put(di, s->ss_es_bits_ss[1], s->ss_es_bits_es[1], s->out_ann, ann_idx,
                str1, str2, str3, str4, str5);
        }
    }

    {
        int ann_idx = (s->bits_val[2] == 0) ? ANN_TOGGLEBIT0 : ANN_TOGGLEBIT1;
        char str1[32], str2[16], str3[16], str4[8], str5[4];
        snprintf(str1, sizeof(str1), "Togglebit: %d", s->bits_val[2]);
        snprintf(str2, sizeof(str2), "Toggle: %d", s->bits_val[2]);
        snprintf(str3, sizeof(str3), "TB: %d", s->bits_val[2]);
        snprintf(str4, sizeof(str4), "TB");
        snprintf(str5, sizeof(str5), "T");
        c_put(di, s->ss_es_bits_ss[2], s->ss_es_bits_es[2], s->out_ann, ann_idx,
            str1, str2, str3, str4, str5);
    }

    for (i = 0; i < 5; i++)
        a |= (s->bits_val[3 + i] << (4 - i));
    {
        const rc5_system_entry* sys = rc5_lookup_system(a);
        const char* sys_long = sys ? sys->name_long : "Unknown";
        const char* sys_short = sys ? sys->name_short : "Unk";
        char str1[64], str2[64], str3[32], str4[16], str5[4];
        snprintf(str1, sizeof(str1), "Address: %d (%s)", a, sys_long);
        snprintf(str2, sizeof(str2), "Addr: %d (%s)", a, sys_short);
        snprintf(str3, sizeof(str3), "Addr: %d", a);
        snprintf(str4, sizeof(str4), "A: %d", a);
        snprintf(str5, sizeof(str5), "A");
        c_put(di, s->ss_es_bits_ss[3], s->ss_es_bits_es[7], s->out_ann, ANN_ADDRESS,
            str1, str2, str3, str4, str5);
    }

    for (i = 0; i < 6; i++)
        c |= (s->bits_val[8 + i] << (5 - i));
    if (s->is_extended) {
        int inverted_bit6 = (s->bits_val[1] == 0) ? 1 : 0;
        c |= (inverted_bit6 << 6);
    }
    {
        const rc5_system_entry* sys = rc5_lookup_system(a);
        const char* sys_short = sys ? sys->name_short : "Unk";
        int is_vcr = (sys_short && (strcmp(sys_short, "VCR1") == 0 || strcmp(sys_short, "VCR2") == 0));
        const rc5_command_entry* cmd_entry;
        if (is_vcr)
            cmd_entry = rc5_lookup_command(rc5_vcr_commands, RC5_VCR_COMMANDS_SIZE, c);
        else
            cmd_entry = rc5_lookup_command(rc5_tv_commands, RC5_TV_COMMANDS_SIZE, c);
        const char* cmd_long = cmd_entry ? cmd_entry->name_long : "Unknown";
        const char* cmd_short = cmd_entry ? cmd_entry->name_short : "Unk";
        char str1[64], str2[64], str3[32], str4[16], str5[4];
        snprintf(str1, sizeof(str1), "Command: %d (%s)", c, cmd_long);
        snprintf(str2, sizeof(str2), "Cmd: %d (%s)", c, cmd_short);
        snprintf(str3, sizeof(str3), "Cmd: %d", c);
        snprintf(str4, sizeof(str4), "C: %d", c);
        snprintf(str5, sizeof(str5), "C");
        c_put(di, s->ss_es_bits_ss[8], s->ss_es_bits_es[13], s->out_ann, ANN_COMMAND,
            str1, str2, str3, str4, str5);
    }
}

static void rc5_reset(struct srd_decoder_inst* di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(struct rc5_priv)));
    }
    struct rc5_priv* s = (struct rc5_priv*)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct rc5_priv));
    s->state = STATE_IDLE;
}

static void rc5_start(struct srd_decoder_inst* di)
{
    struct rc5_priv* s = (struct rc5_priv*)c_decoder_get_private(di);

    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ir_rc5");

    const char* polarity = c_opt_str(di, "polarity", "active-low");
    s->next_edge_is_low = (strcmp(polarity, "active-low") == 0) ? 1 : 0;

    const char* protocol = c_opt_str(di, "protocol", "standard");
    s->is_extended = (strcmp(protocol, "extended") == 0) ? 1 : 0;

    s->samplerate = c_samplerate(di);
    if (s->samplerate)
        s->halfbit = (uint64_t)((double)s->samplerate * 0.00178 / 2.0);
}

static void rc5_metadata(struct srd_decoder_inst* di, int key, uint64_t value)
{
    struct rc5_priv* s = (struct rc5_priv*)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        s->halfbit = (uint64_t)((double)value * 0.00178 / 2.0);
    }
}

static void rc5_decode(struct srd_decoder_inst* di)
{
    struct rc5_priv* s = (struct rc5_priv*)c_decoder_get_private(di);
    if (!s->samplerate)
        return;

    while (1) {
        int ret;
        if (s->next_edge_is_low)
            ret = c_wait(di, CW_L(IR_CH), CW_END);
        else
            ret = c_wait(di, CW_H(IR_CH), CW_END);
        if (ret != SRD_OK)
            return;

        int ir = c_pin(di, IR_CH);

        if (s->state == STATE_IDLE) {
            s->num_edges = 0;
            s->num_bits = 0;
            s->edges[0] = di_samplenum(di);
            s->num_edges = 1;
            s->bits_ss[0] = di_samplenum(di);
            s->bits_val[0] = 1;
            s->num_bits = 1;
            s->state = STATE_MID1;
            s->next_edge_is_low = ir ? 1 : 0;
            continue;
        }

        char etype = rc5_edge_type(s, di_samplenum(di));
        if (etype == 'e') {
            s->num_edges = 0;
            s->num_bits = 0;
            s->state = STATE_IDLE;
            continue;
        }

        int bit = -1;

        if (s->state == STATE_MID1) {
            if (etype == 's') {
                s->state = STATE_START1;
                bit = -1;
            } else {
                s->state = STATE_MID0;
                bit = 0;
            }
        } else if (s->state == STATE_MID0) {
            if (etype == 's') {
                s->state = STATE_START0;
                bit = -1;
            } else {
                s->state = STATE_MID1;
                bit = 1;
            }
        } else if (s->state == STATE_START1) {
            if (etype == 's') {
                s->state = STATE_MID1;
                bit = 1;
            } else {
                bit = -1;
            }
        } else if (s->state == STATE_START0) {
            if (etype == 's') {
                s->state = STATE_MID0;
                bit = 0;
            } else {
                bit = -1;
            }
        }

        if (s->num_edges < MAX_EDGES) {
            s->edges[s->num_edges] = di_samplenum(di);
            s->num_edges++;
        }

        if (bit >= 0 && s->num_bits < MAX_BITS) {
            s->bits_ss[s->num_bits] = di_samplenum(di);
            s->bits_val[s->num_bits] = bit;
            s->num_bits++;
        }

        if (s->num_bits == MAX_BITS) {
            rc5_handle_bits(di, s);
            s->num_edges = 0;
            s->num_bits = 0;
            s->state = STATE_IDLE;
        }

        s->next_edge_is_low = ir ? 1 : 0;
    }
}

static void rc5_destroy(struct srd_decoder_inst* di)
{
    void* priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ir_rc5_c_decoder = {
    .id = "ir_rc5_c",
    .name = "IR RC-5(C)",
    .longname = "IR RC-5(C)",
    .desc = "RC-5 infrared remote control protocol (C implementation)",
    .license = "gplv2+",
    .channels = rc5_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = rc5_options_arr,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = rc5_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = rc5_ann_rows,
    .inputs = rc5_inputs,
    .num_inputs = 1,
    .outputs = rc5_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = rc5_tags,
    .num_tags = 1,
    .reset = rc5_reset,
    .start = rc5_start,
    .decode = rc5_decode,
    .end = NULL,
    .metadata = rc5_metadata,
    .destroy = rc5_destroy,
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
    rc5_options_arr[0].id = "polarity";
    rc5_options_arr[0].idn = "dec_ir_rc5_opt_polarity";
    rc5_options_arr[0].desc = "Polarity";
    rc5_options_arr[0].def = g_variant_new_string("active-low");
    rc5_options_arr[0].values = polarity_list;

    GVariant* protocol_vals[] = {
        g_variant_new_string("standard"),
        g_variant_new_string("extended"),
    };
    GSList* protocol_list = NULL;
    protocol_list = g_slist_append(protocol_list, protocol_vals[0]);
    protocol_list = g_slist_append(protocol_list, protocol_vals[1]);
    rc5_options_arr[1].id = "protocol";
    rc5_options_arr[1].idn = "dec_ir_rc5_opt_protocol";
    rc5_options_arr[1].desc = "Protocol type";
    rc5_options_arr[1].def = g_variant_new_string("standard");
    rc5_options_arr[1].values = protocol_list;

    return &ir_rc5_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}