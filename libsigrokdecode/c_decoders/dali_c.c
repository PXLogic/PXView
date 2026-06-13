#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum dali_state {
    STATE_IDLE,
    STATE_PHASE0,
    STATE_PHASE1,
};

enum dali_ann {
    ANN_BIT = 0,
    ANN_STARTBIT,
    ANN_SBIT,
    ANN_YBIT,
    ANN_ADDRESS,
    ANN_COMMAND,
    ANN_REPLY,
    ANN_RAW,
    NUM_ANN,
};

#define DALI_CH 0
#define MAX_EDGES 40
#define MAX_BITS 17

struct dali_priv {
    enum dali_state state;
    uint64_t samplerate;
    uint64_t halfbit;
    int old_dali;
    int phase0;
    uint64_t edges[MAX_EDGES];
    int num_edges;
    uint64_t bits_samplenum[MAX_BITS];
    int bits_value[MAX_BITS];
    int num_bits;
    uint64_t ss_es_bits[MAX_BITS][2];
    int num_ss_es_bits;
    int dev_type;
    int polarity_invert;
    int out_ann;
};

static struct srd_channel dali_channels[] = {
    {"dali", "DALI", "DALI data line", 0, SRD_CHANNEL_SDATA, "dec_dali_chan_dali"},
};

static struct srd_decoder_option dali_options_arr[] = {
    {
        .id = "polarity",
        .idn = "dec_dali_opt_polarity",
        .desc = "Polarity",
        .def = NULL,
        .values = NULL,
    },
};

static const char *dali_ann_labels[][3] = {
    {"", "bit", "Bit"},
    {"", "startbit", "Startbit"},
    {"", "sbit", "Select bit"},
    {"", "ybit", "Individual or group"},
    {"", "address", "Address"},
    {"", "command", "Command"},
    {"", "reply", "Reply data"},
    {"", "raw", "Raw data"},
};

static const int dali_row_bits_classes[] = {ANN_BIT, -1};
static const int dali_row_raw_classes[] = {ANN_RAW, -1};
static const int dali_row_fields_classes[] = {ANN_STARTBIT, ANN_SBIT, ANN_YBIT, ANN_ADDRESS, ANN_COMMAND, ANN_REPLY, -1};
static const struct srd_c_ann_row dali_ann_rows[] = {
    {"bits", "Bits", dali_row_bits_classes, 1},
    {"raw", "Raw data", dali_row_raw_classes, 1},
    {"fields", "Fields", dali_row_fields_classes, 6},
};

static const char *dali_inputs[] = {"logic", NULL};
static const char *dali_outputs[] = {NULL};
static const char *dali_tags[] = {"Embedded/industrial", "Lighting", NULL};

static const struct { uint8_t code; const char *long_name; const char *short_name; } dali_extended_cmds[] = {
    {0xA1, "Terminate special processes", "Terminate"},
    {0xA3, "DTR = DATA", "DTR"},
    {0xA5, "INITIALISE", "INIT"},
    {0xA7, "RANDOMISE", "RAND"},
    {0xA9, "COMPARE", "COMP"},
    {0xAB, "WITHDRAW", "WDRAW"},
    {0xB1, "SET SEARCH H", "SAH"},
    {0xB3, "SET SEARCH M", "SAM"},
    {0xB5, "SET SEARCH L", "SAL"},
    {0xB7, "Program Short Address", "ProgSA"},
    {0xB9, "Verify Short Address", "VfySA"},
    {0xBB, "Query Short Address", "QryShort"},
    {0xBD, "Physical Selection", "PysSel"},
    {0xC1, "Enable Device Type X", "EnTyp"},
    {0xC3, "DTR1 = DATA", "DTR1"},
    {0xC5, "DTR2 = DATA", "DTR2"},
    {0xC7, "Write Memory Location", "WRI"},
};

