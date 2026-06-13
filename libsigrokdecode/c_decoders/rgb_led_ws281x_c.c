/*
 * rgb_led_ws281x_c.c — RGB LED WS281x color decoder (C implementation)
 *
 * Decodes colors from bus pulses for single wire RGB leds like
 * APA106, WS2811, WS2812, WS2813, SK6812, TM1829, TM1814, and TX1812.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum ws281x_ann {
    ANN_BIT = 0,
    ANN_RESET,
    ANN_RGB,
    NUM_ANN,
};

enum ws281x_state {
    STATE_FIND_RESET = 0,
    STATE_RESET,
    STATE_BIT_FALLING,
    STATE_BIT_RISING,
};

enum ws281x_color_mode {
    MODE_GRB = 0,
    MODE_RGB,
    MODE_BRG,
    MODE_RBG,
    MODE_BGR,
    MODE_GRBW,
    MODE_RGBW,
    MODE_WRGB,
    MODE_LBGR,
    MODE_LGRB,
    MODE_LRGB,
    MODE_LRBG,
    MODE_LGBR,
    MODE_LBRG,
};

typedef struct {
    uint64_t samplerate;
    int out_ann;

    int state;
    uint64_t ss_packet;
    uint64_t ss;
    uint64_t es;
    uint64_t last_samplenum;
    int bits[32];
    int bit_count;
    int colorsize;     /* 24 or 32 */
    int bit_val;       /* current bit value */

    /* Timing thresholds (in samples) */
    uint64_t tH_threshold;   /* 625ns in samples */
    uint64_t reset_threshold; /* 50us in samples */
    uint64_t bit_threshold;   /* 3us in samples */

    /* Options */
    int color_mode;
    int polarity;      /* 0=normal, 1=inverted */
} ws281x_state;

static struct srd_channel ws281x_channels[] = {
    { "din", "DIN", "DIN data line", 0, SRD_CHANNEL_SDATA, "dec_rgb_led_ws281x_chan_din" },
};

static struct srd_decoder_option ws281x_options[] = {
    { "colors", NULL, "Colors", NULL, NULL },
    { "polarity", NULL, "Polarity", NULL, NULL },
};

static const char *ws281x_ann_labels[][3] = {
    { "", "bit", "Bit" },
    { "", "reset", "RESET" },
    { "", "rgb", "RGB" },
};

static const int ws281x_row_bit_classes[] = { ANN_BIT, ANN_RESET, -1 };
static const int ws281x_row_rgb_classes[] = { ANN_RGB, -1 };

static const struct srd_c_ann_row ws281x_ann_rows[] = {
    { "bit", "Bits", ws281x_row_bit_classes, 2 },
    { "rgb", "RGB", ws281x_row_rgb_classes, 1 },
};

static const char *ws281x_inputs[] = { "logic" };
static const char *ws281x_tags[] = { "Display", "IC" };

static void ws281x_calc_thresholds(ws281x_state *s)
{
    if (s->samplerate > 0) {
        s->tH_threshold = (uint64_t)(s->samplerate * 625e-9);
        s->reset_threshold = (uint64_t)(s->samplerate * 50e-6);
        s->bit_threshold = (uint64_t)(s->samplerate * 3e-6);
    }
}

