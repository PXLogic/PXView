/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2011 Gareth McMullin <gareth@blacksphere.co.nz>
 * Copyright (C) 2012-2013 Uwe Hermann <uwe@hermann-uwe.de>
 * Copyright (C) 2019 DreamSourceLab <support@dreamsourcelab.com>
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

/* Channel indices — match Python: DP=0, DM=1 */
#define CH_DP 0
#define CH_DM 1

/* Annotation class indices — match Python annotations tuple */
enum usb_ann {
    ANN_J           = 0,
    ANN_K           = 1,
    ANN_SE0         = 2,
    ANN_SE1         = 3,
    ANN_SOP         = 4,
    ANN_EOP         = 5,
    ANN_BIT         = 6,
    ANN_STUFFBIT    = 7,
    ANN_ERROR       = 8,
    ANN_KEEP_ALIVE  = 9,
    ANN_RESET       = 10,
    NUM_ANN,
};

/* Symbol types — match Python symbols dict */
enum usb_sym {
    SYM_J = 0,
    SYM_K,
    SYM_SE0,
    SYM_SE1,
    SYM_FS_J,
    SYM_LS_J,
};

/* State machine — match Python self.state */
enum usb_state {
    STATE_IDLE,
    STATE_GET_BIT,
    STATE_GET_EOP,
    STATE_WAIT_IDLE,
};

/* Signalling mode — match Python signalling option + 'automatic' + 'low-speed-rp' */
enum usb_signalling_mode {
    SIG_AUTO = 0,
    SIG_FULL_SPEED,
    SIG_LOW_SPEED,
    SIG_LOW_SPEED_RP,
};

/* Decoder state struct — C_DECODER_STATE auto-generates usb_s typedef,
 * usb_reset (calloc), and usb_destroy (free). */
C_DECODER_STATE(usb, {
    enum usb_state state;
    enum usb_signalling_mode signalling;
    enum usb_sym oldsym;
    uint64_t samplerate;
    double bitrate;
    double bitwidth;
    double samplepos;
    uint64_t samplenum_target;
    uint64_t samplenum_edge;
    uint64_t samplenum_lastedge;
    uint8_t edgepins_dp;
    uint8_t edgepins_dm;
    int consecutive_ones;
    char bits[17];
    int bits_len;
    uint64_t ss_block;
    int out_ann;
    int out_python;
});

/* Channel definitions — match Python channels tuple */
static struct srd_channel usb_channels[] = {
    {"dp", "D+", "USB D+ signal", 0, SRD_CHANNEL_COMMON, "dec_usb_signalling_chan_dp"},
    {"dm", "D-", "USB D- signal", 1, SRD_CHANNEL_COMMON, "dec_usb_signalling_chan_dm"},
};

/* Options — match Python options tuple */
static struct srd_decoder_option usb_options_arr[1];

/* Annotation labels — match Python annotations tuple */
static const char *usb_ann_labels[][3] = {
    {"", "sym-j",       "J symbol"},
    {"", "sym-k",       "K symbol"},
    {"", "sym-se0",     "SE0 symbol"},
    {"", "sym-se1",     "SE1 symbol"},
    {"", "sop",         "Start of packet (SOP)"},
    {"", "eop",         "End of packet (EOP)"},
    {"", "bit",         "Bit"},
    {"", "stuffbit",    "Stuff bit"},
    {"", "error",       "Error"},
    {"", "keep-alive",  "Low-speed keep-alive"},
    {"", "reset",       "Reset"},
};

/* Annotation row class lists */
static const int usb_row_bits_classes[] = {ANN_SOP, ANN_EOP, ANN_BIT, ANN_STUFFBIT, ANN_ERROR, ANN_KEEP_ALIVE, ANN_RESET, -1};
static const int usb_row_symbols_classes[] = {ANN_J, ANN_K, ANN_SE0, ANN_SE1, -1};

static const struct srd_c_ann_row usb_ann_rows[] = {
    {"bits",    "Bits",    usb_row_bits_classes,    7},
    {"symbols", "Symbols", usb_row_symbols_classes, 4},
};

