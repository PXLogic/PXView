#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

#define T55XX_MAX_BITS 70

enum t55xx_ann {
    ANN_BIT_VALUE = 0,
    ANN_START_GAP,
    ANN_WRITE_GAP,
    ANN_WRITE_MODE_EXIT,
    ANN_BIT,
    ANN_OPCODE,
    ANN_LOCK,
    ANN_DATA,
    ANN_PASSWORD,
    ANN_ADDRESS,
    ANN_BITRATE,
    NUM_ANN,
};

enum t55xx_state {
    STATE_START_GAP = 0,
    STATE_WRITE_GAP,
};

typedef struct {
    uint64_t samplerate;
    uint64_t last_samplenum;
    uint64_t lastlast_samplenum;
    int state;

    struct { int bit_val; uint64_t ss; uint64_t es; } bits_pos[T55XX_MAX_BITS];
    int bit_nr;

    uint64_t field_clock;
    uint64_t wzmax, wzmin, womax, womin;
    uint64_t startgap, writegap, nogap;

    uint64_t oldsamplenum;
    uint64_t old_gap_start, old_gap_end;
    int gap_detected;

    int em4100_decode1_partial;
    int em4100_decode;

    uint64_t coilfreq;
    uint64_t start_gap_val;
    uint64_t w_gap_val;
    uint64_t w_one_min_val;
    uint64_t w_one_max_val;
    uint64_t w_zero_min_val;
    uint64_t w_zero_max_val;

    int out_ann;
} t55xx_state;

static struct srd_channel t55xx_channels[] = {
    {"data", "Data", "Data line", 0, SRD_CHANNEL_SDATA, NULL},
};

static struct srd_decoder_option t55xx_options[] = {
    {"coilfreq", NULL, "Coil frequency", NULL, NULL},
    {"start_gap", NULL, "Start gap min", NULL, NULL},
    {"w_gap", NULL, "Write gap min", NULL, NULL},
    {"w_one_min", NULL, "Write one min", NULL, NULL},
    {"w_one_max", NULL, "Write one max", NULL, NULL},
    {"w_zero_min", NULL, "Write zero min", NULL, NULL},
    {"w_zero_max", NULL, "Write zero max", NULL, NULL},
    {"em4100_decode", NULL, "EM4100 decode", NULL, NULL},
};

static const char *t55xx_ann_labels[][3] = {
    {"", "bit_value", "Bit value"},
    {"", "start_gap", "Start gap"},
    {"", "write_gap", "Write gap"},
    {"", "write_mode_exit", "Write mode exit"},
    {"", "bit", "Bit"},
    {"", "opcode", "Opcode"},
    {"", "lock", "Lock"},
    {"", "data", "Data"},
    {"", "password", "Password"},
    {"", "address", "Address"},
    {"", "bitrate", "Bitrate"},
};

static const int row_bits_classes[] = {ANN_BIT_VALUE, -1};
static const int row_structure_classes[] = {ANN_START_GAP, ANN_WRITE_GAP, ANN_WRITE_MODE_EXIT, ANN_BIT, -1};
static const int row_fields_classes[] = {ANN_OPCODE, ANN_LOCK, ANN_DATA, ANN_PASSWORD, ANN_ADDRESS, -1};
static const int row_decode_classes[] = {ANN_BITRATE, -1};

static const struct srd_c_ann_row t55xx_ann_rows[] = {
    {"bits", "Bits", row_bits_classes, 1},
    {"structure", "Structure", row_structure_classes, 4},
    {"fields", "Fields", row_fields_classes, 5},
    {"decode", "Decode", row_decode_classes, 1},
};

static const char *t55xx_inputs[] = {"logic"};
static const char *t55xx_tags[] = {"IC", "RFID"};

static void t55xx_put4bits(struct srd_decoder_inst *di, t55xx_state *s, int idx)
{
    int val = (s->bits_pos[idx].bit_val << 3) | (s->bits_pos[idx + 1].bit_val << 2) |
              (s->bits_pos[idx + 2].bit_val << 1) | s->bits_pos[idx + 3].bit_val;
    char buf[32];
    snprintf(buf, sizeof(buf), "%X", val);
    c_put(di, s->bits_pos[idx].ss, s->bits_pos[idx + 3].es,
              s->out_ann, ANN_BITRATE, buf);
}