static const struct { uint8_t code; const char *long_name; const char *short_name; } dali_cmds[] = {
    {0x00, "Immediate Off", "IOFF"},
    {0x01, "Up 200ms", "Up"},
    {0x02, "Down 200ms", "Down"},
    {0x03, "Step Up", "Step+"},
    {0x04, "Step Down", "Step-"},
    {0x05, "Recall Maximum Level", "Recall Max"},
    {0x06, "Recall Minimum Level", "Recall Min"},
    {0x07, "Step down and off", "Down Off"},
    {0x08, "Step ON and UP", "On Up"},
    {0x20, "Reset", "Rst"},
    {0x21, "Store Dim Level in DTR", "Level -> DTR"},
    {0x2A, "Store DTR as Max Level", "DTR->Max"},
    {0x2B, "Store DTR as Min Level", "DTR->Min"},
    {0x2C, "Store DTR as Fail Level", "DTR->Fail"},
    {0x2D, "Store DTR as Power On Level", "DTR->Poweron"},
    {0x2E, "Store DTR as Fade Time", "DTR->Fade"},
    {0x2F, "Store DTR as Fade Rate", "DTR->Rate"},
    {0x80, "Store DTR as Short Address", "DTR->Add"},
    {0x81, "Enable Memory Write", "WEn"},
    {0x90, "Query Status", "Status"},
    {0x91, "Query Ballast", "Ballast"},
    {0x92, "Query Lamp Failure", "LmpFail"},
    {0x93, "Query Power On", "Power On"},
    {0x94, "Query Limit Error", "Limit Err"},
    {0x95, "Query Reset", "Reset State"},
    {0x96, "Query Missing Short Address", "NoSrt"},
    {0x97, "Query Version", "Ver"},
    {0x98, "Query DTR", "GetDTR"},
    {0x99, "Query Device Type", "Type"},
    {0x9A, "Query Physical Minimum", "PhysMin"},
    {0x9B, "Query Power Fail", "PowerFailed"},
    {0x9C, "Query DTR1", "GetDTR1"},
    {0x9D, "Query DTR2", "GetDTR2"},
    {0xA0, "Query Level", "GetLevel"},
    {0xA1, "Query Max Level", "GetMax"},
    {0xA2, "Query Min Level", "GetMin"},
    {0xA3, "Query Power On", "GetPwrOn"},
    {0xA4, "Query Fail Level", "GetFail"},
    {0xA5, "Query Fade Rate", "GetRate"},
    {0xA6, "Query Power Fail", "PwrFail"},
    {0xC0, "Query Groups 0-7", "GetGrpsL"},
    {0xC1, "Query Groups 7-15", "GetGrpsH"},
    {0xC2, "Query BRNH", "BRNH"},
    {0xC3, "Query BRNM", "BRNM"},
    {0xC4, "Query BRNL", "BRNL"},
    {0xC5, "Query Memory", "GetMem"},
};

static const struct { uint8_t code; const char *long_name; const char *short_name; } dali_dev_type8[] = {
    {0xE0, "Set Temp X-Y Coordinate", "Set X-Y"},
    {0xE2, "Activate Colour Set point", "Activate SetPoint"},
    {0xE7, "Set Colour Temperature Tc", "DTRs->ColTemp"},
    {0xF9, "Query Features", "QryFeats"},
    {0xFA, "Query Current Setpoint Colour", "GetSetPoint"},
};

static const char *dali_lookup_extended_long(uint8_t code)
{
    for (int i = 0; i < (int)(sizeof(dali_extended_cmds) / sizeof(dali_extended_cmds[0])); i++)
        if (dali_extended_cmds[i].code == code)
            return dali_extended_cmds[i].long_name;
    return "Unknown";
}

static const char *dali_lookup_extended_short(uint8_t code)
{
    for (int i = 0; i < (int)(sizeof(dali_extended_cmds) / sizeof(dali_extended_cmds[0])); i++)
        if (dali_extended_cmds[i].code == code)
            return dali_extended_cmds[i].short_name;
    return "Unk";
}

static const char *dali_lookup_cmd_long(uint8_t code)
{
    for (int i = 0; i < (int)(sizeof(dali_cmds) / sizeof(dali_cmds[0])); i++)
        if (dali_cmds[i].code == code)
            return dali_cmds[i].long_name;
    return "Unknown";
}