static void ws281x_output_color(struct srd_decoder_inst *di, ws281x_state *s, uint64_t es)
{
    if (s->bit_count != s->colorsize)
        return;

    uint32_t elems = 0;
    for (int i = 0; i < s->colorsize; i++)
        elems = (elems << 1) | s->bits[i];

    char color_str[64];

    if (s->colorsize == 24) {
        uint32_t rgb;
        switch (s->color_mode) {
        case MODE_GRB:
            rgb = ((elems & 0xff0000) >> 8) | ((elems & 0x00ff00) << 8) | (elems & 0x0000ff);
            snprintf(color_str, sizeof(color_str), "GRB#%06x", rgb);
            break;
        case MODE_RGB:
            rgb = elems;
            snprintf(color_str, sizeof(color_str), "RGB#%06x", rgb);
            break;
        case MODE_BRG:
            rgb = ((elems & 0xffff00) >> 8) | ((elems & 0x0000ff) << 16);
            snprintf(color_str, sizeof(color_str), "BRG#%06x", rgb);
            break;
        case MODE_RBG:
            rgb = (elems & 0xff0000) | ((elems & 0x00ff00) >> 8) | ((elems & 0x0000ff) << 8);
            snprintf(color_str, sizeof(color_str), "RBG#%06x", rgb);
            break;
        case MODE_BGR:
            rgb = ((elems & 0xff0000) >> 16) | (elems & 0x00ff00) | ((elems & 0x0000ff) << 16);
            snprintf(color_str, sizeof(color_str), "BGR#%06x", rgb);
            break;
        default:
            rgb = elems;
            snprintf(color_str, sizeof(color_str), "#%06x", rgb);
            break;
        }
    } else {
        /* 32-bit color modes */
        uint32_t rgb, w;
        switch (s->color_mode) {
        case MODE_GRBW:
            rgb = ((elems & 0xff000000) >> 16) | (elems & 0x00ff0000) | ((elems & 0x0000ff00) >> 8);
            w = elems & 0x000000ff;
            snprintf(color_str, sizeof(color_str), "GRB#%06x W#%02x", rgb, w);
            break;
        case MODE_RGBW:
            rgb = (elems & 0xffffff00) >> 8;
            w = elems & 0x000000ff;
            snprintf(color_str, sizeof(color_str), "RGB#%06x W#%02x", rgb, w);
            break;
        case MODE_WRGB:
            rgb = (elems & 0xffffff00) >> 8;
            w = elems & 0x000000ff;
            snprintf(color_str, sizeof(color_str), "W#%02x RGB#%06x", w, rgb);
            break;
        case MODE_LBGR:
            rgb = (elems & 0x0000ff00) | ((elems & 0x00ff0000) >> 16) | ((elems & 0x000000ff) << 16);
            w = (elems & 0xff000000) >> 24;
            snprintf(color_str, sizeof(color_str), "L#%02x BGR#%06x", w, rgb);
            break;
        case MODE_LGRB:
            rgb = (elems & 0x000000ff) | ((elems & 0x00ff0000) >> 8) | ((elems & 0x0000ff00) << 8);
            w = (elems & 0xff000000) >> 24;
            snprintf(color_str, sizeof(color_str), "L#%02x GRB#%06x", w, rgb);
            break;
        case MODE_LRGB:
            rgb = elems & 0x00ffffff;
            w = (elems & 0xff000000) >> 24;
            snprintf(color_str, sizeof(color_str), "L#%02x RGB#%06x", w, rgb);
            break;
        case MODE_LRBG:
            rgb = (elems & 0x00ff0000) | ((elems & 0x0000ff00) >> 8) | ((elems & 0x000000ff) << 8);
            w = (elems & 0xff000000) >> 24;
            snprintf(color_str, sizeof(color_str), "L#%02x RBG#%06x", w, rgb);
            break;
        case MODE_LGBR:
            rgb = ((elems & 0x00ff0000) >> 16) | ((elems & 0x0000ffff) << 8);
            w = (elems & 0xff000000) >> 24;
            snprintf(color_str, sizeof(color_str), "L#%02x GRB#%06x", w, rgb);
            break;
        case MODE_LBRG:
            rgb = ((elems & 0x00ffff00) >> 8) | ((elems & 0x000000ff) << 16);
            w = (elems & 0xff000000) >> 24;
            snprintf(color_str, sizeof(color_str), "L#%02x BRG#%06x", w, rgb);
            break;
        default:
            snprintf(color_str, sizeof(color_str), "#%08x", elems);
            break;
        }
    }

    c_put(di, s->ss_packet, es, s->out_ann, ANN_RGB, color_str);
}

static void ws281x_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(ws281x_state)));
    ws281x_state *s = (ws281x_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ws281x_state));
    s->out_ann = -1;
    s->state = STATE_FIND_RESET;
    s->color_mode = MODE_GRB;
    s->colorsize = 24;
}

