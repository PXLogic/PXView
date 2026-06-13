/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2017 Kevin Redon <kingkevin@cuvoodoo.info>
 * Copyright (C) 2025 C port (v4 API)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Channel index */
#define CH_OWR 0

/* Annotation class indices — match Python annotations tuple */
enum owlink_ann {
    ANN_BIT       = 0,
    ANN_WARN      = 1,
    ANN_RESET     = 2,
    ANN_PRESENCE  = 3,
    ANN_OVERDRIVE = 4,
    NUM_ANN,
};

/* State machine — match Python self.state */
enum owlink_state {
    STATE_INITIAL,
    STATE_IDLE,
    STATE_LOW,
    STATE_PRESENCE_DETECT_HIGH,
    STATE_PRESENCE_DETECT_LOW,
    STATE_SLOT,
    STATE_PRESENCE_DETECT,
};

/* Timing values in us — match Python timing dict.
 * [0]=normal, [1]=overdrive */
struct ow_timing {
    double RSTL_min[2];
    double RSTL_max[2];
    double RSTH_min[2];
    double PDH_min[2];
    double PDH_max[2];
    double PDL_min[2];
    double PDL_max[2];
    double SLOT_min[2];
    double SLOT_max[2];
    double REC_min[2];
    double LOWR_min[2];
    double LOWR_max[2];
};

static const struct ow_timing ow_timing = {
    .RSTL_min = {480.0, 48.0},
    .RSTL_max = {960.0, 80.0},
    .RSTH_min = {480.0, 48.0},
    .PDH_min  = {15.0,  2.0},
    .PDH_max  = {60.0,  6.0},
    .PDL_min  = {60.0,  8.0},
    .PDL_max  = {240.0, 24.0},
    .SLOT_min = {60.0,  6.0},
    .SLOT_max = {120.0, 16.0},
    .REC_min  = {1.0,   1.0},
    .LOWR_min = {1.0,   1.0},
    .LOWR_max = {15.0,  2.0},
};

/* Decoder state struct — C_DECODER_STATE auto-generates owlink_s typedef,
 * owlink_reset (calloc), and owlink_destroy (free). */
C_DECODER_STATE(owlink, {
    enum owlink_state state;
    uint8_t byte_val;
    int bit_cnt;
    uint64_t ss_rise;
    uint64_t ss_fall;
    int overdrive;
    int present;
    int bit_val;
    uint64_t samplerate;
    int out_ann;
    int out_python;
});

/* Channel definitions — match Python channels tuple */
static struct srd_channel owlink_channels[] = {
    {"owr", "OWR", "1-Wire signal line", 0, SRD_CHANNEL_SDATA, "dec_onewire_link_chan_owr"},
};

/* Options — match Python options tuple */
static struct srd_decoder_option owlink_options[] = {
    {"overdrive", "dec_onewire_link_opt_overdrive", "Start in overdrive speed", NULL, NULL},
};

/* Annotation labels — match Python annotations tuple */
static const char *owlink_ann_labels[][3] = {
    {"", "Bit", "B"},
    {"", "Warnings", "Warn"},
    {"", "Reset", "Rst"},
    {"", "Presence", "Pres"},
    {"", "Overdrive speed notifications", "Overdrive"},
};

/* Annotation row class lists */
static const int owlink_row_bits_classes[] = {ANN_BIT, ANN_RESET, ANN_PRESENCE, -1};
static const int owlink_row_info_classes[] = {ANN_OVERDRIVE, -1};
static const int owlink_row_warnings_classes[] = {ANN_WARN, -1};

static const struct srd_c_ann_row owlink_ann_rows[] = {
    {"bits",     "Bits",     owlink_row_bits_classes,     3},
    {"info",     "Info",     owlink_row_info_classes,     1},
    {"warnings", "Warnings", owlink_row_warnings_classes, 1},
};