static const char *dali_lookup_cmd_short(uint8_t code)
{
    for (int i = 0; i < (int)(sizeof(dali_cmds) / sizeof(dali_cmds[0])); i++)
        if (dali_cmds[i].code == code)
            return dali_cmds[i].short_name;
    return "Unk";
}

static const char *dali_lookup_type8_long(uint8_t code)
{
    for (int i = 0; i < (int)(sizeof(dali_dev_type8) / sizeof(dali_dev_type8[0])); i++)
        if (dali_dev_type8[i].code == code)
            return dali_dev_type8[i].long_name;
    return "Unknown App";
}

static const char *dali_lookup_type8_short(uint8_t code)
{
    for (int i = 0; i < (int)(sizeof(dali_dev_type8) / sizeof(dali_dev_type8[0])); i++)
        if (dali_dev_type8[i].code == code)
            return dali_dev_type8[i].short_name;
    return "Unk";
}

static void dali_putb(struct srd_decoder_inst *di, struct dali_priv *s,
                       int bit1, int bit2, int ann_class, const char *txt1,
                       const char *txt2, const char *txt3, const char *txt4,
                       const char *txt5)
{
    uint64_t ss = s->ss_es_bits[bit1][0];
    uint64_t es = s->ss_es_bits[bit2][1];
    c_put(di, ss, es, s->out_ann, ann_class, txt1, txt2, txt3, txt4, txt5);
}

