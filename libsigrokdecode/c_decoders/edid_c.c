/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2012 Bert Vermeulen <bert@biot.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_FIELDS = 0,
    ANN_SECTIONS = 1,
    NUM_ANN,
};

enum {
    EDID_STATE_IDLE,
    EDID_STATE_OFFSET,
    EDID_STATE_HEADER,
    EDID_STATE_EDID,
    EDID_STATE_EXTENSIONS,
};

#define OFF_VENDOR    8
#define OFF_VERSION  18
#define OFF_BASIC    20
#define OFF_CHROM    25
#define OFF_EST_TIMING 35
#define OFF_STD_TIMING 38
#define OFF_DET_TIMING 54
#define OFF_NUM_EXT  126
#define OFF_CHECKSUM 127

static const uint8_t EDID_HEADER[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};

static const char *est_modes[] = {
    "720x400@70Hz",
    "720x400@88Hz",
    "640x480@60Hz",
    "640x480@67Hz",
    "640x480@72Hz",
    "640x480@75Hz",
    "800x600@56Hz",
    "800x600@60Hz",
    "800x600@72Hz",
    "800x600@75Hz",
    "832x624@75Hz",
    "1024x768@87Hz(i)",
    "1024x768@60Hz",
    "1024x768@70Hz",
    "1024x768@75Hz",
    "1280x1024@75Hz",
    "1152x870@75Hz",
};

static const int xy_ratio[][2] = {
    {16, 10},
    {4, 3},
    {5, 4},
    {16, 9},
};

#define MAX_EXTENSIONS 4

typedef struct {
    int out_ann;
    int state;
    int cnt;
    uint8_t cache[128];
    uint64_t sn[128][2];
    int offset;
    int extension;
    uint8_t ext_cache[MAX_EXTENSIONS][128];
    uint64_t ext_sn[MAX_EXTENSIONS][128][2];
    int have_preferred_timing;
    uint64_t ss;
    uint64_t es;
} edid_state;

static const char *edid_inputs[] = {"i2c", NULL};
static const char *edid_tags[] = {"Display", "Memory", "PC", NULL};

static const char *edid_ann_labels[][3] = {
    {"", "fields", "EDID structure fields"},
    {"", "sections", "EDID structure sections"},
};

static const int edid_row_sections_classes[] = {ANN_SECTIONS, -1};
static const int edid_row_fields_classes[] = {ANN_FIELDS, -1};

static const struct srd_c_ann_row edid_ann_rows[] = {
    {"sections", "Sections", edid_row_sections_classes, 1},
    {"fields", "Fields", edid_row_fields_classes, 1},
};

/* Helper: get sn array for current context */
static uint64_t (*edid_get_sn(edid_state *s))[2]
{
    if (s->extension > 0 && s->extension <= MAX_EXTENSIONS)
        return s->ext_sn[s->extension - 1];
    return s->sn;
}

/* Helper: get cache for current context */
static uint8_t *edid_get_cache(edid_state *s)
{
    if (s->extension > 0 && s->extension <= MAX_EXTENSIONS)
        return s->ext_cache[s->extension - 1];
    return s->cache;
}

/* ann_field: output a field annotation */
static void edid_ann_field(struct srd_decoder_inst *di, edid_state *s,
                           int start, int end, const char *text)
{
    uint64_t (*sn_ptr)[2] = edid_get_sn(s);
    c_put(di, sn_ptr[start][0], sn_ptr[end][1], s->out_ann, ANN_FIELDS, text);
}

static double edid_convert_color(int value)
{
    double outval = 0.0;
    for (int i = 0; i < 10; i++) {
        if (value & 0x01)
            outval += 1.0 / (1 << (10 - i));
        value >>= 1;
    }
    return outval;
}

static void edid_decode_vid(struct srd_decoder_inst *di, edid_state *s, int offset)
{
    uint8_t *c = edid_get_cache(s);
    char pnpid[4];
    pnpid[0] = (char)(64 + ((c[offset] & 0x7c) >> 2));
    pnpid[1] = (char)(64 + (((c[offset] & 0x03) << 3) | ((c[offset + 1] & 0xe0) >> 5)));
    pnpid[2] = (char)(64 + (c[offset + 1] & 0x1f));
    pnpid[3] = '\0';
    /* Simplified: just output PNPID code without vendor name lookup */
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", pnpid);
    edid_ann_field(di, s, offset, offset + 1, buf);
}

static void edid_decode_pid(struct srd_decoder_inst *di, edid_state *s, int offset)
{
    uint8_t *c = edid_get_cache(s);
    char buf[64];
    snprintf(buf, sizeof(buf), "Product 0x%.2X%.2X", c[offset + 1], c[offset]);
    edid_ann_field(di, s, offset, offset + 1, buf);
}

