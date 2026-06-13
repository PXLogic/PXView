#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_BIT = 0,
    ANN_REF,
    ANN_FIELD,
    ANN_REF_FIELD,
    ANN_L2,
    ANN_REF_L2,
    NUM_ANN,
};

enum ookvis_display {
    OOKVIS_BYTE_HEX = 0,
    OOKVIS_BYTE_HEX_REV,
    OOKVIS_BYTE_BCD,
    OOKVIS_BYTE_BCD_REV,
    OOKVIS_NIBBLE_HEX,
    OOKVIS_NIBBLE_HEX_REV,
    OOKVIS_NIBBLE_BCD,
    OOKVIS_NIBBLE_BCD_REV,
};

#define OOK_VIS_MAX_BITS 4096
#define OOK_VIS_MAX_CACHE_TRACES 30
#define OOK_VIS_MAX_CACHE_BITS 1024

typedef struct {
    int out_ann;
    int out_python;
    enum ookvis_display displayas;
    int sync_length;
    int sync_offset;
    int ref_sample; /* 0=off, -1=show numbers, 1~30=ref index */
    /* Current trace */
    uint64_t *ss_arr;
    uint64_t *es_arr;
    char *bit_arr;
    int num_bits;
    int arr_capacity;
    char ookstring[OOK_VIS_MAX_BITS];
    int ook_len;
    int decode_pos;
    int trace_num;
    /* Cache for reference comparison */
    int cache_count;
    char cache_bits[OOK_VIS_MAX_CACHE_TRACES][OOK_VIS_MAX_CACHE_BITS];
    int cache_lens[OOK_VIS_MAX_CACHE_TRACES];
} ookvis_state;

static const char *ookvis_inputs[] = {"ook", NULL};
static const char *ookvis_outputs[] = {"ook", NULL};
static const char *ookvis_tags[] = {"Encoding", NULL};

static struct srd_decoder_option ookvis_options[] = {
    {"displayas", "dec_ook_vis_opt_displayas", "Display as", NULL, NULL},
    {"synclen", "dec_ook_vis_opt_synclen", "Sync length", NULL, NULL},
    {"syncoffset", "dec_ook_vis_opt_syncoffset", "Sync offset", NULL, NULL},
    {"refsample", "dec_ook_vis_opt_refsample", "Compare", NULL, NULL},
};

static const char *ookvis_ann_labels[][3] = {
    {"", "Bit", "Bit"},
    {"", "Reference", "Reference"},
    {"", "Field", "Field"},
    {"", "Ref field", "Ref field"},
    {"", "L2", "L2"},
    {"", "Ref L2", "Ref L2"},
};

static const int ookvis_row_bits_classes[] = {ANN_BIT, -1};
static const int ookvis_row_compare_classes[] = {ANN_REF, -1};
static const int ookvis_row_fields_classes[] = {ANN_FIELD, -1};
static const int ookvis_row_ref_fields_classes[] = {ANN_REF_FIELD, -1};
static const int ookvis_row_l2_classes[] = {ANN_L2, -1};
static const int ookvis_row_ref_l2_classes[] = {ANN_REF_L2, -1};

static const struct srd_c_ann_row ookvis_ann_rows[] = {
    {"bits", "Bits", ookvis_row_bits_classes, 1},
    {"compare", "Compare", ookvis_row_compare_classes, 1},
    {"fields", "Fields", ookvis_row_fields_classes, 1},
    {"ref_fields", "Ref fields", ookvis_row_ref_fields_classes, 1},
    {"level2", "L2", ookvis_row_l2_classes, 1},
    {"ref_level2", "Ref L2", ookvis_row_ref_l2_classes, 1},
};

static int bcd2int_val(unsigned int v)
{
    return ((v >> 4) & 0x0f) * 10 + (v & 0x0f);
}

static int ookvis_get_bits_width(ookvis_state *s)
{
    if (s->displayas >= OOKVIS_NIBBLE_HEX)
        return 4;
    return 8;
}