static void dali_handle_bits(struct srd_decoder_inst *di, struct dali_priv *s, int length)
{
    int i;
    uint8_t f = 0, c = 0;
    int b[MAX_BITS];

    for (i = 0; i < length; i++)
        b[i] = s->bits_value[i];

    for (i = 0; i < length; i++) {
        uint64_t ss;
        if (i == 0)
            ss = s->bits_samplenum[0] > 0 ? s->bits_samplenum[0] : 0;
        else
            ss = s->ss_es_bits[i - 1][1];
        uint64_t es = s->bits_samplenum[i] + s->halfbit * 2;
        s->ss_es_bits[i][0] = ss;
        s->ss_es_bits[i][1] = es;

        char bit_str[16];
        snprintf(bit_str, sizeof(bit_str), "%d", b[i]);
        c_put(di, ss, es, s->out_ann, ANN_BIT, bit_str);
    }
    s->num_ss_es_bits = length;

    {
        char st_long[32], st_mid[16], st_short[8], st_tiny[4], st_min[4];
        snprintf(st_long, sizeof(st_long), "Startbit: %d", b[0]);
        snprintf(st_mid, sizeof(st_mid), "ST: %d", b[0]);
        snprintf(st_short, sizeof(st_short), "ST");
        snprintf(st_tiny, sizeof(st_tiny), "S");
        snprintf(st_min, sizeof(st_min), "S");
        dali_putb(di, s, 0, 0, ANN_STARTBIT, st_long, st_mid, st_short, st_tiny, st_min);
        dali_putb(di, s, 0, 0, ANN_RAW, st_long, st_mid, st_short, st_tiny, st_min);
    }

    for (i = 0; i < 8; i++)
        f |= (b[1 + i] << (7 - i));

    if (length == 9) {
        char r_long[32], r_mid[24], r_short[16], r_tiny[8], r_min[4];
        snprintf(r_long, sizeof(r_long), "Reply: %02X", f);
        snprintf(r_mid, sizeof(r_mid), "Rply: %02X", f);
        snprintf(r_short, sizeof(r_short), "Rep: %02X", f);
        snprintf(r_tiny, sizeof(r_tiny), "R: %02X", f);
        snprintf(r_min, sizeof(r_min), "R");
        dali_putb(di, s, 1, 8, ANN_RAW, r_long, r_mid, r_short, r_tiny, r_min);

        char rd_long[32], rd_mid[24], rd_short[16], rd_tiny[8], rd_min[4];
        snprintf(rd_long, sizeof(rd_long), "Reply: %d", f);
        snprintf(rd_mid, sizeof(rd_mid), "Rply: %d", f);
        snprintf(rd_short, sizeof(rd_short), "Rep: %d", f);
        snprintf(rd_tiny, sizeof(rd_tiny), "R: %d", f);
        snprintf(rd_min, sizeof(rd_min), "R");
        dali_putb(di, s, 1, 8, ANN_REPLY, rd_long, rd_mid, rd_short, rd_tiny, rd_min);
        return;
    }

    for (i = 0; i < 8; i++)
        c |= (b[9 + i] << (7 - i));

    {
        char raw_long[32], raw_mid[24], raw_short[16], raw_tiny[8], raw_min[4];
        snprintf(raw_long, sizeof(raw_long), "Raw data: %02X", f);
        snprintf(raw_mid, sizeof(raw_mid), "Raw: %02X", f);
        snprintf(raw_short, sizeof(raw_short), "Raw: %02X", f);
        snprintf(raw_tiny, sizeof(raw_tiny), "R: %02X", f);
        snprintf(raw_min, sizeof(raw_min), "R");
        dali_putb(di, s, 1, 8, ANN_RAW, raw_long, raw_mid, raw_short, raw_tiny, raw_min);
    }

    {
        char raw_long[32], raw_mid[24], raw_short[16], raw_tiny[8], raw_min[4];
        snprintf(raw_long, sizeof(raw_long), "Raw data: %02X", c);
        snprintf(raw_mid, sizeof(raw_mid), "Raw: %02X", c);
        snprintf(raw_short, sizeof(raw_short), "Raw: %02X", c);
        snprintf(raw_tiny, sizeof(raw_tiny), "R: %02X", c);
        snprintf(raw_min, sizeof(raw_min), "R");
        dali_putb(di, s, 9, 16, ANN_RAW, raw_long, raw_mid, raw_short, raw_tiny, raw_min);
    }

    if (b[8] == 1) {
        dali_putb(di, s, 8, 8, ANN_STARTBIT, "Command", "Comd", "COM", "CO", "C");
    } else {
        dali_putb(di, s, 8, 8, ANN_STARTBIT, "Arc Power Level", "Arc Pwr", "ARC", "AC", "A");
    }

    if (f >= 254) {
        dali_putb(di, s, 1, 7, ANN_COMMAND, "BROADCAST", "Brdcast", "BC", "B", "B");
    } else if (f >= 160) {
        if (f == 0xC1)
            s->dev_type = -1;
        const char *xc_long_name = dali_lookup_extended_long(f);
        const char *xc_short_name = dali_lookup_extended_short(f);
        char xc_long[64], xc_mid[48], xc_short[24], xc_tiny[16], xc_min[4];
        snprintf(xc_long, sizeof(xc_long), "Extended Command: %02X (%s)", f, xc_long_name);
        snprintf(xc_mid, sizeof(xc_mid), "XC: %02X (%s)", f, xc_short_name);
        snprintf(xc_short, sizeof(xc_short), "XC: %02X", f);
        snprintf(xc_tiny, sizeof(xc_tiny), "X: %02X", f);
        snprintf(xc_min, sizeof(xc_min), "X");
        dali_putb(di, s, 1, 8, ANN_COMMAND, xc_long, xc_mid, xc_short, xc_tiny, xc_min);
    } else if (f >= 128) {
        {
            char yb_long[24], yb_mid[16], yb_short[8], yb_tiny[4], yb_min[4];
            snprintf(yb_long, sizeof(yb_long), "YBit: %d", b[1]);
            snprintf(yb_mid, sizeof(yb_mid), "YB: %d", b[1]);
            snprintf(yb_short, sizeof(yb_short), "YB");
            snprintf(yb_tiny, sizeof(yb_tiny), "Y");
            snprintf(yb_min, sizeof(yb_min), "Y");
            dali_putb(di, s, 1, 1, ANN_YBIT, yb_long, yb_mid, yb_short, yb_tiny, yb_min);
        }
        {
            int g = (f & 127) >> 1;
            char ga_long[32], ga_mid[24], ga_short[16], ga_tiny[8], ga_min[4];
            snprintf(ga_long, sizeof(ga_long), "Group address: %d", g);
            snprintf(ga_mid, sizeof(ga_mid), "Group: %d", g);
            snprintf(ga_short, sizeof(ga_short), "GP: %d", g);
            snprintf(ga_tiny, sizeof(ga_tiny), "G: %d", g);
            snprintf(ga_min, sizeof(ga_min), "G");
            dali_putb(di, s, 2, 7, ANN_ADDRESS, ga_long, ga_mid, ga_short, ga_tiny, ga_min);
        }
    } else {
        {
            char yb_long[24], yb_mid[16], yb_short[8], yb_tiny[4], yb_min[4];
            snprintf(yb_long, sizeof(yb_long), "YBit: %d", b[1]);
            snprintf(yb_mid, sizeof(yb_mid), "YB: %d", b[1]);
            snprintf(yb_short, sizeof(yb_short), "YB");
            snprintf(yb_tiny, sizeof(yb_tiny), "Y");
            snprintf(yb_min, sizeof(yb_min), "Y");
            dali_putb(di, s, 1, 1, ANN_YBIT, yb_long, yb_mid, yb_short, yb_tiny, yb_min);
        }
        {
            int a = f >> 1;
            char sa_long[32], sa_mid[24], sa_short[16], sa_tiny[8], sa_min[4];
            snprintf(sa_long, sizeof(sa_long), "Short address: %d", a);
            snprintf(sa_mid, sizeof(sa_mid), "Addr: %d", a);
            snprintf(sa_short, sizeof(sa_short), "Addr: %d", a);
            snprintf(sa_tiny, sizeof(sa_tiny), "A: %d", a);
            snprintf(sa_min, sizeof(sa_min), "A");
            dali_putb(di, s, 2, 7, ANN_ADDRESS, sa_long, sa_mid, sa_short, sa_tiny, sa_min);
        }
    }

    if (f >= 160 && f < 254) {
        if (s->dev_type == -1) {
            s->dev_type = c;
            char t_long[24], t_mid[16], t_short[16], t_tiny[8], t_min[4];
            snprintf(t_long, sizeof(t_long), "Type: %d", c);
            snprintf(t_mid, sizeof(t_mid), "Typ: %d", c);
            snprintf(t_short, sizeof(t_short), "Typ: %d", c);
            snprintf(t_tiny, sizeof(t_tiny), "T: %d", c);
            snprintf(t_min, sizeof(t_min), "D");
            dali_putb(di, s, 9, 16, ANN_COMMAND, t_long, t_mid, t_short, t_tiny, t_min);
        } else {
            s->dev_type = 0;
            char d_long[24], d_mid[16], d_short[16], d_tiny[8], d_min[4];
            snprintf(d_long, sizeof(d_long), "Data: %d", c);
            snprintf(d_mid, sizeof(d_mid), "Dat: %d", c);
            snprintf(d_short, sizeof(d_short), "Dat: %d", c);
            snprintf(d_tiny, sizeof(d_tiny), "D: %d", c);
            snprintf(d_min, sizeof(d_min), "D");
            dali_putb(di, s, 9, 16, ANN_COMMAND, d_long, d_mid, d_short, d_tiny, d_min);
        }
    } else if (b[8] == 1) {
        int un = c & 0xF0;
        int ln = c & 0x0F;
        const char *cmd_lname, *cmd_sname;
        char scene_long[48], scene_short[32];
        if (un == 0x10) {
            snprintf(scene_long, sizeof(scene_long), "Recall Scene %d", ln);
            snprintf(scene_short, sizeof(scene_short), "SC %d", ln);
            cmd_lname = scene_long; cmd_sname = scene_short;
        } else if (un == 0x40) {
            snprintf(scene_long, sizeof(scene_long), "Store DTR as Scene %d", ln);
            snprintf(scene_short, sizeof(scene_short), "SC %d = DTR", ln);
            cmd_lname = scene_long; cmd_sname = scene_short;
        } else if (un == 0x50) {
            snprintf(scene_long, sizeof(scene_long), "Delete Scene %d", ln);
            snprintf(scene_short, sizeof(scene_short), "DEL SC %d", ln);
            cmd_lname = scene_long; cmd_sname = scene_short;
        } else if (un == 0x60) {
            snprintf(scene_long, sizeof(scene_long), "Add to Group %d", ln);
            snprintf(scene_short, sizeof(scene_short), "Grp %d Add", ln);
            cmd_lname = scene_long; cmd_sname = scene_short;
        } else if (un == 0x70) {
            snprintf(scene_long, sizeof(scene_long), "Remove from Group %d", ln);
            snprintf(scene_short, sizeof(scene_short), "Grp %d Del", ln);
            cmd_lname = scene_long; cmd_sname = scene_short;
        } else if (un == 0xB0) {
            snprintf(scene_long, sizeof(scene_long), "Query Scene %d Level", ln);
            snprintf(scene_short, sizeof(scene_short), "Sc %d Level", ln);
            cmd_lname = scene_long; cmd_sname = scene_short;
        } else if (c >= 224) {
            if (s->dev_type == 8) {
                cmd_lname = dali_lookup_type8_long(c);
                cmd_sname = dali_lookup_type8_short(c);
            } else {
                snprintf(scene_long, sizeof(scene_long), "Application Specific Command %d", c);
                snprintf(scene_short, sizeof(scene_short), "App Cmd %d", c);
                cmd_lname = scene_long; cmd_sname = scene_short;
            }
        } else {
            cmd_lname = dali_lookup_cmd_long(c);
            cmd_sname = dali_lookup_cmd_short(c);
        }
        char cmd_long[64], cmd_mid[48], cmd_short[24], cmd_tiny[16], cmd_min[4];
        snprintf(cmd_long, sizeof(cmd_long), "Command: %d (%s)", c, cmd_lname);
        snprintf(cmd_mid, sizeof(cmd_mid), "Com: %d (%s)", c, cmd_sname);
        snprintf(cmd_short, sizeof(cmd_short), "Com: %d", c);
        snprintf(cmd_tiny, sizeof(cmd_tiny), "C: %d", c);
        snprintf(cmd_min, sizeof(cmd_min), "C");
        dali_putb(di, s, 9, 16, ANN_COMMAND, cmd_long, cmd_mid, cmd_short, cmd_tiny, cmd_min);
    } else {
        char arc_long[32], arc_mid[24], arc_short[16], arc_tiny[8], arc_min[4];
        snprintf(arc_long, sizeof(arc_long), "Arc Power Level: %d", c);
        snprintf(arc_mid, sizeof(arc_mid), "Level: %d", c);
        snprintf(arc_short, sizeof(arc_short), "Lev: %d", c);
        snprintf(arc_tiny, sizeof(arc_tiny), "L: %d", c);
        snprintf(arc_min, sizeof(arc_min), "L");
        dali_putb(di, s, 9, 16, ANN_COMMAND, arc_long, arc_mid, arc_short, arc_tiny, arc_min);
    }
}