static void ws281x_start(struct srd_decoder_inst *di)
{
    ws281x_state *s = (ws281x_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "rgb_led_ws281x");

    const char *colors_str = c_opt_str(di, "colors", "GRB");
    if (strcmp(colors_str, "GRB") == 0)       s->color_mode = MODE_GRB;
    else if (strcmp(colors_str, "RGB") == 0)   s->color_mode = MODE_RGB;
    else if (strcmp(colors_str, "BRG") == 0)   s->color_mode = MODE_BRG;
    else if (strcmp(colors_str, "RBG") == 0)   s->color_mode = MODE_RBG;
    else if (strcmp(colors_str, "BGR") == 0)   s->color_mode = MODE_BGR;
    else if (strcmp(colors_str, "GRBW") == 0)  s->color_mode = MODE_GRBW;
    else if (strcmp(colors_str, "RGBW") == 0)  s->color_mode = MODE_RGBW;
    else if (strcmp(colors_str, "WRGB") == 0)  s->color_mode = MODE_WRGB;
    else if (strcmp(colors_str, "LBGR") == 0)  s->color_mode = MODE_LBGR;
    else if (strcmp(colors_str, "LGRB") == 0)  s->color_mode = MODE_LGRB;
    else if (strcmp(colors_str, "LRGB") == 0)  s->color_mode = MODE_LRGB;
    else if (strcmp(colors_str, "LRBG") == 0)  s->color_mode = MODE_LRBG;
    else if (strcmp(colors_str, "LGBR") == 0)  s->color_mode = MODE_LGBR;
    else if (strcmp(colors_str, "LBRG") == 0)  s->color_mode = MODE_LBRG;
    else s->color_mode = MODE_GRB;

    /* 3-char mode names -> 24-bit, 4-char -> 32-bit */
    s->colorsize = (strlen(colors_str) == 4) ? 32 : 24;

    const char *pol_str = c_opt_str(di, "polarity", "normal");
    s->polarity = (strcmp(pol_str, "inverted") == 0) ? 1 : 0;
}

static void ws281x_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    ws281x_state *s = (ws281x_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        ws281x_calc_thresholds(s);
    }
}

static void ws281x_check_bit(ws281x_state *s, uint64_t samplenum)
{
    (void)samplenum;
    uint64_t period = samplenum - s->ss;
    uint64_t tH_samples = s->es - s->ss;

    if (tH_samples >= s->tH_threshold) {
        s->bit_val = 1;
    } else {
        /* Duty cycle check */
        s->bit_val = (period > 0 && (tH_samples * 10 / period) > 5) ? 1 : 0;
    }
}

static void ws281x_decode(struct srd_decoder_inst *di)
{
    ws281x_state *s = (ws281x_state *)c_decoder_get_private(di);
    if (!s->samplerate) {
        s->samplerate = c_samplerate(di);
        ws281x_calc_thresholds(s);
    }
    if (s->samplerate == 0)
        return;

    while (1) {
        int ret;

        switch (s->state) {
        case STATE_FIND_RESET:
            /* Wait for low (normal) or high (inverted) */
            if (s->polarity == 0)
                ret = c_wait(di, CW_L(0), CW_END);
            else
                ret = c_wait(di, CW_H(0), CW_END);
            if (ret != SRD_OK)
                return;
            s->last_samplenum = di_samplenum(di);

            s->ss = di_samplenum(di);

            /* Wait for next edge */
            ret = c_wait(di, CW_E(0), CW_END);
            if (ret != SRD_OK)
                return;
            s->last_samplenum = di_samplenum(di);

            s->es = di_samplenum(di);

            if ((s->es - s->ss) > s->reset_threshold) {
                s->state = STATE_RESET;
            } else if ((s->es - s->ss) > s->bit_threshold) {
                /* Not a RESET, might be a bit */
                s->bit_count = 0;
                s->ss = di_samplenum(di);
                s->ss_packet = di_samplenum(di);
                ret = c_wait(di, CW_E(0), CW_END);
                if (ret != SRD_OK)
                    return;
                s->last_samplenum = di_samplenum(di);
                s->state = STATE_BIT_FALLING;
            }
            break;

        case STATE_RESET:
            c_put(di, s->ss, s->es, s->out_ann, ANN_RESET, "RESET", "RST", "R");
            s->bit_count = 0;
            s->ss = di_samplenum(di);
            s->ss_packet = di_samplenum(di);
            ret = c_wait(di, CW_E(0), CW_END);
            if (ret != SRD_OK)
                return;
            s->last_samplenum = di_samplenum(di);
            s->state = STATE_BIT_FALLING;
            break;

        case STATE_BIT_FALLING:
            s->es = di_samplenum(di);
            ret = c_wait(di, CW_E(0), CW_END);
            if (ret != SRD_OK)
                return;
            s->last_samplenum = di_samplenum(di);

            if ((di_samplenum(di) - s->es) > s->reset_threshold) {
                /* Check bit value before RESET */
                ws281x_check_bit(s, di_samplenum(di));
                char bit_str[16];
                snprintf(bit_str, sizeof(bit_str), "%d", s->bit_val);
                c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, bit_str);

                if (s->bit_count < 32) {
                    s->bits[s->bit_count] = s->bit_val;
                    s->bit_count++;
                }
                ws281x_output_color(di, s, s->es);

                s->ss = s->es;
                s->es = di_samplenum(di);
                s->state = STATE_RESET;
            } else {
                s->state = STATE_BIT_RISING;
            }
            break;

        case STATE_BIT_RISING:
            ws281x_check_bit(s, di_samplenum(di));
            {
                char bit_str[16];
                snprintf(bit_str, sizeof(bit_str), "%d", s->bit_val);
                c_put(di, s->ss, di_samplenum(di), s->out_ann, ANN_BIT, bit_str);
            }
            if (s->bit_count < 32) {
                s->bits[s->bit_count] = s->bit_val;
                s->bit_count++;
            }
            ws281x_output_color(di, s, di_samplenum(di));

            s->ss = di_samplenum(di);
            ret = c_wait(di, CW_E(0), CW_END);
            if (ret != SRD_OK)
                return;
            s->last_samplenum = di_samplenum(di);
            s->state = STATE_BIT_FALLING;
            break;
        }
    }
}