static const char *usb_inputs[] = {"logic", NULL};
static const char *usb_outputs[] = {"usb_signalling", NULL};
static const char *usb_tags[] = {"PC", NULL};

/* ---- Helper: get symbol from DP/DM pins — match Python symbols dict ---- */
static enum usb_sym get_symbol(enum usb_signalling_mode mode, uint8_t dp, uint8_t dm)
{
    if (dp == 0 && dm == 0) return SYM_SE0;
    if (dp == 1 && dm == 1) return SYM_SE1;
    if (mode == SIG_LOW_SPEED || mode == SIG_LOW_SPEED_RP) {
        return (dp == 1 && dm == 0) ? SYM_K : SYM_J;
    } else if (mode == SIG_FULL_SPEED) {
        return (dp == 1 && dm == 0) ? SYM_J : SYM_K;
    } else {
        /* SIG_AUTO */
        return (dp == 1 && dm == 0) ? SYM_FS_J : SYM_LS_J;
    }
}

/* ---- Helper: check if symbol is J in current mode ---- */
static int sym_is_j(enum usb_sym s, enum usb_signalling_mode mode)
{
    if (mode == SIG_LOW_SPEED || mode == SIG_LOW_SPEED_RP)
        return s == SYM_J;
    if (mode == SIG_FULL_SPEED)
        return s == SYM_J;
    return 0;
}

/* ---- Helper: check if symbol is K in current mode ---- */
static int sym_is_k(enum usb_sym s, enum usb_signalling_mode mode)
{
    if (mode == SIG_LOW_SPEED || mode == SIG_LOW_SPEED_RP)
        return s == SYM_K;
    if (mode == SIG_FULL_SPEED)
        return s == SYM_K;
    return 0;
}

/* ---- Helper: get symbol name string ---- */
static const char *get_sym_name(enum usb_sym sym)
{
    switch (sym) {
    case SYM_J: case SYM_FS_J: case SYM_LS_J: return "J";
    case SYM_K: return "K";
    case SYM_SE0: return "SE0";
    case SYM_SE1: return "SE1";
    default: return "";
    }
}

/* ---- Helper: output symbol annotation ---- */
static void put_sym_ann(struct srd_decoder_inst *di, usb_s *s, enum usb_sym sym)
{
    switch (sym) {
    case SYM_J: case SYM_FS_J: case SYM_LS_J:
        c_put(di, s->samplenum_lastedge, s->samplenum_edge, s->out_ann, ANN_J, "J");
        break;
    case SYM_K:
        c_put(di, s->samplenum_lastedge, s->samplenum_edge, s->out_ann, ANN_K, "K");
        break;
    case SYM_SE0:
        c_put(di, s->samplenum_lastedge, s->samplenum_edge, s->out_ann, ANN_SE0, "SE0", "0");
        break;
    case SYM_SE1:
        c_put(di, s->samplenum_lastedge, s->samplenum_edge, s->out_ann, ANN_SE1, "SE1", "1");
        break;
    default:
        break;
    }
}

/* ---- Helper: update bitrate from signalling mode ---- */
static void update_bitrate(usb_s *s)
{
    if (s->signalling == SIG_LOW_SPEED || s->signalling == SIG_LOW_SPEED_RP)
        s->bitrate = 1500000.0;
    else if (s->signalling == SIG_FULL_SPEED)
        s->bitrate = 12000000.0;
    else
        return;
    if (s->samplerate > 0)
        s->bitwidth = (double)s->samplerate / s->bitrate;
}

/* ---- Helper: set new target samplenum — match Python set_new_target_samplenum() ---- */
static void set_new_target(usb_s *s)
{
    s->samplepos += s->bitwidth;
    s->samplenum_target = (uint64_t)s->samplepos;
    s->samplenum_lastedge = s->samplenum_edge;
    s->samplenum_edge = (uint64_t)(s->samplepos - (s->bitwidth / 2.0));
}