static void edid_decode_serial(struct srd_decoder_inst *di, edid_state *s, int offset)
{
    uint8_t *c = edid_get_cache(s);
    uint32_t serialnum = ((uint32_t)c[offset + 3] << 24)
                       | ((uint32_t)c[offset + 2] << 16)
                       | ((uint32_t)c[offset + 1] << 8)
                       | c[offset];
    /* Check if it's alphanumeric */
    int is_alnum = 1;
    char serialstr[8] = {0};
    for (int i = 0; i < 4; i++) {
        if (c[offset + 3 - i] < 32 || c[offset + 3 - i] > 126) {
            is_alnum = 0;
            break;
        }
        serialstr[i] = (char)c[offset + 3 - i];
    }
    char buf[64];
    if (is_alnum)
        snprintf(buf, sizeof(buf), "Serial %s", serialstr);
    else
        snprintf(buf, sizeof(buf), "Serial %u", serialnum);
    edid_ann_field(di, s, offset, offset + 3, buf);
}

static void edid_decode_mfrdate(struct srd_decoder_inst *di, edid_state *s, int offset)
{
    uint8_t *c = edid_get_cache(s);
    char buf[256];
    int pos = 0;
    if (c[offset])
        pos += snprintf(buf + pos, sizeof(buf) - pos, "week %d, ", c[offset]);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%d", 1990 + c[offset + 1]);
    if (pos > 0) {
        char buf2[512];
        snprintf(buf2, sizeof(buf2), "Manufactured %s", buf);
        edid_ann_field(di, s, offset, offset + 1, buf2);
    }
}

static void edid_decode_basicdisplay(struct srd_decoder_inst *di, edid_state *s, int offset)
{
    uint8_t *c = edid_get_cache(s);
    /* Video input definition */
    uint8_t vid = c[offset];
    if (vid & 0x80) {
        edid_ann_field(di, s, offset, offset, "Video input: VESA DFP 1.");
    } else {
        int sls = (vid & 60) >> 5;
        char buf[64];
        snprintf(buf, sizeof(buf), "Signal level standard: %.2x", sls);
        edid_ann_field(di, s, offset, offset, buf);
        if (vid & 0x10)
            edid_ann_field(di, s, offset, offset, "Blank-to-black setup expected");
        char syncs[128] = "";
        if (vid & 0x08)
            strcat(syncs, "separate syncs, ");
        if (vid & 0x04)
            strcat(syncs, "composite syncs, ");
        if (vid & 0x02)
            strcat(syncs, "sync on green, ");
        if (vid & 0x01)
            strcat(syncs, "Vsync serration required, ");
        if (syncs[0]) {
            syncs[strlen(syncs) - 2] = '\0';
            char buf2[256];
            snprintf(buf2, sizeof(buf2), "Supported syncs: %s", syncs);
            edid_ann_field(di, s, offset, offset, buf2);
        }
    }
    /* Max horizontal/vertical image size */
    if (c[offset + 1] != 0 && c[offset + 2] != 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Physical size: %dx%dcm", c[offset + 1], c[offset + 2]);
        edid_ann_field(di, s, offset + 1, offset + 2, buf);
    }
    /* Gamma */
    if (c[offset + 3] != 0xff) {
        double gamma = (c[offset + 3] + 100) / 100.0;
        char buf[64];
        snprintf(buf, sizeof(buf), "Gamma: %1.2f", gamma);
        edid_ann_field(di, s, offset + 3, offset + 3, buf);
    }
    /* Feature support */
    uint8_t fs = c[offset + 4];
    char dpms[128] = "";
    if (fs & 0x80)
        strcat(dpms, "standby, ");
    if (fs & 0x40)
        strcat(dpms, "suspend, ");
    if (fs & 0x20)
        strcat(dpms, "active off, ");
    if (dpms[0]) {
        dpms[strlen(dpms) - 2] = '\0';
        char buf[512];
        snprintf(buf, sizeof(buf), "DPMS support: %s", dpms);
        edid_ann_field(di, s, offset + 4, offset + 4, buf);
    }
    int dt = (fs & 0x18) >> 3;
    const char *dtstr = NULL;
    if (dt == 0)
        dtstr = "Monochrome";
    else if (dt == 1)
        dtstr = "RGB color";
    else if (dt == 2)
        dtstr = "non-RGB multicolor";
    if (dtstr) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Display type: %s", dtstr);
        edid_ann_field(di, s, offset + 4, offset + 4, buf);
    }
    if (fs & 0x04)
        edid_ann_field(di, s, offset + 4, offset + 4, "Color space: standard sRGB");
    s->have_preferred_timing = (fs & 0x02) == 0x02;
    const char *gft = (fs & 0x01) ? "" : "not ";
    char buf[256];
    snprintf(buf, sizeof(buf), "Generalized timing formula: %ssupported", gft);
    edid_ann_field(di, s, offset + 4, offset + 4, buf);
}