static void t55xx_decode_config(struct srd_decoder_inst *di, t55xx_state *s, int idx)
{
    static const char *br_string[] = {"RF/8", "RF/16", "RF/32", "RF/40",
                                       "RF/50", "RF/64", "RF/100", "RF/128"};
    static const char *mod_str1[] = {"Direct", "Manchester", "Biphase", "Reserved"};
    static const char *mod_str2[] = {"Direct", "PSK1", "PSK2", "PSK3",
                                      "FSK1", "FSK2", "FSK1a", "FSK2a"};
    static const char *pskcf_str[] = {"RF/2", "RF/4", "RF/8", "Reserved"};

    if (idx + 27 > s->bit_nr)
        return;

    /* Safer Key (4 bits) */
    int safer_key = (s->bits_pos[idx].bit_val << 3) | (s->bits_pos[idx + 1].bit_val << 2) |
                    (s->bits_pos[idx + 2].bit_val << 1) | s->bits_pos[idx + 3].bit_val;
    char buf[64];
    snprintf(buf, sizeof(buf), "Safer Key: %X", safer_key);
    c_put(di, s->bits_pos[idx].ss, s->bits_pos[idx + 3].es, s->out_ann, ANN_BITRATE, buf);

    /* Data Bit Rate (3 bits at idx+11) */
    if (idx + 13 < s->bit_nr) {
        int bitrate = (s->bits_pos[idx + 11].bit_val << 2) |
                      (s->bits_pos[idx + 12].bit_val << 1) |
                      s->bits_pos[idx + 13].bit_val;
        if (bitrate < 8) {
            snprintf(buf, sizeof(buf), "Data Bit Rate: %s", br_string[bitrate]);
            c_put(di, s->bits_pos[idx + 11].ss, s->bits_pos[idx + 13].es,
                      s->out_ann, ANN_BITRATE, buf);
        }
    }

    /* Modulation (5 bits at idx+15..19) */
    if (idx + 19 < s->bit_nr) {
        int modulation1 = (s->bits_pos[idx + 15].bit_val << 1) |
                          s->bits_pos[idx + 16].bit_val;
        int modulation2 = (s->bits_pos[idx + 17].bit_val << 2) |
                          (s->bits_pos[idx + 18].bit_val << 1) |
                          s->bits_pos[idx + 19].bit_val;
        const char *mod_string;
        if (modulation1 == 0 && modulation2 < 8)
            mod_string = mod_str2[modulation2];
        else if (modulation1 < 4)
            mod_string = mod_str1[modulation1];
        else
            mod_string = "Unknown";

        snprintf(buf, sizeof(buf), "Modulation: %s", mod_string);
        c_put(di, s->bits_pos[idx + 15].ss, s->bits_pos[idx + 19].es,
                  s->out_ann, ANN_BITRATE, buf);
    }

    /* PSK-CF (2 bits at idx+20..21) */
    if (idx + 21 < s->bit_nr) {
        int pskcf = (s->bits_pos[idx + 20].bit_val << 1) |
                    s->bits_pos[idx + 21].bit_val;
        if (pskcf < 4) {
            snprintf(buf, sizeof(buf), "PSK-CF: %s", pskcf_str[pskcf]);
            c_put(di, s->bits_pos[idx + 20].ss, s->bits_pos[idx + 21].es,
                      s->out_ann, ANN_BITRATE, buf);
        }
    }

    /* AOR (1 bit at idx+22) */
    if (idx + 22 < s->bit_nr) {
        snprintf(buf, sizeof(buf), "AOR: %d", s->bits_pos[idx + 22].bit_val);
        c_put(di, s->bits_pos[idx + 22].ss, s->bits_pos[idx + 22].es,
                  s->out_ann, ANN_BITRATE, buf);
    }

    /* Max-Block (3 bits at idx+23..25) */
    if (idx + 25 < s->bit_nr) {
        int maxblock = (s->bits_pos[idx + 23].bit_val << 2) |
                       (s->bits_pos[idx + 24].bit_val << 1) |
                       s->bits_pos[idx + 25].bit_val;
        snprintf(buf, sizeof(buf), "Max-Block: %d", maxblock);
        c_put(di, s->bits_pos[idx + 23].ss, s->bits_pos[idx + 25].es,
                  s->out_ann, ANN_BITRATE, buf);
    }

    /* PWD (1 bit at idx+26) */
    if (idx + 26 < s->bit_nr) {
        snprintf(buf, sizeof(buf), "PWD: %d", s->bits_pos[idx + 26].bit_val);
        c_put(di, s->bits_pos[idx + 26].ss, s->bits_pos[idx + 26].es,
                  s->out_ann, ANN_BITRATE, buf);
    }
}