static void ws281x_end(struct srd_decoder_inst *di)
{
    ws281x_state *s = (ws281x_state *)c_decoder_get_private(di);
    if (!s || s->state != STATE_BIT_FALLING)
        return;

    /* Flush the last bit when data ends in BIT_FALLING state,
     * matching Python's end() method behavior.
     * Use the actual last sample of the data stream (like Python's
     * self.last_samplenum) so the duty cycle check works correctly. */
    uint64_t last_sample = c_last_samplenum(di);
    if (last_sample == 0)
        last_sample = s->last_samplenum;
    ws281x_check_bit(s, last_sample);
    char bit_str[16];
    snprintf(bit_str, sizeof(bit_str), "%d", s->bit_val);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, bit_str);

    if (s->bit_count < 32) {
        s->bits[s->bit_count] = s->bit_val;
        s->bit_count++;
    }
    ws281x_output_color(di, s, s->es);
}

static void ws281x_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder rgb_led_ws281x_c_decoder = {
    .id = "rgb_led_ws281x_c",
    .name = "RGB LED WS2812+(C)",
    .longname = "RGB LED color decoder (C)",
    .desc = "Decodes colors from bus pulses for single wire RGB leds like APA106, WS2811, WS2812, WS2813, SK6812, TM1829, TM1814, and TX1812 (C implementation)",
    .license = "gplv3+",
    .channels = ws281x_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = ws281x_options,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = ws281x_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = ws281x_ann_rows,
    .inputs = ws281x_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = ws281x_tags,
    .num_tags = 2,
    .reset = ws281x_reset,
    .start = ws281x_start,
    .decode = ws281x_decode,
    .end = ws281x_end,
    .destroy = ws281x_destroy,
    .state_size = 0,
    .metadata = ws281x_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    GVariant *vals_colors[] = {
        g_variant_new_string("GRB"),
        g_variant_new_string("RGB"),
        g_variant_new_string("BRG"),
        g_variant_new_string("RBG"),
        g_variant_new_string("BGR"),
        g_variant_new_string("GRBW"),
        g_variant_new_string("RGBW"),
        g_variant_new_string("WRGB"),
        g_variant_new_string("LBGR"),
        g_variant_new_string("LGRB"),
        g_variant_new_string("LRGB"),
        g_variant_new_string("LRBG"),
        g_variant_new_string("LGBR"),
        g_variant_new_string("LBRG"),
    };
    GSList *list_colors = NULL;
    for (int i = 0; i < 14; i++)
        list_colors = g_slist_append(list_colors, vals_colors[i]);

    ws281x_options[0].id = "colors";
    ws281x_options[0].idn = "dec_rgb_led_ws281x_opt_colors";
    ws281x_options[0].desc = "Colors";
    ws281x_options[0].def = g_variant_new_string("GRB");
    ws281x_options[0].values = list_colors;

    GVariant *vals_pol[] = {
        g_variant_new_string("normal"),
        g_variant_new_string("inverted"),
    };
    GSList *list_pol = NULL;
    list_pol = g_slist_append(list_pol, vals_pol[0]);
    list_pol = g_slist_append(list_pol, vals_pol[1]);

    ws281x_options[1].id = "polarity";
    ws281x_options[1].idn = "dec_rgb_led_ws281x_opt_polarity";
    ws281x_options[1].desc = "Polarity";
    ws281x_options[1].def = g_variant_new_string("normal");
    ws281x_options[1].values = list_pol;

    return &rgb_led_ws281x_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}