/* ---- Helper: handle_idle — match Python handle_idle() ---- */
static void usb_handle_idle(struct srd_decoder_inst *di, usb_s *s, enum usb_sym sym)
{
    s->samplenum_edge = di_samplenum(di);
    double se0_length = (double)(di_samplenum(di) - s->samplenum_lastedge) / (double)s->samplerate;
    if (se0_length > 2.5e-6) {
        c_put(di, s->samplenum_lastedge, s->samplenum_edge, s->out_ann, ANN_RESET, "Reset", "Res", "R");
        c_proto(di, s->samplenum_lastedge, s->samplenum_edge, s->out_python, "RESET", C_END);
        /* Reset signalling to option value, matching Python */
        const char *sig = c_opt_str(di, "signalling", "automatic");
        if (strcmp(sig, "full-speed") == 0)
            s->signalling = SIG_FULL_SPEED;
        else if (strcmp(sig, "low-speed") == 0)
            s->signalling = SIG_LOW_SPEED;
        else
            s->signalling = SIG_AUTO;
    } else if (se0_length > 1.2e-6 && s->signalling == SIG_LOW_SPEED) {
        c_put(di, s->samplenum_lastedge, s->samplenum_edge, s->out_ann, ANN_KEEP_ALIVE, "Keep-alive", "KA", "A");
        c_proto(di, s->samplenum_lastedge, s->samplenum_edge, s->out_python, "KEEP ALIVE", C_END);
    }

    /* Auto-detect signalling mode, matching Python's handle_idle() */
    {
        const char *sig = c_opt_str(di, "signalling", "automatic");
        if (strcmp(sig, "automatic") == 0) {
            if (sym == SYM_FS_J) {
                s->signalling = SIG_FULL_SPEED;
            } else if (sym == SYM_LS_J) {
                s->signalling = SIG_LOW_SPEED;
            } else {
                s->signalling = SIG_AUTO;
            }
        } else if (strcmp(sig, "full-speed") == 0) {
            s->signalling = SIG_FULL_SPEED;
        } else if (strcmp(sig, "low-speed") == 0) {
            s->signalling = SIG_LOW_SPEED;
        }
    }
    update_bitrate(s);

    /* Always set oldsym = SYM_J, matching Python's handle_idle() */
    s->oldsym = SYM_J;
    s->state = STATE_IDLE;
}

/* ---- Helper: wait_for_sop — match Python wait_for_sop() ---- */
static void usb_wait_for_sop(struct srd_decoder_inst *di, usb_s *s, enum usb_sym sym)
{
    /* Wait for a Start of Packet (SOP), i.e. a J->K symbol change. */
    if (!sym_is_k(sym, s->signalling) || !sym_is_j(s->oldsym, s->signalling))
        return;
    s->consecutive_ones = 0;
    s->bits_len = 0;
    update_bitrate(s);
    s->samplepos = (double)di_samplenum(di) - (s->bitwidth / 2.0) + 0.5;
    set_new_target(s);
    c_put(di, s->samplenum_edge, s->samplenum_edge, s->out_ann, ANN_SOP, "SOP", "S");
    c_proto(di, s->samplenum_edge, s->samplenum_edge, s->out_python, "SOP", C_END);
    s->state = STATE_GET_BIT;
}

/* ---- Helper: handle_bit — match Python handle_bit() ---- */
static void usb_handle_bit(struct srd_decoder_inst *di, usb_s *s, int b)
{
    if (s->consecutive_ones == 6) {
        if (b == 0) {
            /* Stuff bit */
            c_put(di, s->samplenum_lastedge, s->samplenum_edge, s->out_ann, ANN_STUFFBIT, "Stuff bit: 0", "SB: 0", "0");
            c_proto(di, s->samplenum_lastedge, s->samplenum_edge, s->out_python, "STUFF BIT", C_END);
            s->consecutive_ones = 0;
        } else {
            c_put(di, s->samplenum_lastedge, s->samplenum_edge, s->out_ann, ANN_ERROR, "Bit stuff error", "BS ERR", "B");
            c_proto(di, s->samplenum_lastedge, s->samplenum_edge, s->out_python, "ERR", C_END);
            s->state = STATE_IDLE;
        }
    } else {
        /* Normal bit (not a stuff bit) */
        char bstr[2] = {(char)('0' + b), 0};
        c_put(di, s->samplenum_lastedge, s->samplenum_edge, s->out_ann, ANN_BIT, bstr);
        c_proto(di, s->samplenum_lastedge, s->samplenum_edge, s->out_python, "BIT", C_U8(b), C_END);
        if (b == 1)
            s->consecutive_ones++;
        else
            s->consecutive_ones = 0;
    }
}