static void t55xx_em4100_decode1(struct srd_decoder_inst *di, t55xx_state *s, int idx)
{
    if (idx + 32 > s->bit_nr)
        return;

    c_put(di, s->bits_pos[idx].ss, s->bits_pos[idx + 8].es,
              s->out_ann, ANN_BITRATE, "EM4100 header", "EM header", "Header", "H");

    /* Output 4 nibbles */
    t55xx_put4bits(di, s, idx + 9);
    t55xx_put4bits(di, s, idx + 14);
    t55xx_put4bits(di, s, idx + 19);
    t55xx_put4bits(di, s, idx + 24);

    /* Partial nibble */
    s->em4100_decode1_partial = (s->bits_pos[idx + 29].bit_val << 3) |
                                 (s->bits_pos[idx + 30].bit_val << 2) |
                                 (s->bits_pos[idx + 31].bit_val << 1);
    c_put(di, s->bits_pos[idx + 29].ss, s->bits_pos[idx + 31].es,
              s->out_ann, ANN_BITRATE, "Partial nibble");
}

static void t55xx_put_fields(struct srd_decoder_inst *di, t55xx_state *s)
{
    if (s->bit_nr == 70) {
        /* Opcode: bits 0-1 (2 bits) */
        char op_buf[16];
        snprintf(op_buf, sizeof(op_buf), "Opcode: %d%d", s->bits_pos[0].bit_val, s->bits_pos[1].bit_val);
        c_put(di, s->bits_pos[0].ss, s->bits_pos[1].es, s->out_ann, ANN_OPCODE, op_buf);

        /* Password: bits 2-33 (32 bits) */
        uint64_t password = 0;
        for (int i = 2; i < 34; i++)
            password = (password << 1) | s->bits_pos[i].bit_val;
        char pw_buf[64];
        snprintf(pw_buf, sizeof(pw_buf), "Password: %X", (unsigned)password);
        c_put(di, s->bits_pos[2].ss, s->bits_pos[33].es, s->out_ann, ANN_PASSWORD, pw_buf);

        /* Lock: bit 34 */
        char lock_buf[16];
        snprintf(lock_buf, sizeof(lock_buf), "Lock: %X", s->bits_pos[34].bit_val);
        c_put(di, s->bits_pos[34].ss, s->bits_pos[34].es, s->out_ann, ANN_LOCK, lock_buf);

        /* Data: bits 35-66 (32 bits) */
        uint64_t data = 0;
        for (int i = 35; i < 67; i++)
            data = (data << 1) | s->bits_pos[i].bit_val;
        char data_buf[64];
        snprintf(data_buf, sizeof(data_buf), "Data: %X", (unsigned)data);
        c_put(di, s->bits_pos[35].ss, s->bits_pos[66].es, s->out_ann, ANN_DATA, data_buf);

        /* Addr: bits 67-69 (3 bits) */
        int addr = (s->bits_pos[67].bit_val << 2) | (s->bits_pos[68].bit_val << 1) | s->bits_pos[69].bit_val;
        char addr_buf[32];
        snprintf(addr_buf, sizeof(addr_buf), "Addr: %X", addr);
        c_put(di, s->bits_pos[67].ss, s->bits_pos[69].es, s->out_ann, ANN_ADDRESS, addr_buf);

        if (addr == 0)
            t55xx_decode_config(di, s, 35);
        if (addr == 7) {
            char pw2_buf[64];
            snprintf(pw2_buf, sizeof(pw2_buf), "Password: %X", (unsigned)data);
            c_put(di, s->bits_pos[35].ss, s->bits_pos[66].es, s->out_ann, ANN_BITRATE, pw2_buf);
        }
        if (addr == 1 && s->em4100_decode)
            t55xx_em4100_decode1(di, s, 35);
    }

    if (s->bit_nr == 38) {
        /* Opcode: bits 0-1 (2 bits) */
        char op_buf[16];
        snprintf(op_buf, sizeof(op_buf), "Opcode: %d%d", s->bits_pos[0].bit_val, s->bits_pos[1].bit_val);
        c_put(di, s->bits_pos[0].ss, s->bits_pos[1].es, s->out_ann, ANN_OPCODE, op_buf);

        /* Lock: bit 2 */
        char lock_buf[16];
        snprintf(lock_buf, sizeof(lock_buf), "Lock: %X", s->bits_pos[2].bit_val);
        c_put(di, s->bits_pos[2].ss, s->bits_pos[2].es, s->out_ann, ANN_LOCK, lock_buf);

        /* Data: bits 3-34 (32 bits) */
        uint64_t data = 0;
        for (int i = 3; i < 35; i++)
            data = (data << 1) | s->bits_pos[i].bit_val;
        char data_buf[64];
        snprintf(data_buf, sizeof(data_buf), "Data: %X", (unsigned)data);
        c_put(di, s->bits_pos[3].ss, s->bits_pos[34].es, s->out_ann, ANN_DATA, data_buf);

        /* Addr: bits 35-37 (3 bits) */
        int addr = (s->bits_pos[35].bit_val << 2) | (s->bits_pos[36].bit_val << 1) | s->bits_pos[37].bit_val;
        char addr_buf[32];
        snprintf(addr_buf, sizeof(addr_buf), "Addr: %X", addr);
        c_put(di, s->bits_pos[35].ss, s->bits_pos[37].es, s->out_ann, ANN_ADDRESS, addr_buf);

        if (addr == 0)
            t55xx_decode_config(di, s, 3);
        if (addr == 7) {
            char pw_buf[64];
            snprintf(pw_buf, sizeof(pw_buf), "Password: %X", (unsigned)data);
            c_put(di, s->bits_pos[3].ss, s->bits_pos[34].es, s->out_ann, ANN_BITRATE, pw_buf);
        }
        if (addr == 1 && s->em4100_decode)
            t55xx_em4100_decode1(di, s, 3);
    }

    if (s->bit_nr == 2) {
        /* Opcode: bits 0-1 (2 bits) */
        char op_buf[16];
        snprintf(op_buf, sizeof(op_buf), "Opcode: %d%d", s->bits_pos[0].bit_val, s->bits_pos[1].bit_val);
        c_put(di, s->bits_pos[0].ss, s->bits_pos[1].es, s->out_ann, ANN_OPCODE, op_buf);
    }

    s->bit_nr = 0;
}