static void edid_decode_chromaticity(struct srd_decoder_inst *di, edid_state *s, int offset)
{
    uint8_t *c = edid_get_cache(s);
    char buf[256];

    int redx = (c[offset + 2] << 2) + ((c[offset] & 0xc0) >> 6);
    int redy = (c[offset + 3] << 2) + ((c[offset] & 0x30) >> 4);
    snprintf(buf, sizeof(buf), "Chromacity red: X %1.3f, Y %1.3f",
             edid_convert_color(redx), edid_convert_color(redy));
    edid_ann_field(di, s, offset, offset + 9, buf);

    int greenx = (c[offset + 4] << 2) + ((c[offset] & 0x0c) >> 2);
    int greeny = (c[offset + 5] << 2) + (c[offset] & 0x03);
    snprintf(buf, sizeof(buf), "Chromacity green: X %1.3f, Y %1.3f",
             edid_convert_color(greenx), edid_convert_color(greeny));
    edid_ann_field(di, s, offset, offset + 9, buf);

    int bluex = (c[offset + 6] << 2) + ((c[offset + 1] & 0xc0) >> 6);
    int bluey = (c[offset + 7] << 2) + ((c[offset + 1] & 0x30) >> 4);
    snprintf(buf, sizeof(buf), "Chromacity blue: X %1.3f, Y %1.3f",
             edid_convert_color(bluex), edid_convert_color(bluey));
    edid_ann_field(di, s, offset, offset + 9, buf);

    int whitex = (c[offset + 8] << 2) + ((c[offset + 1] & 0x0c) >> 2);
    int whitey = (c[offset + 9] << 2) + (c[offset + 1] & 0x03);
    snprintf(buf, sizeof(buf), "Chromacity white: X %1.3f, Y %1.3f",
             edid_convert_color(whitex), edid_convert_color(whitey));
    edid_ann_field(di, s, offset, offset + 9, buf);
}

static void edid_decode_est_timing(struct srd_decoder_inst *di, edid_state *s, int offset)
{
    uint8_t *c = edid_get_cache(s);
    int bitmap = (c[offset] << 9) + (c[offset + 1] << 1) + ((c[offset + 2] & 0x80) >> 7);
    char modestr[512] = "";
    for (int i = 0; i < 17; i++) {
        if (bitmap & (1 << (16 - i))) {
            strcat(modestr, est_modes[i]);
            strcat(modestr, ", ");
        }
    }
    if (modestr[0]) {
        modestr[strlen(modestr) - 2] = '\0';
        char buf[600];
        snprintf(buf, sizeof(buf), "Supported established modes: %s", modestr);
        edid_ann_field(di, s, offset, offset + 2, buf);
    }
}

static void edid_decode_std_timing(struct srd_decoder_inst *di, edid_state *s, int offset)
{
    uint8_t *c = edid_get_cache(s);
    char modestr[512] = "";
    for (int i = 0; i < 16; i += 2) {
        if (c[offset + i] == 0x01 && c[offset + i + 1] == 0x01)
            continue;
        int x = (c[offset + i] + 31) * 8;
        int ratio = (c[offset + i + 1] & 0xc0) >> 6;
        int rx = xy_ratio[ratio][0];
        int ry = xy_ratio[ratio][1];
        int y = x * ry / rx;
        int refresh = (c[offset + i + 1] & 0x3f) + 60;
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%dx%d@%dHz, ", x, y, refresh);
        strcat(modestr, tmp);
    }
    if (modestr[0]) {
        modestr[strlen(modestr) - 2] = '\0';
        char buf[600];
        snprintf(buf, sizeof(buf), "Supported standard modes: %s", modestr);
        edid_ann_field(di, s, offset, offset + 15, buf);
    }
}