static void ookvis_put_field(struct srd_decoder_inst *di, ookvis_state *s,
                              int numbits, int line)
{
    char param[64];
    int plen = 0;
    for (int i = 0; i < numbits && (s->decode_pos + i) < s->ook_len; i++)
        param[plen++] = s->ookstring[s->decode_pos + i];
    param[plen] = '\0';

    if (s->displayas == OOKVIS_BYTE_HEX_REV || s->displayas == OOKVIS_NIBBLE_HEX_REV ||
        s->displayas == OOKVIS_BYTE_BCD_REV || s->displayas == OOKVIS_NIBBLE_BCD_REV) {
        /* Reverse */
        for (int i = 0; i < plen / 2; i++) {
            char tmp = param[i];
            param[i] = param[plen - 1 - i];
            param[plen - 1 - i] = tmp;
        }
    }

    if (!strchr(param, 'E')) {
        unsigned int val = strtol(param, NULL, 2);
        if (s->displayas == OOKVIS_BYTE_HEX || s->displayas == OOKVIS_BYTE_HEX_REV ||
            s->displayas == OOKVIS_NIBBLE_HEX || s->displayas == OOKVIS_NIBBLE_HEX_REV) {
            snprintf(param, sizeof(param), "%X", val);
        } else {
            snprintf(param, sizeof(param), "%d", bcd2int_val(val));
        }
    }

    if (s->decode_pos < s->num_bits && (s->decode_pos + numbits - 1) < s->num_bits)
        c_put(di, s->ss_arr[s->decode_pos], s->es_arr[s->decode_pos + numbits - 1],
                  s->out_ann, line, param);

    s->decode_pos += numbits;
}

static void ookvis_display_level2(struct srd_decoder_inst *di, ookvis_state *s,
                                   int bits, int line)
{
    s->decode_pos = 0;
    if (s->num_bits <= 1)
        return;

    /* Find the end of the preamble which could be 1010 or 1111 */
    char char_first = s->bit_arr[0];
    char char_second = s->bit_arr[1];
    int preamble_end = s->num_bits + 1;
    char preamble[5];
    char char_last;

    if (char_first == char_second) {
        strcpy(preamble, "1111");
        char_last = char_first;
    } else {
        strcpy(preamble, "1010");
        char_last = char_second;
    }

    for (int i = 0; i < s->num_bits; i++) {
        if (strcmp(preamble, "1111") == 0) {
            if (s->bit_arr[i] != char_last) {
                preamble_end = i;
                break;
            }
        } else {
            if (s->bit_arr[i] != char_last)
                char_last = s->bit_arr[i];
            else {
                preamble_end = i;
                break;
            }
        }
    }

    if (s->num_bits >= preamble_end) {
        preamble_end += s->sync_offset - 1;
        if (preamble_end < 0) preamble_end = 0;
        if (preamble_end >= s->num_bits) preamble_end = s->num_bits - 1;

        c_put(di, s->ss_arr[0], s->es_arr[preamble_end], s->out_ann, line, "Preamble");
        s->decode_pos = preamble_end;

        if (s->num_bits > s->decode_pos + s->sync_length) {
            int synNULL = s->decode_pos + s->sync_length;
            c_put(di, s->ss_arr[s->decode_pos], s->es_arr[synNULL], s->out_ann, line, "Sync");
            s->decode_pos = synNULL + 1;
        }

        /* Display remaining nibbles/bytes */
        int rem = (s->ook_len - s->decode_pos) / bits;
        for (int i = 0; i < rem; i++) {
            if (s->decode_pos + bits - 1 < s->num_bits)
                ookvis_put_field(di, s, bits, line);
            else
                break;
        }
    }
}

static void ookvis_display_all(struct srd_decoder_inst *di, ookvis_state *s)
{
    s->ook_len = 0;
    s->decode_pos = 0;

    for (int i = 0; i < s->num_bits && s->ook_len < OOK_VIS_MAX_BITS - 1; i++)
        s->ookstring[s->ook_len++] = s->bit_arr[i];
    s->ookstring[s->ook_len] = '\0';

    int bits = ookvis_get_bits_width(s);
    int rem_nibbles = s->ook_len / bits;

    for (int i = 0; i < rem_nibbles; i++) {
        if (s->decode_pos + bits - 1 < s->num_bits)
            ookvis_put_field(di, s, bits, ANN_FIELD);
        else
            break;
    }

    ookvis_display_level2(di, s, bits, ANN_L2);

    /* Reference comparison */
    if (s->ref_sample > 0 && s->cache_count >= s->ref_sample) {
        int ref = s->ref_sample - 1;
        if (ref < s->cache_count) {
            int display_len = s->cache_lens[ref];
            if (s->num_bits < display_len)
                display_len = s->num_bits;
            for (int i = 0; i < display_len; i++) {
                char bit_str[2] = {s->cache_bits[ref][i], '\0'};
                c_put(di, s->ss_arr[i], s->es_arr[i], s->out_ann, ANN_REF, bit_str);
            }
        }
    } else if (s->ref_sample == -1) {
        /* Show numbers */
        if (s->trace_num < s->cache_count) {
            char num_str[16];
            snprintf(num_str, sizeof(num_str), "%d", s->trace_num + 1);
            c_put(di, s->ss_arr[0], s->es_arr[s->num_bits - 1],
                      s->out_ann, ANN_REF, num_str);
        }
    }
}