static void t55xx_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(t55xx_state)));
    t55xx_state *s = (t55xx_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(t55xx_state));
    s->state = STATE_START_GAP;
    s->out_ann = -1;
    s->coilfreq = 125000;
    s->start_gap_val = 20;
    s->w_gap_val = 20;
    s->w_one_min_val = 48;
    s->w_one_max_val = 63;
    s->w_zero_min_val = 16;
    s->w_zero_max_val = 31;
    s->em4100_decode = 1;
}

static void t55xx_start(struct srd_decoder_inst *di)
{
    t55xx_state *s = (t55xx_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "t55xx");

    s->coilfreq = c_opt_int(di, "coilfreq", 125000);
    s->start_gap_val = c_opt_int(di, "start_gap", 20);
    s->w_gap_val = c_opt_int(di, "w_gap", 20);
    s->w_one_min_val = c_opt_int(di, "w_one_min", 48);
    s->w_one_max_val = c_opt_int(di, "w_one_max", 63);
    s->w_zero_min_val = c_opt_int(di, "w_zero_min", 16);
    s->w_zero_max_val = c_opt_int(di, "w_zero_max", 31);

    const char *em_str = c_opt_str(di, "em4100_decode", "on");
    s->em4100_decode = (strcmp(em_str, "on") == 0) ? 1 : 0;
}

static void t55xx_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    t55xx_state *s = (t55xx_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        if (value > 0 && s->coilfreq > 0) {
            s->field_clock = value / s->coilfreq;
            s->wzmax = s->w_zero_max_val * s->field_clock;
            s->wzmin = s->w_zero_min_val * s->field_clock;
            s->womax = s->w_one_max_val * s->field_clock;
            s->womin = s->w_one_min_val * s->field_clock;
            s->startgap = s->start_gap_val * s->field_clock;
            s->writegap = s->w_gap_val * s->field_clock;
            s->nogap = 64 * s->field_clock;
        }
    }
}