static void edid_decode_detailed_timing(struct srd_decoder_inst *di, edid_state *s,
    uint8_t *cache, uint64_t (*sn_ptr)[2], int offset, int is_first)
{
    char section[64];
    if (is_first && s->have_preferred_timing)
        snprintf(section, sizeof(section), "Preferred timing descriptor");
    else
        snprintf(section, sizeof(section), "Detailed timing descriptor");
    c_put(di, sn_ptr[0][0], sn_ptr[17][1], s->out_ann, ANN_SECTIONS, section);

    char buf[512];

    double pixclock = (double)((cache[1] << 8) + cache[0]) / 100.0;
    snprintf(buf, sizeof(buf), "Pixel clock: %.2f MHz", pixclock);
    edid_ann_field(di, s, offset, offset + 1, buf);

    int horiz_active = ((cache[4] & 0xf0) << 4) + cache[2];
    int horiz_blank = ((cache[4] & 0x0f) << 8) + cache[3];
    snprintf(buf, sizeof(buf), "Horizontal active: %d, blanking: %d", horiz_active, horiz_blank);
    edid_ann_field(di, s, offset + 2, offset + 4, buf);

    int vert_active = ((cache[7] & 0xf0) << 4) + cache[5];
    int vert_blank = ((cache[7] & 0x0f) << 8) + cache[6];
    snprintf(buf, sizeof(buf), "Vertical active: %d, blanking: %d", vert_active, vert_blank);
    edid_ann_field(di, s, offset + 5, offset + 7, buf);

    int horiz_sync_off = ((cache[11] & 0xc0) << 2) + cache[8];
    int horiz_sync_pw  = ((cache[11] & 0x30) << 4) + cache[9];
    int vert_sync_off  = ((cache[11] & 0x0c) << 2) + ((cache[10] & 0xf0) >> 4);
    int vert_sync_pw   = ((cache[11] & 0x03) << 4) + (cache[10] & 0x0f);
    snprintf(buf, sizeof(buf),
             "Horizontal sync offset: %d, pulse width: %d, Vertical sync offset: %d, pulse width: %d",
             horiz_sync_off, horiz_sync_pw, vert_sync_off, vert_sync_pw);
    edid_ann_field(di, s, offset + 8, offset + 11, buf);

    int horiz_size = ((cache[14] & 0xf0) << 4) + cache[12];
    int vert_size  = ((cache[14] & 0x0f) << 8) + cache[13];
    snprintf(buf, sizeof(buf), "Physical size: %dx%dmm", horiz_size, vert_size);
    edid_ann_field(di, s, offset + 12, offset + 14, buf);

    snprintf(buf, sizeof(buf), "Horizontal border: %d pixels", cache[15]);
    edid_ann_field(di, s, offset + 15, offset + 15, buf);

    snprintf(buf, sizeof(buf), "Vertical border: %d lines", cache[16]);
    edid_ann_field(di, s, offset + 16, offset + 16, buf);

    /* Features byte */
    char features[512] = "Flags: ";
    if (cache[17] & 0x80)
        strcat(features, "interlaced, ");
    int stereo = (cache[17] & 0x60) >> 5;
    if (stereo) {
        if (cache[17] & 0x01)
            strcat(features, "2-way interleaved stereo, ");
        else
            strcat(features, "field sequential stereo, ");
    }
    int sync = (cache[17] & 0x18) >> 3;
    int sync2 = (cache[17] & 0x06) >> 1;
    const char *posneg[] = {"negative", "positive"};
    if (sync == 0x00)
        strcat(features, "analog composite (serrate on RGB)");
    else if (sync == 0x01)
        strcat(features, "bipolar analog composite (serrate on RGB)");
    else if (sync == 0x02) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "digital composite (serrate on composite polarity %s)",
                 posneg[sync2 & 0x01]);
        strcat(features, tmp);
    } else if (sync == 0x03) {
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "digital separate (Vsync polarity %s, Hsync polarity %s)",
                 posneg[(sync2 & 0x02) >> 1], posneg[sync2 & 0x01]);
        strcat(features, tmp);
    }
    strcat(features, ", ");
    features[strlen(features) - 2] = '\0';
    edid_ann_field(di, s, offset + 17, offset + 17, features);
}