static void ookvis_add_to_cache(ookvis_state *s)
{
    if (s->cache_count >= OOK_VIS_MAX_CACHE_TRACES)
        return;
    int idx = s->cache_count;
    int len = s->num_bits;
    if (len > OOK_VIS_MAX_CACHE_BITS)
        len = OOK_VIS_MAX_CACHE_BITS;
    memcpy(s->cache_bits[idx], s->bit_arr, len);
    s->cache_lens[idx] = len;
    s->cache_count++;
}

static void ookvis_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    ookvis_state *s = (ookvis_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "DATA") == 0) {
        /* Parse bit list from data */
        int num_entries = (int)(n_fields / 9);
        s->num_bits = 0;
        s->ook_len = 0;
        s->decode_pos = 0;

        if (num_entries > s->arr_capacity) {
            if (s->ss_arr) g_free(s->ss_arr);
            if (s->es_arr) g_free(s->es_arr);
            if (s->bit_arr) g_free(s->bit_arr);
            s->arr_capacity = num_entries + 256;
            s->ss_arr = (uint64_t *)g_malloc(s->arr_capacity * sizeof(uint64_t));
            s->es_arr = (uint64_t *)g_malloc(s->arr_capacity * sizeof(uint64_t));
            s->bit_arr = (char *)g_malloc(s->arr_capacity);
        }

        for (int i = 0; i < num_entries; i++) {
            const c_field *p = fields + i * 9;
            uint64_t ss = 0, es = 0;
            for (int j = 0; j < 8; j++)
                ss |= ((uint64_t)p[j].u8) << (j * 8);
            for (int j = 0; j < 8; j++)
                es |= ((uint64_t)p[4 + j].u8) << (j * 8);
            s->ss_arr[i] = ss;
            s->es_arr[i] = es;
            s->bit_arr[i] = p[8].u8;
            s->num_bits++;
        }

        ookvis_add_to_cache(s);
        ookvis_display_all(di, s);
        s->trace_num++;
    }
}

static void ookvis_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        ookvis_state *s = (ookvis_state *)g_malloc0(sizeof(ookvis_state));
        s->ss_arr = NULL;
        s->es_arr = NULL;
        s->bit_arr = NULL;
        s->arr_capacity = 0;
        c_decoder_set_private(di, s);
    }
    ookvis_state *s = (ookvis_state *)c_decoder_get_private(di);
    s->num_bits = 0;
    s->ook_len = 0;
    s->decode_pos = 0;
    s->trace_num = 0;
    s->cache_count = 0;
    s->displayas = OOKVIS_NIBBLE_HEX;
    s->sync_length = 4;
    s->sync_offset = 0;
    s->ref_sample = 0;
}