static void t55xx_decode(struct srd_decoder_inst *di)
{
    t55xx_state *s = (t55xx_state *)c_decoder_get_private(di);
    uint64_t last_samplenum = 0;

    if (!s->samplerate || !s->field_clock)
        return;

    s->lastlast_samplenum = 0;
    s->oldsamplenum = 0;
    s->old_gap_start = 0;
    s->old_gap_end = 0;
    s->gap_detected = 0;
    s->bit_nr = 0;

    while (1) {
        int ret = c_wait(di, CW_E(0), CW_END);
        if (ret != SRD_OK)
            return;

        uint64_t pl = di_samplenum(di) - s->oldsamplenum;

        if (s->state == STATE_WRITE_GAP) {
            if (pl > s->writegap) {
                s->gap_detected = 1;
                c_put(di, last_samplenum, di_samplenum(di), s->out_ann, ANN_WRITE_GAP, "Write gap");
            }
            if ((last_samplenum - s->old_gap_end) > s->nogap) {
                s->gap_detected = 0;
                s->state = STATE_START_GAP;
                c_put(di, s->old_gap_end, last_samplenum, s->out_ann, ANN_WRITE_MODE_EXIT,
                          "Write mode exit", "Exit", "X");
                t55xx_put_fields(di, s);
            }
        }

        if (s->state == STATE_START_GAP) {
            if (pl > s->startgap) {
                s->gap_detected = 1;
                c_put(di, last_samplenum, di_samplenum(di), s->out_ann, ANN_START_GAP, "Start gap");
                s->state = STATE_WRITE_GAP;
            }
        }

        if (s->gap_detected == 1) {
            s->gap_detected = 0;
            if ((last_samplenum - s->old_gap_end) > s->wzmin \
                    && (last_samplenum - s->old_gap_end) < s->wzmax) {
                c_put(di, s->old_gap_end, last_samplenum,
                          s->out_ann, ANN_BIT_VALUE, "0");
                c_put(di, s->old_gap_end, last_samplenum,
                          s->out_ann, ANN_BIT, "0");
                if (s->bit_nr < T55XX_MAX_BITS) {
                    s->bits_pos[s->bit_nr].bit_val = 0;
                    s->bits_pos[s->bit_nr].ss = s->old_gap_end;
                    s->bits_pos[s->bit_nr].es = last_samplenum;
                    s->bit_nr++;
                }
            }
            if ((last_samplenum - s->old_gap_end) > s->womin \
                    && (last_samplenum - s->old_gap_end) < s->womax) {
                c_put(di, s->old_gap_end, last_samplenum,
                          s->out_ann, ANN_BIT_VALUE, "1");
                c_put(di, s->old_gap_end, last_samplenum,
                          s->out_ann, ANN_BIT, "1");
                if (s->bit_nr < T55XX_MAX_BITS) {
                    s->bits_pos[s->bit_nr].bit_val = 1;
                    s->bits_pos[s->bit_nr].ss = s->old_gap_end;
                    s->bits_pos[s->bit_nr].es = last_samplenum;
                    s->bit_nr++;
                }
            }

            s->old_gap_start = last_samplenum;
            s->old_gap_end = di_samplenum(di);
        }

        s->oldsamplenum = di_samplenum(di);
        last_samplenum = di_samplenum(di);
    }
}

static void t55xx_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder t55xx_c_decoder = {
    .id = "t55xx_c",
    .name = "T55xx(C)",
    .longname = "T55xx RFID (C)",
    .desc = "T55xx 100-150kHz RFID protocol (C implementation)",
    .license = "gplv2+",
    .channels = t55xx_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = t55xx_options,
    .num_options = 8,
    .num_annotations = NUM_ANN,
    .ann_labels = t55xx_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = t55xx_ann_rows,
    .inputs = t55xx_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = t55xx_tags,
    .num_tags = 2,
    .reset = t55xx_reset,
    .start = t55xx_start,
    .decode = t55xx_decode,
    .destroy = t55xx_destroy,
    .state_size = 0,
    .metadata = t55xx_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    t55xx_options[0].idn = "dec_t55xx_opt_coilfreq";
    t55xx_options[0].def = g_variant_new_uint64(125000);

    t55xx_options[1].idn = "dec_t55xx_opt_start_gap";
    t55xx_options[1].def = g_variant_new_uint64(20);

    t55xx_options[2].idn = "dec_t55xx_opt_w_gap";
    t55xx_options[2].def = g_variant_new_uint64(20);

    t55xx_options[3].idn = "dec_t55xx_opt_w_one_min";
    t55xx_options[3].def = g_variant_new_uint64(48);

    t55xx_options[4].idn = "dec_t55xx_opt_w_one_max";
    t55xx_options[4].def = g_variant_new_uint64(63);

    t55xx_options[5].idn = "dec_t55xx_opt_w_zero_min";
    t55xx_options[5].def = g_variant_new_uint64(16);

    t55xx_options[6].idn = "dec_t55xx_opt_w_zero_max";
    t55xx_options[6].def = g_variant_new_uint64(31);

    t55xx_options[7].idn = "dec_t55xx_opt_em4100_decode";
    t55xx_options[7].def = g_variant_new_string("on");
    GSList *em_vals = NULL;
    em_vals = g_slist_append(em_vals, g_variant_new_string("on"));
    em_vals = g_slist_append(em_vals, g_variant_new_string("off"));
    t55xx_options[7].values = em_vals;

    return &t55xx_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}