/* ---- Helper: get_eop — match Python get_eop() ---- */
static void usb_get_eop(struct srd_decoder_inst *di, usb_s *s, enum usb_sym sym)
{
    set_new_target(s);
    put_sym_ann(di, s, sym);
    {
        const char *sn = get_sym_name(sym);
        c_proto(di, s->samplenum_lastedge, s->samplenum_edge, s->out_python, "SYM", C_STR(sn), C_END);
    }
    s->oldsym = sym;

    if (sym == SYM_SE0) {
        /* continue */
    } else if (sym_is_j(sym, s->signalling)) {
        /* Got an EOP */
        c_put(di, s->ss_block, s->samplenum_edge, s->out_ann, ANN_EOP, "EOP", "E");
        c_proto(di, s->ss_block, s->samplenum_edge, s->out_python, "EOP", C_END);
        s->state = STATE_WAIT_IDLE;
    } else {
        c_put(di, s->ss_block, s->samplenum_edge, s->out_ann, ANN_ERROR, "EOP Error", "EErr", "E");
        c_proto(di, s->ss_block, s->samplenum_edge, s->out_python, "ERR", C_END);
        s->state = STATE_IDLE;
    }
}

/* ---- Helper: get_bit — match Python get_bit() ---- */
static void usb_get_bit(struct srd_decoder_inst *di, usb_s *s, enum usb_sym sym)
{
    set_new_target(s);
    int b = (s->oldsym == sym) ? 1 : 0;
    s->oldsym = sym;

    if (sym == SYM_SE0) {
        /* Start of an EOP */
        s->state = STATE_GET_EOP;
        s->ss_block = s->samplenum_lastedge;
    } else {
        usb_handle_bit(di, s, b);

        /* Output SYM annotation after BIT/STUFF BIT/ERR */
        put_sym_ann(di, s, sym);
        {
            const char *sn = get_sym_name(sym);
            c_proto(di, s->samplenum_lastedge, s->samplenum_edge, s->out_python, "SYM", C_STR(sn), C_END);
        }

        if (s->state == STATE_IDLE)
            return;

        if (s->bits_len < 16) {
            s->bits[s->bits_len++] = '0' + b;
            s->bits[s->bits_len] = 0;
        }
        if (s->bits_len == 16 && strcmp(s->bits, "0000000100111100") == 0) {
            /* Sync and low-speed PREamble seen */
            c_proto(di, s->samplenum_edge, s->samplenum_edge, s->out_python, "EOP", C_END);
            s->signalling = SIG_LOW_SPEED_RP;
            update_bitrate(s);
            s->oldsym = SYM_J;
            s->state = STATE_IDLE;
            return;
        }

        if (b == 0) {
            enum usb_sym edgesym = get_symbol(s->signalling, s->edgepins_dp, s->edgepins_dm);
            if (edgesym != SYM_SE0 && edgesym != SYM_SE1) {
                if (edgesym == sym) {
                    s->bitwidth = s->bitwidth - (0.001 * s->bitwidth);
                    s->samplepos = s->samplepos - (0.01 * s->bitwidth);
                } else {
                    s->bitwidth = s->bitwidth + (0.001 * s->bitwidth);
                    s->samplepos = s->samplepos + (0.01 * s->bitwidth);
                }
            }
        }
    }

    /* Output SE0 SYM annotation when entering GET_EOP */
    if (sym == SYM_SE0) {
        put_sym_ann(di, s, sym);
        {
            const char *sn = get_sym_name(sym);
            c_proto(di, s->samplenum_lastedge, s->samplenum_edge, s->out_python, "SYM", C_STR(sn), C_END);
        }
    }
}