static void edid_decode_descriptor(struct srd_decoder_inst *di, edid_state *s,
    uint8_t *cache, uint64_t (*sn_ptr)[2], int offset)
{
    uint8_t tag = cache[3];
    edid_ann_field(di, s, offset, offset + 1, "Flag");
    edid_ann_field(di, s, offset + 2, offset + 2, "Flag (reserved)");
    char tagbuf[32];
    snprintf(tagbuf, sizeof(tagbuf), "Tag: %X", tag);
    edid_ann_field(di, s, offset + 3, offset + 3, tagbuf);
    edid_ann_field(di, s, offset + 4, offset + 4, "Flag");

    if (tag == 0xff) {
        c_put(di, sn_ptr[offset][0], sn_ptr[offset + 17][1],
                  s->out_ann, ANN_SECTIONS, "Serial number");
        char text[14] = {0};
        for (int i = 0; i < 13; i++)
            text[i] = (cache[5 + i] >= 32 && cache[5 + i] <= 126) ? (char)cache[5 + i] : ' ';
        /* Strip trailing spaces */
        for (int i = 12; i >= 0 && text[i] == ' '; i--)
            text[i] = '\0';
        edid_ann_field(di, s, offset + 5, offset + 17, text);
    } else if (tag == 0xfe) {
        c_put(di, sn_ptr[offset][0], sn_ptr[offset + 17][1],
                  s->out_ann, ANN_SECTIONS, "Text");
        char text[14] = {0};
        for (int i = 0; i < 13; i++)
            text[i] = (cache[5 + i] >= 32 && cache[5 + i] <= 126) ? (char)cache[5 + i] : ' ';
        for (int i = 12; i >= 0 && text[i] == ' '; i--)
            text[i] = '\0';
        edid_ann_field(di, s, offset + 5, offset + 17, text);
    } else if (tag == 0xfc) {
        c_put(di, sn_ptr[offset][0], sn_ptr[offset + 17][1],
                  s->out_ann, ANN_SECTIONS, "Monitor name");
        char text[14] = {0};
        for (int i = 0; i < 13; i++)
            text[i] = (cache[5 + i] >= 32 && cache[5 + i] <= 126) ? (char)cache[5 + i] : ' ';
        for (int i = 12; i >= 0 && text[i] == ' '; i--)
            text[i] = '\0';
        edid_ann_field(di, s, offset + 5, offset + 17, text);
    } else if (tag == 0xfd) {
        c_put(di, sn_ptr[offset][0], sn_ptr[offset + 17][1],
                  s->out_ann, ANN_SECTIONS, "Monitor range limits");
        char buf[256];
        snprintf(buf, sizeof(buf), "Minimum vertical rate: %dHz", cache[5]);
        edid_ann_field(di, s, offset + 5, offset + 5, buf);
        snprintf(buf, sizeof(buf), "Maximum vertical rate: %dHz", cache[6]);
        edid_ann_field(di, s, offset + 6, offset + 6, buf);
        snprintf(buf, sizeof(buf), "Minimum horizontal rate: %dkHz", cache[7]);
        edid_ann_field(di, s, offset + 7, offset + 7, buf);
        snprintf(buf, sizeof(buf), "Maximum horizontal rate: %dkHz", cache[8]);
        edid_ann_field(di, s, offset + 8, offset + 8, buf);
        snprintf(buf, sizeof(buf), "Maximum pixel clock: %dMHz", cache[9] * 10);
        edid_ann_field(di, s, offset + 9, offset + 9, buf);
        if (cache[10] == 0x02) {
            edid_ann_field(di, s, offset + 10, offset + 10, "Secondary timing formula supported");
            edid_ann_field(di, s, offset + 11, offset + 17, "GTF");
        } else {
            edid_ann_field(di, s, offset + 10, offset + 10, "Secondary timing formula unsupported");
            edid_ann_field(di, s, offset + 11, offset + 17, "Padding");
        }
    } else if (tag == 0xfb) {
        c_put(di, sn_ptr[offset][0], sn_ptr[offset + 17][1],
                  s->out_ann, ANN_SECTIONS, "Additional color point data");
    } else if (tag == 0xfa) {
        c_put(di, sn_ptr[offset][0], sn_ptr[offset + 17][1],
                  s->out_ann, ANN_SECTIONS, "Additional standard timing definitions");
    } else {
        c_put(di, sn_ptr[offset][0], sn_ptr[offset + 17][1],
                  s->out_ann, ANN_SECTIONS, "Unknown descriptor");
    }
}

static void edid_decode_descriptors(struct srd_decoder_inst *di, edid_state *s, int offset)
{
    uint8_t *c = edid_get_cache(s);
    uint64_t (*sn_ptr)[2] = edid_get_sn(s);

    for (int i = offset; i < offset + 72; i += 18) {
        if (c[i] != 0 || c[i + 1] != 0) {
            edid_decode_detailed_timing(di, s, &c[i], &sn_ptr[i], i, i == offset);
        } else {
            if (c[i + 2] == 0 || c[i + 4] == 0) {
                edid_decode_descriptor(di, s, &c[i], &sn_ptr[i], i);
            }
        }
    }
}