static void dali_reset_decoder_state(struct dali_priv *s)
{
    s->num_edges = 0;
    s->num_bits = 0;
    s->num_ss_es_bits = 0;
    s->state = STATE_IDLE;
}

static void dali_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(struct dali_priv)));
    }
    struct dali_priv *s = (struct dali_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct dali_priv));
    s->state = STATE_IDLE;
    s->dev_type = 0;
}

static void dali_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    struct dali_priv *s = (struct dali_priv *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        if (s->samplerate > 0)
            s->halfbit = (uint64_t)((s->samplerate * 0.0008333) / 2.0);
    }
}

static void dali_start(struct srd_decoder_inst *di)
{
    struct dali_priv *s = (struct dali_priv *)c_decoder_get_private(di);

    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "dali");

    const char *polarity = c_opt_str(di, "polarity", "active-low");
    if (polarity && strcmp(polarity, "active-high") == 0) {
        s->old_dali = 0;
        s->polarity_invert = 1;
    } else {
        s->old_dali = 1;
        s->polarity_invert = 0;
    }

    s->samplerate = c_samplerate(di);
    if (s->samplerate > 0)
        s->halfbit = (uint64_t)((s->samplerate * 0.0008333) / 2.0);
}

static void dali_decode(struct srd_decoder_inst *di)
{
    struct dali_priv *s = (struct dali_priv *)c_decoder_get_private(di);
    if (s->samplerate == 0) {
        s->samplerate = c_samplerate(di);
        if (s->samplerate > 0)
            s->halfbit = (uint64_t)((s->samplerate * 0.0008333) / 2.0);
    }
    if (s->samplerate == 0)
        return;

    while (1) {
        /* Wait for edge OR midpoint timeout.
         * Python processes every sample to detect virtual edges at midpoints.
         * We use CW_E + CW_SKIP to reach midpoints efficiently. */
        int ret;
        if (s->num_edges > 0) {
            uint64_t midpoint = s->edges[s->num_edges - 1] + (uint64_t)(s->halfbit * 1.5);
            uint64_t cur = di_samplenum(di);
            uint64_t skip = (midpoint > cur) ? (midpoint - cur) : 1;
            ret = c_wait(di, CW_E(DALI_CH), CW_OR, CW_SKIP(skip), CW_END);
        } else {
            ret = c_wait(di, CW_E(DALI_CH), CW_END);
        }
        if (ret != SRD_OK)
            return;

        int dali = c_pin(di, DALI_CH);

        if (s->polarity_invert)
            dali ^= 1;

        if (s->state == STATE_IDLE) {
            if (s->old_dali == dali)
                continue;
            if (s->num_edges < MAX_EDGES)
                s->edges[s->num_edges++] = di_samplenum(di);
            s->state = STATE_PHASE0;
            s->old_dali = dali;
            continue;
        }

        if (s->old_dali != dali) {
            if (s->num_edges < MAX_EDGES)
                s->edges[s->num_edges++] = di_samplenum(di);
        } else if (di_samplenum(di) == (s->edges[s->num_edges - 1] + (uint64_t)(s->halfbit * 1.5))) {
            if (s->num_edges < MAX_EDGES)
                s->edges[s->num_edges++] = di_samplenum(di) - (uint64_t)(s->halfbit * 0.5);
        } else {
            continue;
        }

        int bit = s->old_dali;

        if (s->state == STATE_PHASE0) {
            s->phase0 = bit;
            s->state = STATE_PHASE1;
        } else if (s->state == STATE_PHASE1) {
            if (bit == 1 && s->phase0 == 1) {
                if (s->num_bits == 17 || s->num_bits == 9)
                    dali_handle_bits(di, s, s->num_bits);
                dali_reset_decoder_state(s);
                s->old_dali = dali;
                continue;
            } else {
                if (s->num_bits < MAX_BITS) {
                    s->bits_samplenum[s->num_bits] = s->edges[s->num_edges - 3];
                    s->bits_value[s->num_bits] = bit;
                    s->num_bits++;
                }
                s->state = STATE_PHASE0;
            }
        }

        s->old_dali = dali;
    }
}

static void dali_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder dali_c_decoder = {
    .id = "dali_c",
    .name = "DALI(C)",
    .longname = "Digital Addressable Lighting Interface (C)",
    .desc = "DALI protocol decoder (C implementation)",
    .license = "gplv2+",
    .channels = dali_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = dali_options_arr,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = dali_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = dali_ann_rows,
    .inputs = dali_inputs,
    .num_inputs = 1,
    .outputs = dali_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = dali_tags,
    .num_tags = 2,
    .metadata = dali_metadata,
    .reset = dali_reset,
    .start = dali_start,
    .decode = dali_decode,
    .destroy = dali_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    GVariant *vals[] = {
        g_variant_new_string("active-low"),
        g_variant_new_string("active-high"),
    };
    GSList *val_list = NULL;
    val_list = g_slist_append(val_list, vals[0]);
    val_list = g_slist_append(val_list, vals[1]);
    dali_options_arr[0].def = g_variant_new_string("active-low");
    dali_options_arr[0].values = val_list;
    return &dali_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}