/* start callback — match Python start() */
static void usb_start(struct srd_decoder_inst *di)
{
    usb_s *s = (usb_s *)c_decoder_get_private(di);

    s->out_ann    = c_reg_out(di, SRD_OUTPUT_ANN, "usb_signalling");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "usb_signalling");

    const char *sig = c_opt_str(di, "signalling", "automatic");
    if (strcmp(sig, "full-speed") == 0)
        s->signalling = SIG_FULL_SPEED;
    else if (strcmp(sig, "low-speed") == 0)
        s->signalling = SIG_LOW_SPEED;
    else
        s->signalling = SIG_AUTO;

    s->samplerate = c_samplerate(di);
    if (s->samplerate > 0)
        update_bitrate(s);
}

/* metadata callback — match Python metadata() */
static void usb_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    usb_s *s = (usb_s *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        update_bitrate(s);
    }
}

/* decode callback — main state machine, match Python decode() */
static void usb_decode(struct srd_decoder_inst *di)
{
    usb_s *s = (usb_s *)c_decoder_get_private(di);
    int ret;

    if (!s->samplerate)
        return;

    uint8_t dp, dm;
    enum usb_sym sym;

    /* Read the first sample to seed internal state, matching Python's
     * "self.wait()" before the main loop, followed by handle_idle(). */
    ret = c_wait(di, CW_SKIP(0), CW_END);
    if (ret != SRD_OK)
        return;

    dp = c_pin(di, CH_DP);
    dm = c_pin(di, CH_DM);
    sym = get_symbol(s->signalling, dp, dm);

    /* handle_idle(sym) equivalent */
    s->samplenum_edge = di_samplenum(di);
    /* se0_length = 0, no Reset/Keep-alive emitted */

    /* Auto-detect signalling mode, matching Python's handle_idle() logic */
    {
        const char *sig = c_opt_str(di, "signalling", "automatic");
        if (strcmp(sig, "automatic") == 0) {
            if (sym == SYM_FS_J) {
                s->signalling = SIG_FULL_SPEED;
                update_bitrate(s);
            } else if (sym == SYM_LS_J) {
                s->signalling = SIG_LOW_SPEED;
                update_bitrate(s);
            } else {
                s->signalling = SIG_AUTO;
            }
        } else if (strcmp(sig, "full-speed") == 0) {
            s->signalling = SIG_FULL_SPEED;
            update_bitrate(s);
        } else if (strcmp(sig, "low-speed") == 0) {
            s->signalling = SIG_LOW_SPEED;
            update_bitrate(s);
        }
    }
    /* Always set oldsym = SYM_J, matching Python's handle_idle() */
    s->oldsym = SYM_J;
    s->edgepins_dp = dp;
    s->edgepins_dm = dm;

    while (1) {
        if (s->state == STATE_IDLE) {
            /* Python: self.wait([{0: 'e'}, {1: 'e'}]) */
            ret = c_wait(di, CW_E(CH_DP), CW_OR, CW_E(CH_DM), CW_END);
            if (ret != SRD_OK)
                return;

            dp = c_pin(di, CH_DP);
            dm = c_pin(di, CH_DM);
            sym = get_symbol(s->signalling, dp, dm);
            s->edgepins_dp = dp;
            s->edgepins_dm = dm;

            if (sym == SYM_SE0) {
                s->samplenum_lastedge = di_samplenum(di);
                s->state = STATE_WAIT_IDLE;
            } else {
                usb_wait_for_sop(di, s, sym);
            }

        } else if (s->state == STATE_GET_BIT || s->state == STATE_GET_EOP) {
            /* Wait until we're in the middle of the desired bit.
             * Python: if (self.samplenum_edge > self.samplenum):
             *             (dp, dm) = self.wait([{'skip': ...}]) */
            if (s->samplenum_edge > di_samplenum(di)) {
                ret = c_wait(di, CW_SKIP(s->samplenum_edge - di_samplenum(di)), CW_END);
                if (ret != SRD_OK)
                    return;
                dp = c_pin(di, CH_DP);
                dm = c_pin(di, CH_DM);
                s->edgepins_dp = dp;
                s->edgepins_dm = dm;
            }
            if (s->samplenum_target > di_samplenum(di)) {
                ret = c_wait(di, CW_SKIP(s->samplenum_target - di_samplenum(di)), CW_END);
                if (ret != SRD_OK)
                    return;
                dp = c_pin(di, CH_DP);
                dm = c_pin(di, CH_DM);
            }

            sym = get_symbol(s->signalling, dp, dm);

            if (s->state == STATE_GET_BIT) {
                usb_get_bit(di, s, sym);
            } else if (s->state == STATE_GET_EOP) {
                usb_get_eop(di, s, sym);
            }

        } else if (s->state == STATE_WAIT_IDLE) {
            /* Skip "all-low" input. Wait for high level on either DP or DM.
             * Matching Python: self.wait() then while not dp and not dm: self.wait([{0:'h'},{1:'h'}]) */
            ret = c_wait(di, CW_SKIP(1), CW_END);
            if (ret != SRD_OK)
                return;

            dp = c_pin(di, CH_DP);
            dm = c_pin(di, CH_DM);

            while (!dp && !dm) {
                ret = c_wait(di, CW_H(CH_DP), CW_OR, CW_H(CH_DM), CW_END);
                if (ret != SRD_OK)
                    return;
                dp = c_pin(di, CH_DP);
                dm = c_pin(di, CH_DM);
            }

            s->edgepins_dp = dp;
            s->edgepins_dm = dm;

            if (di_samplenum(di) - s->samplenum_lastedge > 1) {
                /* handle_idle(sym) equivalent — use SIG_AUTO to get FS_J/LS_J
                 * symbols for proper idle detection, matching Python's
                 * symbols[self.options['signalling']] */
                sym = get_symbol(SIG_AUTO, dp, dm);
                usb_handle_idle(di, s, sym);
            } else {
                /* samplenum - samplenum_lastedge <= 1: check for SOP */
                sym = get_symbol(s->signalling, dp, dm);
                usb_wait_for_sop(di, s, sym);
                /* If no SOP: state remains STATE_WAIT_IDLE, matching Python */
            }
        }
    }
}