static void ookvis_start(struct srd_decoder_inst *di)
{
    ookvis_state *s = (ookvis_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ook_vis");

    const char *displayas = c_opt_str(di, "displayas", "Nibble - Hex");
    if (displayas) {
        if (strcmp(displayas, "Byte - Hex") == 0) s->displayas = OOKVIS_BYTE_HEX;
        else if (strcmp(displayas, "Byte - Hex rev") == 0) s->displayas = OOKVIS_BYTE_HEX_REV;
        else if (strcmp(displayas, "Byte - BCD") == 0) s->displayas = OOKVIS_BYTE_BCD;
        else if (strcmp(displayas, "Byte - BCD rev") == 0) s->displayas = OOKVIS_BYTE_BCD_REV;
        else if (strcmp(displayas, "Nibble - Hex") == 0) s->displayas = OOKVIS_NIBBLE_HEX;
        else if (strcmp(displayas, "Nibble - Hex rev") == 0) s->displayas = OOKVIS_NIBBLE_HEX_REV;
        else if (strcmp(displayas, "Nibble - BCD") == 0) s->displayas = OOKVIS_NIBBLE_BCD;
        else if (strcmp(displayas, "Nibble - BCD rev") == 0) s->displayas = OOKVIS_NIBBLE_BCD_REV;
    }

    const char *synclen = c_opt_str(di, "synclen", "4");
    if (synclen) s->sync_length = atoi(synclen);

    const char *syncoffset = c_opt_str(di, "syncoffset", "0");
    if (syncoffset) s->sync_offset = atoi(syncoffset);

    const char *refsample = c_opt_str(di, "refsample", "off");
    if (refsample) {
        if (strcmp(refsample, "off") == 0) s->ref_sample = 0;
        else if (strcmp(refsample, "show numbers") == 0) s->ref_sample = -1;
        else s->ref_sample = atoi(refsample);
    }
}

static void ookvis_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void ookvis_destroy(struct srd_decoder_inst *di)
{
    ookvis_state *s = (ookvis_state *)c_decoder_get_private(di);
    if (s) {
        if (s->ss_arr) g_free(s->ss_arr);
        if (s->es_arr) g_free(s->es_arr);
        if (s->bit_arr) g_free(s->bit_arr);
        g_free(s);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ook_vis_c_decoder = {
    .id = "ook_vis_c",
    .name = "OOK visualisation(C)",
    .longname = "On-off keying visualisation (C)",
    .desc = "OOK visualisation in various formats. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = ookvis_options,
    .num_options = 4,
    .num_annotations = NUM_ANN,
    .ann_labels = ookvis_ann_labels,
    .num_annotation_rows = 6,
    .annotation_rows = ookvis_ann_rows,
    .inputs = ookvis_inputs,
    .num_inputs = 1,
    .outputs = ookvis_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = ookvis_tags,
    .num_tags = 1,
    .reset = ookvis_reset,
    .start = ookvis_start,
    .decode = ookvis_decode,
    .destroy = ookvis_destroy,
    .decode_upper = ookvis_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    ookvis_options[0].def = g_variant_new_string("Nibble - Hex");
    GSList *disp_vals = NULL;
    disp_vals = g_slist_append(disp_vals, g_variant_new_string("Byte - Hex"));
    disp_vals = g_slist_append(disp_vals, g_variant_new_string("Byte - Hex rev"));
    disp_vals = g_slist_append(disp_vals, g_variant_new_string("Byte - BCD"));
    disp_vals = g_slist_append(disp_vals, g_variant_new_string("Byte - BCD rev"));
    disp_vals = g_slist_append(disp_vals, g_variant_new_string("Nibble - Hex"));
    disp_vals = g_slist_append(disp_vals, g_variant_new_string("Nibble - Hex rev"));
    disp_vals = g_slist_append(disp_vals, g_variant_new_string("Nibble - BCD"));
    disp_vals = g_slist_append(disp_vals, g_variant_new_string("Nibble - BCD rev"));
    ookvis_options[0].values = disp_vals;

    ookvis_options[1].def = g_variant_new_string("4");
    GSList *sl_vals = NULL;
    for (int i = 0; i <= 10; i++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", i);
        sl_vals = g_slist_append(sl_vals, g_variant_new_string(buf));
    }
    ookvis_options[1].values = sl_vals;

    ookvis_options[2].def = g_variant_new_string("0");
    GSList *so_vals = NULL;
    for (int i = -4; i <= 4; i++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", i);
        so_vals = g_slist_append(so_vals, g_variant_new_string(buf));
    }
    ookvis_options[2].values = so_vals;

    ookvis_options[3].def = g_variant_new_string("off");
    GSList *ref_vals = NULL;
    ref_vals = g_slist_append(ref_vals, g_variant_new_string("off"));
    ref_vals = g_slist_append(ref_vals, g_variant_new_string("show numbers"));
    for (int i = 1; i <= 30; i++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", i);
        ref_vals = g_slist_append(ref_vals, g_variant_new_string(buf));
    }
    ookvis_options[3].values = ref_vals;

    return &ook_vis_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}