static void edid_decode_data_block_collection(struct srd_decoder_inst *di, edid_state *s,
    uint8_t *cache, uint64_t (*sn_ptr)[2], int len)
{
    int offset = 0;
    while (offset < len) {
        int length = 1 + (cache[offset] & 0x1f);
        int tag = cache[offset] >> 5;
        char buf[512];

        if (tag < 7) {
            const char *code_names[] = {
                "0: Reserved",
                "1: Audio Data Block",
                "2: Video Data Block",
                "3: Vendor Specific Data Block",
                "4: Speaker Allocation Data Block",
                "5: VESA DTC Data Block",
                "6: Reserved",
            };
            c_put(di, sn_ptr[offset][0], sn_ptr[offset][1],
                      s->out_ann, ANN_FIELDS, code_names[tag]);

            if (tag == 1 && length >= 4) {
                int aformat = cache[1] >> 3;
                int channels = (cache[1] & 0x7) + 1;
                char rates_str[128] = "";
                const char *rate_names[] = {"192", "176", "96", "88", "48", "44", "32"};
                for (int i = 0; i < 7; i++) {
                    if ((1 << i) & cache[2]) {
                        strcat(rates_str, rate_names[6 - i]);
                        strcat(rates_str, " ");
                    }
                }
                snprintf(buf, sizeof(buf), "Format: %d Channels: %d Rates: %s",
                         aformat, channels, rates_str);
            } else if (tag == 2) {
                char vic_str[256] = "VIC: ";
                for (int j = 1; j < length; j++) {
                    char tmp[32];
                    snprintf(tmp, sizeof(tmp), "%d%s ", cache[j] & 0x7f,
                             (cache[j] >> 7) ? " (Native)" : "");
                    strcat(vic_str, tmp);
                }
                snprintf(buf, sizeof(buf), "%s", vic_str);
            } else if (tag == 3 && length >= 6) {
                snprintf(buf, sizeof(buf), "OUI: %02X %02X %02X, PhyAddr: %d.%d.%d.%d",
                         cache[3], cache[4], cache[5],
                         cache[4] >> 4, cache[4] & 0xf,
                         cache[5] >> 4, cache[5] & 0xf);
            } else {
                char hex_str[256] = "";
                for (int j = 1; j < length && strlen(hex_str) < 200; j++) {
                    char tmp[8];
                    snprintf(tmp, sizeof(tmp), "%02X ", cache[j]);
                    strcat(hex_str, tmp);
                }
                snprintf(buf, sizeof(buf), "%s", hex_str);
            }
            if (length > 1)
                c_put(di, sn_ptr[offset + 1][0], sn_ptr[offset + length - 1][1],
                          s->out_ann, ANN_FIELDS, buf);
        } else {
            /* Extended tags */
            const char *ext_name = "Unknown";
            if (cache[1] == 0)
                ext_name = "0: Video Capability Data Block";
            else if (cache[1] == 1)
                ext_name = "1: Vendor Specific Video Data Block";
            else if (cache[1] == 17)
                ext_name = "17: Vendor Specific Audio Data Block";

            char code_buf[256];
            snprintf(code_buf, sizeof(code_buf), "7: Extended, %s", ext_name);
            c_put(di, sn_ptr[offset][0], sn_ptr[offset + 1][1],
                      s->out_ann, ANN_FIELDS, code_buf);

            char hex_str[256] = "";
            for (int j = 2; j < length && strlen(hex_str) < 200; j++) {
                char tmp[8];
                snprintf(tmp, sizeof(tmp), "%02X ", cache[j]);
                strcat(hex_str, tmp);
            }
            if (length > 2)
                c_put(di, sn_ptr[offset + 2][0], sn_ptr[offset + length - 1][1],
                          s->out_ann, ANN_FIELDS, hex_str);
        }

        offset += length;
    }
}