static const char *owlink_inputs[] = {"logic", NULL};
static const char *owlink_outputs[] = {"onewire_link", NULL};
static const char *owlink_tags[] = {"Embedded/industrial", NULL};

/* Helper: convert microseconds to sample count */
static uint64_t us_to_samples(uint64_t samplerate, double us)
{
    return (uint64_t)(us * (double)samplerate / 1000000.0);
}

/* Helper: emit samplerate warnings — match Python checks() */
static void owlink_checks(struct srd_decoder_inst *di, owlink_s *s)
{
    if (s->overdrive) {
        if (s->samplerate < 2000000) {
            c_put(di, 0, 0, s->out_ann, ANN_WARN,
                  "Sampling rate is too low. Must be above 2MHz for proper overdrive mode decoding.");
        } else if (s->samplerate < 5000000) {
            c_put(di, 0, 0, s->out_ann, ANN_WARN,
                  "Sampling rate is suggested to be above 5MHz for proper overdrive mode decoding.");
        }
    } else {
        if (s->samplerate < 400000) {
            c_put(di, 0, 0, s->out_ann, ANN_WARN,
                  "Sampling rate is too low. Must be above 400kHz for proper normal mode decoding.");
        } else if (s->samplerate < 1000000) {
            c_put(di, 0, 0, s->out_ann, ANN_WARN,
                  "Sampling rate is suggested to be above 1MHz for proper normal mode decoding.");
        }
    }
}

/* start callback — match Python start() */
static void owlink_start(struct srd_decoder_inst *di)
{
    owlink_s *s = (owlink_s *)c_decoder_get_private(di);

    s->out_ann    = c_reg_out(di, SRD_OUTPUT_ANN, "onewire_link");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "onewire_link");

    const char *od = c_opt_str(di, "overdrive", "no");
    s->overdrive = (strcmp(od, "yes") == 0) ? 1 : 0;
    s->bit_cnt = -1;
    s->samplerate = c_samplerate(di);
}