/* ---- Decoder definition (v4 API) ---- */
static struct srd_c_decoder usb_c_def = {
    .id = "usb_signalling_c",
    .name = "USB signalling",
    .longname = "Universal Serial Bus (LS/FS) signalling",
    .desc = "USB (low-speed/full-speed) signalling protocol.",
    .license = "gplv2+",
    .channels = usb_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = usb_options_arr,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = usb_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = usb_ann_rows,
    .inputs = usb_inputs,
    .num_inputs = 1,
    .outputs = usb_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = usb_tags,
    .num_tags = 1,
    .state_size = sizeof(usb_s),
    .reset = usb_reset,
    .start = usb_start,
    .decode = usb_decode,
    .end = NULL,
    .metadata = usb_metadata,
    .destroy = usb_destroy,
    .decode_upper = NULL,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    GVariant *vals[] = {
        g_variant_new_string("automatic"),
        g_variant_new_string("full-speed"),
        g_variant_new_string("low-speed"),
    };
    GSList *val_list = NULL;
    val_list = g_slist_append(val_list, vals[0]);
    val_list = g_slist_append(val_list, vals[1]);
    val_list = g_slist_append(val_list, vals[2]);
    usb_options_arr[0].id = "signalling";
    usb_options_arr[0].idn = "dec_usb_signalling_opt_signalling";
    usb_options_arr[0].desc = "Signalling";
    usb_options_arr[0].def = g_variant_new_string("automatic");
    usb_options_arr[0].values = val_list;
    return &usb_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}