static void edid_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    edid_state *s = (edid_state *)c_decoder_get_private(di);
    if (!s)
        return;

    s->ss = start_sample;
    s->es = end_sample;
    uint8_t databyte = (fields && n_fields > 0) ? fields[0].u8 : 0;

    /* ADDRESS WRITE + 0x50 → offset state */
    if (strcmp(cmd, "ADDRESS WRITE") == 0 && databyte == 0x50) {
        s->state = EDID_STATE_OFFSET;
        s->ss = start_sample;
        return;
    }

    /* ADDRESS READ + 0x50 → header or extensions */
    if (strcmp(cmd, "ADDRESS READ") == 0 && databyte == 0x50) {
        if (s->extension > 0) {
            s->state = EDID_STATE_EXTENSIONS;
            char ext_str[64];
            snprintf(ext_str, sizeof(ext_str), "Extension: %d", s->extension);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_SECTIONS,
                      ext_str, ext_str);
        } else {
            s->state = EDID_STATE_HEADER;
            c_put(di, start_sample, end_sample, s->out_ann, ANN_SECTIONS, "EDID");
        }
        return;
    }

    /* DATA WRITE + offset state */
    if (strcmp(cmd, "DATA WRITE") == 0 && s->state == EDID_STATE_OFFSET) {
        s->offset = databyte;
        s->extension = s->offset / 128;
        s->cnt = s->offset % 128;

        /* Truncate or extend cache/sn arrays */
        if (s->extension > 0) {
            int ext = s->extension - 1;
            if (ext < MAX_EXTENSIONS) {
                /* Clear data beyond cnt */
                for (int i = s->cnt; i < 128; i++) {
                    s->ext_cache[ext][i] = 0;
                    s->ext_sn[ext][i][0] = 0;
                    s->ext_sn[ext][i][1] = 0;
                }
            }
        } else {
            for (int i = s->cnt; i < 128; i++) {
                s->cache[i] = 0;
                s->sn[i][0] = 0;
                s->sn[i][1] = 0;
            }
        }

        char buf[32];
        snprintf(buf, sizeof(buf), "Offset: %d", databyte);
        c_put(di, s->ss ? s->ss : start_sample, end_sample,
                  s->out_ann, ANN_SECTIONS, buf, buf);
        return;
    }

    /* Only process DATA READ */
    if (strcmp(cmd, "DATA READ") != 0)
        return;

    s->cnt++;
    /* Store in cache and sn */
    if (s->extension > 0) {
        int ext = s->extension - 1;
        if (ext < MAX_EXTENSIONS && s->cnt - 1 < 128) {
            s->ext_sn[ext][s->cnt - 1][0] = start_sample;
            s->ext_sn[ext][s->cnt - 1][1] = end_sample;
            s->ext_cache[ext][s->cnt - 1] = databyte;
        }
    } else {
        if (s->cnt - 1 < 128) {
            s->sn[s->cnt - 1][0] = start_sample;
            s->sn[s->cnt - 1][1] = end_sample;
            s->cache[s->cnt - 1] = databyte;
        }
    }

    if (s->state == EDID_STATE_IDLE || s->state == EDID_STATE_HEADER) {
        /* Wait for the EDID header */
        if (s->cnt >= OFF_VENDOR) {
            uint8_t *c = edid_get_cache(s);
            int match = 1;
            for (int i = 0; i < 8; i++) {
                if (c[s->cnt - 8 + i] != EDID_HEADER[i]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                /* Throw away any garbage before the header */
                uint64_t (*sn_ptr)[2] = edid_get_sn(s);
                uint64_t header_ss = sn_ptr[s->cnt - 8][0];
                memmove(s->cache, &s->cache[s->cnt - 8], 8);
                memmove(s->sn, &s->sn[s->cnt - 8], 8 * sizeof(uint64_t[2]));
                s->cnt = 8;
                s->state = EDID_STATE_EDID;
                c_put(di, header_ss, end_sample, s->out_ann, ANN_SECTIONS, "Header");
                c_put(di, header_ss, end_sample, s->out_ann, ANN_FIELDS, "Header pattern");
            }
        }
    } else if (s->state == EDID_STATE_EDID) {
        if (s->cnt == OFF_VERSION) {
            edid_decode_vid(di, s, -10);
            edid_decode_pid(di, s, -8);
            edid_decode_serial(di, s, -6);
            edid_decode_mfrdate(di, s, -2);
            uint64_t (*sn_ptr)[2] = edid_get_sn(s);
            c_put(di, sn_ptr[OFF_VENDOR][0], end_sample,
                      s->out_ann, ANN_SECTIONS, "Vendor/product");
        } else if (s->cnt == OFF_BASIC) {
            uint64_t (*sn_ptr)[2] = edid_get_sn(s);
            c_put(di, sn_ptr[OFF_VERSION][0], end_sample,
                      s->out_ann, ANN_SECTIONS, "EDID Version");
            uint8_t *c = edid_get_cache(s);
            char buf[64];
            snprintf(buf, sizeof(buf), "Version %d", c[OFF_VERSION]);
            c_put(di, sn_ptr[OFF_VERSION][0], sn_ptr[OFF_VERSION][1],
                      s->out_ann, ANN_FIELDS, buf);
            snprintf(buf, sizeof(buf), "Revision %d", c[OFF_VERSION + 1]);
            c_put(di, sn_ptr[OFF_VERSION + 1][0], sn_ptr[OFF_VERSION + 1][1],
                      s->out_ann, ANN_FIELDS, buf);
        } else if (s->cnt == OFF_CHROM) {
            uint64_t (*sn_ptr)[2] = edid_get_sn(s);
            c_put(di, sn_ptr[OFF_BASIC][0], end_sample,
                      s->out_ann, ANN_SECTIONS, "Basic display");
            edid_decode_basicdisplay(di, s, -5);
        } else if (s->cnt == OFF_EST_TIMING) {
            uint64_t (*sn_ptr)[2] = edid_get_sn(s);
            c_put(di, sn_ptr[OFF_CHROM][0], end_sample,
                      s->out_ann, ANN_SECTIONS, "Color characteristics");
            edid_decode_chromaticity(di, s, -10);
        } else if (s->cnt == OFF_STD_TIMING) {
            uint64_t (*sn_ptr)[2] = edid_get_sn(s);
            c_put(di, sn_ptr[OFF_EST_TIMING][0], end_sample,
                      s->out_ann, ANN_SECTIONS, "Established timings");
            edid_decode_est_timing(di, s, -3);
        } else if (s->cnt == OFF_DET_TIMING) {
            uint64_t (*sn_ptr)[2] = edid_get_sn(s);
            c_put(di, sn_ptr[OFF_STD_TIMING][0], end_sample,
                      s->out_ann, ANN_SECTIONS, "Standard timings");
            edid_decode_std_timing(di, s, s->cnt - 16);
        } else if (s->cnt == OFF_NUM_EXT) {
            edid_decode_descriptors(di, s, -72);
        } else if (s->cnt == OFF_CHECKSUM) {
            char buf[64];
            uint8_t *c = edid_get_cache(s);
            snprintf(buf, sizeof(buf), "Extensions present: %d", c[s->cnt - 1]);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_FIELDS, buf);
        } else if (s->cnt == OFF_CHECKSUM + 1) {
            uint8_t *c = edid_get_cache(s);
            int checksum = 0;
            for (int i = 0; i < 128; i++)
                checksum += c[i];
            const char *csstr = (checksum % 256 == 0) ? "OK" : "WRONG!";
            char buf[64];
            snprintf(buf, sizeof(buf), "Checksum: %d (%s)", c[s->cnt - 1], csstr);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_FIELDS, buf);
            s->state = EDID_STATE_EXTENSIONS;
        }
    } else if (s->state == EDID_STATE_EXTENSIONS) {
        uint8_t *cache = NULL;
        uint64_t (*sn_ptr)[2] = NULL;
        int ext = s->extension - 1;
        if (ext >= 0 && ext < MAX_EXTENSIONS) {
            cache = s->ext_cache[ext];
            sn_ptr = s->ext_sn[ext];
        }
        if (!cache || !sn_ptr)
            return;

        int v = cache[s->cnt - 1];
        if (s->cnt == 1) {
            if (v == 2)
                c_put(di, start_sample, end_sample, s->out_ann, ANN_SECTIONS, "Extensions Tag", "Tag");
            else
                c_put(di, start_sample, end_sample, s->out_ann, ANN_SECTIONS, "Bad Tag");
        } else if (s->cnt == 2) {
            c_put(di, start_sample, end_sample, s->out_ann, ANN_SECTIONS, "Version");
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", v);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_FIELDS, buf);
        } else if (s->cnt == 3) {
            c_put(di, start_sample, end_sample, s->out_ann, ANN_SECTIONS, "DTD offset");
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", v);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_FIELDS, buf);
        } else if (s->cnt == 4) {
            c_put(di, start_sample, end_sample, s->out_ann, ANN_SECTIONS,
                      "Format support | DTD count");
            const char *underscan = (v & 0x80) ? "yes" : "no";
            const char *audio = (v & 0x40) ? "Basic" : "No";
            const char *ycbcr_vals[] = {"None", "422", "444", "422+444"};
            const char *ycbcr = ycbcr_vals[(v & 0x30) >> 4];
            char buf[512];
            snprintf(buf, sizeof(buf),
                     "Underscan: %s, %s Audio, YCbCr: %s, DTDs: %d",
                     underscan, audio, ycbcr, v & 0xf);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_FIELDS, buf);
        } else if (s->cnt <= cache[2]) {
            if (s->cnt == cache[2]) {
                c_put(di, sn_ptr[4][0], end_sample,
                          s->out_ann, ANN_SECTIONS, "Data block collection");
                int n_fields = cache[2] - 4;
                if (n_fields > 0)
                    edid_decode_data_block_collection(di, s, &cache[4], &sn_ptr[4], n_fields);
            }
        } else if ((s->cnt - cache[2]) % 18 == 0) {
            int n = (s->cnt - cache[2]) / 18;
            if (n <= (cache[3] & 0xf)) {
                c_put(di, sn_ptr[s->cnt - 18][0], end_sample,
                          s->out_ann, ANN_SECTIONS, "DTD");
                /* Parse as detailed timing or descriptor */
                int desc_offset = s->cnt - 18;
                if (cache[desc_offset] != 0 || cache[desc_offset + 1] != 0) {
                    edid_decode_detailed_timing(di, s, &cache[desc_offset],
                        &sn_ptr[desc_offset], desc_offset, 0);
                } else {
                    edid_decode_descriptor(di, s, &cache[desc_offset],
                        &sn_ptr[desc_offset], desc_offset);
                }
            }
        } else if (s->cnt == 127) {
            int dtd_last = cache[2] + (cache[3] & 0xf) * 18;
            if (dtd_last < 128)
                c_put(di, sn_ptr[dtd_last][0], end_sample,
                          s->out_ann, ANN_SECTIONS, "Padding");
        } else if (s->cnt == 128) {
            int checksum = 0;
            for (int i = 0; i < 128; i++)
                checksum += cache[i];
            char buf[64];
            snprintf(buf, sizeof(buf), "Checksum: %d (%s)",
                     cache[s->cnt - 1], (checksum % 256) ? "Wrong" : "OK");
            c_put(di, start_sample, end_sample, s->out_ann, ANN_FIELDS, buf);
        }
    }
}

static void edid_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(edid_state)));
    }
    edid_state *s = (edid_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(edid_state));
    s->state = EDID_STATE_IDLE;
}

static void edid_start(struct srd_decoder_inst *di)
{
    edid_state *s = (edid_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "edid");
}

static void edid_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void edid_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder edid_c_decoder = {
    .id = "edid_c",
    .name = "EDID(C)",
    .longname = "Extended Display Identification Data (C)",
    .desc = "Data structure describing display device capabilities. (C implementation)",
    .license = "gplv3+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = edid_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = edid_ann_rows,
    .inputs = edid_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = edid_tags,
    .num_tags = 3,
    .reset = edid_reset,
    .start = edid_start,
    .decode = edid_decode,
    .destroy = edid_destroy,
    .decode_upper = edid_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &edid_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}