/* metadata callback — match Python metadata() */
static void owlink_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    owlink_s *s = (owlink_s *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

/* decode callback — main state machine, match Python decode() */
static void owlink_decode(struct srd_decoder_inst *di)
{
    owlink_s *s = (owlink_s *)c_decoder_get_private(di);
    int ret;

    if (!s->samplerate)
        s->samplerate = c_samplerate(di);
    if (!s->samplerate)
        return;

    owlink_checks(di, s);

    while (1) {
        int od = s->overdrive ? 1 : 0;

        switch (s->state) {

        case STATE_INITIAL: {
            /* Python: self.wait({0: 'h'}) */
            ret = c_wait(di, CW_H(CH_OWR), CW_END);
            if (ret != SRD_OK)
                return;
            s->ss_rise = di_samplenum(di);
            s->state = STATE_IDLE;
            break;
        }

        case STATE_IDLE: {
            /* Python: self.wait({0: 'f'}) */
            ret = c_wait(di, CW_F(CH_OWR), CW_END);
            if (ret != SRD_OK)
                return;
            s->ss_fall = di_samplenum(di);

            /* Check recovery time */
            if (s->ss_rise > 0) {
                double time_us = (double)(s->ss_fall - s->ss_rise) / (double)s->samplerate * 1000000.0;
                if (time_us < ow_timing.REC_min[od]) {
                    char txt3[64];
                    snprintf(txt3, sizeof(txt3), "REC < %.0f", ow_timing.REC_min[od]);
                    c_put(di, s->ss_rise, s->ss_fall, s->out_ann, ANN_WARN,
                          "Recovery time not long enough", "Recovery too short", txt3);
                }
            }
            s->state = STATE_LOW;
            break;
        }

        case STATE_LOW: {
            /* Python: self.wait({0: 'r'}) */
            ret = c_wait(di, CW_R(CH_OWR), CW_END);
            if (ret != SRD_OK)
                return;
            s->ss_rise = di_samplenum(di);

            double time_us = (double)(s->ss_rise - s->ss_fall) / (double)s->samplerate * 1000000.0;

            if (time_us >= ow_timing.RSTL_min[0]) {
                /* Normal reset pulse */
                if (time_us > ow_timing.RSTL_max[0]) {
                    char txt3[64];
                    snprintf(txt3, sizeof(txt3), "RST > %.0f", ow_timing.RSTL_max[0]);
                    c_put(di, s->ss_fall, s->ss_rise, s->out_ann, ANN_WARN,
                          "Too long reset pulse might mask interrupt signalling by other devices",
                          "Reset pulse too long", txt3);
                }
                if (s->overdrive) {
                    c_put(di, s->ss_fall, s->ss_rise, s->out_ann, ANN_OVERDRIVE,
                          "Exiting overdrive mode", "Overdrive off");
                    s->overdrive = 0;
                }
                c_put(di, s->ss_fall, s->ss_rise, s->out_ann, ANN_RESET, "Reset", "Rst", "R");
                s->state = STATE_PRESENCE_DETECT_HIGH;
            } else if (s->overdrive && time_us >= ow_timing.RSTL_min[1] && time_us < ow_timing.RSTL_max[1]) {
                /* Overdrive reset pulse */
                c_put(di, s->ss_fall, s->ss_rise, s->out_ann, ANN_RESET, "Reset", "Rst", "R");
                s->state = STATE_PRESENCE_DETECT_HIGH;
            } else if (time_us < ow_timing.SLOT_max[od]) {
                /* Read/write time slot */
                if (time_us < ow_timing.LOWR_min[od]) {
                    char txt3[64];
                    snprintf(txt3, sizeof(txt3), "LOW < %.0f", ow_timing.LOWR_min[od]);
                    c_put(di, s->ss_fall, s->ss_rise, s->out_ann, ANN_WARN,
                          "Low signal not long enough", "Low too short", txt3);
                }
                if (time_us < ow_timing.LOWR_max[od])
                    s->bit_val = 1; /* Short pulse = 1 bit */
                else
                    s->bit_val = 0; /* Long pulse = 0 bit */
                s->state = STATE_SLOT;
            } else {
                /* Timing outside known states */
                c_put(di, s->ss_fall, s->ss_rise, s->out_ann, ANN_WARN,
                      "Erroneous signal", "Error", "Err", "E");
                s->state = STATE_IDLE;
            }
            break;
        }

        case STATE_PRESENCE_DETECT_HIGH: {
            /* Python: wait_falling_timeout(self.rise, timing['PDH']['max'])
             * which is: self.wait([{0: 'f'}, {'skip': samples_to_skip}]) */
            uint64_t pdh_max_samples = us_to_samples(s->samplerate, ow_timing.PDH_max[od]);
            uint64_t cur = di_samplenum(di);
            uint64_t skip_count = 0;
            if (s->ss_rise + pdh_max_samples > cur)
                skip_count = s->ss_rise + pdh_max_samples - cur;

            ret = c_wait(di, CW_F(CH_OWR), CW_OR, CW_SKIP(skip_count), CW_END);
            if (ret != SRD_OK)
                return;

            uint64_t samplenum = di_samplenum(di);
            uint64_t matched = di_matched(di);
            double time_us = (double)(samplenum - s->ss_rise) / (double)s->samplerate * 1000000.0;

            if ((matched & (1ULL << 0)) && !(matched & (1ULL << 1))) {
                /* Presence detected (falling edge, not timeout) */
                if (time_us < ow_timing.PDH_min[od]) {
                    char txt3[64];
                    snprintf(txt3, sizeof(txt3), "PDH < %.0f", ow_timing.PDH_min[od]);
                    c_put(di, s->ss_rise, samplenum, s->out_ann, ANN_WARN,
                          "Presence detect signal is too early", "Presence detect too early", txt3);
                }
                s->ss_fall = samplenum;
                s->state = STATE_PRESENCE_DETECT_LOW;
            } else {
                /* No presence detected */
                c_put(di, s->ss_rise, samplenum, s->out_ann, ANN_PRESENCE,
                      "Presence: false", "Presence", "Pres", "P");
                c_proto(di, s->ss_rise, samplenum, s->out_python, "RESET/PRESENCE", C_U8(0), C_END);
                s->state = STATE_IDLE;
            }
            break;
        }

        case STATE_PRESENCE_DETECT_LOW: {
            /* Python: self.wait({0: 'r'}) */
            ret = c_wait(di, CW_R(CH_OWR), CW_END);
            if (ret != SRD_OK)
                return;

            uint64_t samplenum = di_samplenum(di);
            double time_us = (double)(samplenum - s->ss_fall) / (double)s->samplerate * 1000000.0;
            od = s->overdrive ? 1 : 0;

            if (time_us < ow_timing.PDL_min[od]) {
                char txt3[64];
                snprintf(txt3, sizeof(txt3), "PDL < %.0f", ow_timing.PDL_min[od]);
                c_put(di, s->ss_fall, samplenum, s->out_ann, ANN_WARN,
                      "Presence detect signal is too short", "Presence detect too short", txt3);
            } else if (time_us > ow_timing.PDL_max[od]) {
                char txt3[64];
                snprintf(txt3, sizeof(txt3), "PDL > %.0f", ow_timing.PDL_max[od]);
                c_put(di, s->ss_fall, samplenum, s->out_ann, ANN_WARN,
                      "Presence detect signal is too long", "Presence detect too long", txt3);
            }

            if (time_us > ow_timing.RSTH_min[od])
                s->ss_rise = samplenum;

            s->state = STATE_PRESENCE_DETECT;
            break;
        }

        case STATE_SLOT: {
            /* Python: wait_falling_timeout(self.fall, timing['SLOT']['min'])
             * which is: self.wait([{0: 'f'}, {'skip': samples_to_skip}]) */
            uint64_t slot_min_samples = us_to_samples(s->samplerate, ow_timing.SLOT_min[od]);
            uint64_t cur = di_samplenum(di);
            uint64_t skip_count = 0;
            if (s->ss_fall + slot_min_samples > cur)
                skip_count = s->ss_fall + slot_min_samples - cur;

            ret = c_wait(di, CW_F(CH_OWR), CW_OR, CW_SKIP(skip_count), CW_END);
            if (ret != SRD_OK)
                return;

            uint64_t samplenum = di_samplenum(di);
            uint64_t matched = di_matched(di);

            if ((matched & (1ULL << 0)) && !(matched & (1ULL << 1))) {
                /* Low detected before end of slot */
                char txt3[64];
                snprintf(txt3, sizeof(txt3), "SLOT < %.1f", ow_timing.SLOT_min[od]);
                c_put(di, s->ss_fall, samplenum, s->out_ann, ANN_WARN,
                      "Time slot not long enough", "Slot too short", txt3);
                s->ss_fall = samplenum;
                s->state = STATE_LOW;
            } else {
                /* End of time slot — output bit */
                char bit_long[16], bit_short[4];
                snprintf(bit_long, sizeof(bit_long), "Bit: %d", s->bit_val);
                snprintf(bit_short, sizeof(bit_short), "%d", s->bit_val);
                c_put(di, s->ss_fall, samplenum, s->out_ann, ANN_BIT, bit_long, bit_short);

                c_proto(di, s->ss_fall, samplenum, s->out_python, "BIT", C_U8(s->bit_val), C_END);

                /* Save command bits */
                if (s->bit_cnt >= 0) {
                    s->byte_val |= (s->bit_val << s->bit_cnt);
                    s->bit_cnt++;
                }

                /* Check for overdrive ROM command */
                if (s->bit_cnt >= 8) {
                    if ((s->byte_val == 0x3C || s->byte_val == 0x69) && !s->overdrive) {
                        s->overdrive = 1;
                        c_put(di, samplenum, samplenum, s->out_ann, ANN_OVERDRIVE,
                              "Entering overdrive mode", "Overdrive on");
                    }
                    s->bit_cnt = -1;
                    s->byte_val = 0;
                }

                s->state = STATE_IDLE;
            }
            break;
        }

        case STATE_PRESENCE_DETECT: {
            /* Python: wait_falling_timeout(self.rise, timing['RSTH']['min'])
             * which is: self.wait([{0: 'f'}, {'skip': samples_to_skip}]) */
            uint64_t rsth_min_samples = us_to_samples(s->samplerate, ow_timing.RSTH_min[od]);
            uint64_t cur = di_samplenum(di);
            uint64_t skip_count = 0;
            if (s->ss_rise + rsth_min_samples > cur)
                skip_count = s->ss_rise + rsth_min_samples - cur;

            ret = c_wait(di, CW_F(CH_OWR), CW_OR, CW_SKIP(skip_count), CW_END);
            if (ret != SRD_OK)
                return;

            uint64_t samplenum = di_samplenum(di);
            uint64_t matched = di_matched(di);

            if ((matched & (1ULL << 0)) && !(matched & (1ULL << 1))) {
                /* Low detected before end of presence detect */
                char txt3[64];
                snprintf(txt3, sizeof(txt3), "RTSH < %.0f", ow_timing.RSTH_min[od]);
                c_put(di, s->ss_rise, samplenum, s->out_ann, ANN_WARN,
                      "Presence detect not long enough", "Presence detect too short", txt3);

                c_put(di, s->ss_rise, samplenum, s->out_ann, ANN_PRESENCE,
                      "Slave presence detected", "Slave present", "Present", "P");
                c_proto(di, s->ss_rise, samplenum, s->out_python, "RESET/PRESENCE", C_U8(1), C_END);
                s->ss_fall = samplenum;
                s->state = STATE_LOW;
            } else {
                /* End of presence detect */
                c_put(di, s->ss_rise, samplenum, s->out_ann, ANN_PRESENCE,
                      "Presence: true", "Presence", "Pres", "P");
                c_proto(di, s->ss_rise, samplenum, s->out_python, "RESET/PRESENCE", C_U8(1), C_END);
                s->ss_rise = samplenum;
                /* Start counting the first 8 bits to get the ROM command */
                s->bit_cnt = 0;
                s->byte_val = 0;
                s->state = STATE_IDLE;
            }
            break;
        }
        }
    }
}

/* ---- Decoder definition (v4 API) ---- */
static struct srd_c_decoder onewire_link_c_def = {
    .id = "onewire_link_c",
    .name = "OneWire link layer(C)",
    .longname = "1-Wire serial communication bus (link layer)(C)",
    .desc = "Bidirectional, half-duplex, asynchronous serial bus.(C implementation)",
    .license = "gplv2+",
    .channels = owlink_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = owlink_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = owlink_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = owlink_ann_rows,
    .inputs = owlink_inputs,
    .num_inputs = 1,
    .outputs = owlink_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = owlink_tags,
    .num_tags = 1,
    .state_size = sizeof(owlink_s),
    .reset = owlink_reset,
    .start = owlink_start,
    .decode = owlink_decode,
    .end = NULL,
    .metadata = owlink_metadata,
    .destroy = owlink_destroy,
    .decode_upper = NULL,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    owlink_options[0].def = g_variant_new_string("no");
    GSList *od_vals = NULL;
    od_vals = g_slist_append(od_vals, g_variant_new_string("yes"));
    od_vals = g_slist_append(od_vals, g_variant_new_string("no"));
    owlink_options[0].values = od_vals;
    return &onewire_link_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}