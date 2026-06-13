#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_SIG_TYPE = 0,
    ANN_SIG_ERROR,
    ANN_SIG_DATA,
    ANN_PKT_TYPE,
    ANN_PKT_ERROR,
    ANN_PKT_DATA,
    NUM_ANN,
};

#define LTTO_MAX_MULTIBYTE 16

typedef struct {
    int out_ann;
    int out_python;
    /* Multibyte state */
    int in_multibyte;
    uint64_t multibyte_start_ss;
    uint64_t multibyte_end_es;
    int multibyte_data_count;
    uint64_t multibyte_data_ss[LTTO_MAX_MULTIBYTE];
    uint64_t multibyte_data_es[LTTO_MAX_MULTIBYTE];
    int multibyte_data_val[LTTO_MAX_MULTIBYTE];
} ltto_decode_state;

/* Packet type lookup table */
typedef struct {
    int code;
    const char *name;
} ptype_entry;

static const ptype_entry ptype_table[] = {
    {0x00, "GAME START"},
    {0x01, "JOIN CONFIRMED"},
    {0x02, "HOSTING CUSTOM"},
    {0x03, "HOSTING 2-TEAMS"},
    {0x04, "HOSTING 3-TEAMS"},
    {0x05, "HOSTING HIDE-AND-SEEK"},
    {0x06, "HOSTING HUNTER-HUNTED"},
    {0x07, "HOSTING 2-KINGS"},
    {0x08, "HOSTING 3-KINGS"},
    {0x09, "HOSTING OWN-THE-ZONE"},
    {0x0A, "HOSTING 2-TEAM OWN-THE-ZONE"},
    {0x0B, "HOSTING 3-TEAM OWN-THE-ZONE"},
    {0x0C, "HOSTING HOOK GAME"},
    {0x0D, "RESERVED(0x0D)"},
    {0x0E, "RESERVED(0x0E)"},
    {0x0F, "CHANNEL FAILURE"},
    {0x10, "REQUEST TO JOIN"},
    {0x11, "CHANNEL RELEASE"},
    {0x12, "RESERVED(0x12)"},
    {0x20, "MEDIC REQUEST"},
    {0x21, "MEDIC ASSIST"},
    {0x22, "MEDIC RELEASE"},
    {0x30, "RESERVED(0x30)"},
    {0x31, "DEBRIEF DATA NEEDED"},
    {0x32, "RANKINGS"},
    {0x33, "NAME-DATA"},
    {0x40, "BASIC DEBRIEF DATA"},
    {0x41, "GROUP 1 DEBRIEF DATA"},
    {0x42, "GROUP 2 DEBRIEF DATA"},
    {0x43, "GROUP 3 DEBRIEF DATA"},
    {0x44, "RESERVED(0x44)"},
    {0x48, "HEAD-TO-HEAD SCORE DATA"},
    {0x49, "RESERVED(0x49)"},
    {0x4A, "RESERVED(0x4A)"},
    {0x4B, "RESERVED(0x4B)"},
    {0x50, "BASIC DE-CLONING DATA"},
    {0x51, "GROUP 1 DE-CLONING DATA"},
    {0x52, "GROUP 2 DE-CLONING DATA"},
    {0x53, "GROUP 3 DE-CLONING DATA"},
    {0x54, "DE-CLONING REQUEST"},
    {0x80, "TEXT MESSAGE"},
    {0x81, "LTAR GAME"},
    {0x82, "LTAR RTJ"},
    {0x83, "LTAR PLAYER"},
    {0x84, "LTAR ACCEPT"},
    {0x85, "LTAR NAME"},
    {0x86, "LTAR WHODAT"},
    {0x87, "LTAR RELEASE"},
    {0x88, "LTAR START COUNTDOWN"},
    {0x8F, "LTAR ABORT"},
    {0x90, "LTAR SPECIAL-ATTACK"},
    {-1, NULL},
};

static const char *healthtext[] = {
    "0%", "<25%", "<50%", ">50%"
};

static const char *ltto_find_ptype(int code)
{
    for (int i = 0; ptype_table[i].name != NULL; i++) {
        if (ptype_table[i].code == code)
            return ptype_table[i].name;
    }
    return NULL;
}

static const char *ltto_decode_inputs[] = {"ir_ltto", NULL};
static const char *ltto_decode_outputs[] = {"ir_ltto_decode", NULL};
static const char *ltto_decode_tags[] = {"Embedded/industrial", NULL};

static const char *ltto_decode_ann_labels[][3] = {
    {"", "Signature Type", "Signature Type"},
    {"", "Error", "Error"},
    {"", "Signature Data", "Signature Data"},
    {"", "Packet Type", "Packet Type"},
    {"", "Packet Error", "Packet Error"},
    {"", "Packet Data", "Packet Data"},
};

static const int ltto_decode_row_sig_types_classes[] = {ANN_SIG_TYPE, ANN_SIG_ERROR, -1};
static const int ltto_decode_row_sig_datas_classes[] = {ANN_SIG_DATA, -1};
static const int ltto_decode_row_pkt_types_classes[] = {ANN_PKT_TYPE, ANN_PKT_ERROR, -1};
static const int ltto_decode_row_pkt_datas_classes[] = {ANN_PKT_DATA, -1};

static const struct srd_c_ann_row ltto_decode_ann_rows[] = {
    {"signature-types", "Signature type", ltto_decode_row_sig_types_classes, 2},
    {"signature-datas", "Signature data", ltto_decode_row_sig_datas_classes, 1},
    {"packet-types", "Packet type", ltto_decode_row_pkt_types_classes, 2},
    {"packet-datas", "Packet data", ltto_decode_row_pkt_datas_classes, 1},
};

static void ltto_put_tag_signature(struct srd_decoder_inst *di, ltto_decode_state *s,
                                    uint64_t ss, uint64_t es, int bitdata)
{
    int team = -1;
    switch (bitdata & 0x60) {
    case 0x00: team = 0; break;
    case 0x20: team = 1; break;
    case 0x40: team = 2; break;
    case 0x60: team = 3; break;
    }

    int player = -1;
    switch (bitdata & 0x1B) {
    case 0x00: player = 0; break;
    case 0x04: player = 1; break;
    case 0x08: player = 2; break;
    case 0x0B: player = 3; break;
    case 0x10: player = 4; break;
    case 0x14: player = 5; break;
    case 0x18: player = 6; break;
    case 0x1B: player = 7; break;
    }

    int megatag = bitdata & 0x03;

    c_put(di, ss, es, s->out_ann, ANN_SIG_TYPE, "Tag");

    if (team == 0) {
        char buf[256];
        const char *tag_type;
        switch (player) {
        case 0: tag_type = "LTAG(SOLO)"; break;
        case 1: tag_type = "TTAG(Team 1)"; break;
        case 2: tag_type = "TTAG(Team 2)"; break;
        case 3: tag_type = "TTAG(Team 3)"; break;
        case 4: tag_type = "Neutral Base"; break;
        case 5: tag_type = "Team 1 Base"; break;
        case 6: tag_type = "Team 2 Base"; break;
        case 7: tag_type = "Team 3 Base"; break;
        default: tag_type = "Unknown"; break;
        }
        snprintf(buf, sizeof(buf), "%s, Megatag: %d", tag_type, megatag);
        c_put(di, ss, es, s->out_ann, ANN_SIG_DATA, buf);
    } else {
        char buf[256];
        snprintf(buf, sizeof(buf), "Team: %d, Player: %d, Megatag: %d", team, player, megatag);
        c_put(di, ss, es, s->out_ann, ANN_SIG_DATA, buf);
    }
}

static void ltto_put_multibyte_start(struct srd_decoder_inst *di, ltto_decode_state *s,
                                      uint64_t ss, uint64_t es, int bitdata)
{
    c_put(di, ss, es, s->out_ann, ANN_SIG_TYPE, "Multibyte Packet Type");

    s->multibyte_start_ss = ss;
    s->multibyte_data_count = 0;
    s->multibyte_data_ss[0] = ss;
    s->multibyte_data_es[0] = es;
    s->multibyte_data_val[0] = bitdata;
    s->multibyte_data_count = 1;

    const char *pname = ltto_find_ptype(bitdata & 0xFF);
    if (pname) {
        c_put(di, ss, es, s->out_ann, ANN_SIG_DATA, pname);
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "Unknown(0x%02X)", bitdata & 0xFF);
        c_put(di, ss, es, s->out_ann, ANN_SIG_DATA, buf);
    }
}

static void ltto_put_multibyte_data(struct srd_decoder_inst *di, ltto_decode_state *s,
                                     uint64_t ss, uint64_t es, int bitdata)
{
    int idx = s->multibyte_data_count - 2;
    char type_buf[64];
    snprintf(type_buf, sizeof(type_buf), "Multibyte Packet Data %d", idx);
    c_put(di, ss, es, s->out_ann, ANN_SIG_TYPE, type_buf);

    char buf[64];
    snprintf(buf, sizeof(buf), "0x%02X", bitdata & 0xFF);
    c_put(di, ss, es, s->out_ann, ANN_SIG_DATA, buf);

    if (s->multibyte_data_count < LTTO_MAX_MULTIBYTE) {
        s->multibyte_data_ss[s->multibyte_data_count] = ss;
        s->multibyte_data_es[s->multibyte_data_count] = es;
        s->multibyte_data_val[s->multibyte_data_count] = bitdata;
        s->multibyte_data_count++;
    }
}

static void ltto_put_multibyte_end(struct srd_decoder_inst *di, ltto_decode_state *s,
                                    uint64_t ss, uint64_t es, int bitdata)
{
    c_put(di, ss, es, s->out_ann, ANN_SIG_TYPE, "Multibyte Packet Checksum");

    char buf[64];
    snprintf(buf, sizeof(buf), "0x%02X", bitdata & 0xFF);
    c_put(di, ss, es, s->out_ann, ANN_SIG_DATA, buf);

    s->multibyte_end_es = es;

    /* Put the complete multibyte packet */
    c_put(di, s->multibyte_start_ss, s->multibyte_end_es,
              s->out_ann, ANN_PKT_TYPE, "Multibyte Packet");

    s->multibyte_data_count = 0;
}

static void ltto_put_ltto_beacon(struct srd_decoder_inst *di, ltto_decode_state *s,
                                  uint64_t ss, uint64_t es, int bitdata)
{
    int team = -1;
    switch (bitdata & 0x18) {
    case 0x00: team = 0; break;
    case 0x08: team = 1; break;
    case 0x10: team = 2; break;
    case 0x18: team = 3; break;
    }

    int hitflag = (bitdata & 0x04) ? 1 : 0;
    int extra = bitdata & 0x03;

    if (hitflag == 0 && extra != 0) {
        /* Special Team 0 beacon - Area Beacon */
        c_put(di, ss, es, s->out_ann, ANN_SIG_TYPE, "Area Beacon");
        switch (extra) {
        case 1:
            c_put(di, ss, es, s->out_ann, ANN_SIG_DATA, "Mine Tag");
            break;
        case 2:
            c_put(di, ss, es, s->out_ann, ANN_SIG_DATA, "Zone");
            break;
        case 3:
            switch (team) {
            case 0: c_put(di, ss, es, s->out_ann, ANN_SIG_DATA, "Neutral Base"); break;
            case 1: c_put(di, ss, es, s->out_ann, ANN_SIG_DATA, "Team 1 Base"); break;
            case 2: c_put(di, ss, es, s->out_ann, ANN_SIG_DATA, "Team 2 Base"); break;
            case 3: c_put(di, ss, es, s->out_ann, ANN_SIG_DATA, "Team 3 Base"); break;
            }
            break;
        }
    } else {
        /* Normal beacon */
        c_put(di, ss, es, s->out_ann, ANN_SIG_TYPE, "LTTO Beacon");
        char buf[256];
        snprintf(buf, sizeof(buf), "Team: %d, Just hit: %d, Extra damage: %d",
                 team, hitflag, extra);
        c_put(di, ss, es, s->out_ann, ANN_SIG_DATA, buf);
    }
}

static void ltto_put_ltar_beacon(struct srd_decoder_inst *di, ltto_decode_state *s,
                                  uint64_t ss, uint64_t es, int bitdata)
{
    int hitflag = (bitdata & 0x100) ? 1 : 0;
    int shields = (bitdata & 0x80) ? 1 : 0;
    int health = -1;
    switch (bitdata & 0x60) {
    case 0x00: health = 0; break;
    case 0x20: health = 1; break;
    case 0x40: health = 2; break;
    case 0x60: health = 3; break;
    }

    int team = -1;
    switch (bitdata & 0x18) {
    case 0x00: team = 0; break;
    case 0x08: team = 1; break;
    case 0x10: team = 2; break;
    case 0x18: team = 3; break;
    }

    int player = bitdata & 0x07;

    c_put(di, ss, es, s->out_ann, ANN_SIG_TYPE, "LTAR Beacon");

    char buf[256];
    const char *ht = (health >= 0 && health <= 3) ? healthtext[health] : "?";
    snprintf(buf, sizeof(buf), "Just hit: %d, Shields up: %d, Rough Health: %s, Team: %d, Player: %d",
             hitflag, shields, ht, team, player);
    c_put(di, ss, es, s->out_ann, ANN_SIG_DATA, buf);
}

static void ltto_decode_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    ltto_decode_state *s = (ltto_decode_state *)c_decoder_get_private(di);
    if (!s)
        return;

    int is_short = (strcmp(cmd, "SHORT") == 0);
    int is_long = (strcmp(cmd, "LONG") == 0);

    if (!is_short && !is_long)
        return;

    /* data format: bitcount(1 byte) + bitdata(2 bytes LE) */
    if (n_fields < 3)
        return;

    int bitcount = fields[0].u8;
    int bitdata = (int)fields[1].u8 | ((int)fields[2].u8 << 8);

    if (is_short) {
        if (bitcount == 7) {
            /* Tag */
            ltto_put_tag_signature(di, s, start_sample, end_sample, bitdata);
        } else if (bitcount == 9) {
            if (!(bitdata & 0x100)) {
                /* Multibyte start */
                ltto_put_multibyte_start(di, s, start_sample, end_sample, bitdata);
            } else {
                /* Multibyte end */
                ltto_put_multibyte_end(di, s, start_sample, end_sample, bitdata);
            }
        } else if (bitcount == 8) {
            /* Multibyte data */
            ltto_put_multibyte_data(di, s, start_sample, end_sample, bitdata);
        }
    } else if (is_long) {
        if (bitcount == 5) {
            /* LTTO Beacon */
            ltto_put_ltto_beacon(di, s, start_sample, end_sample, bitdata);
        } else if (bitcount == 9) {
            /* LTAR Beacon */
            ltto_put_ltar_beacon(di, s, start_sample, end_sample, bitdata);
        }
    }
}

static void ltto_decode_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(ltto_decode_state)));
    }
    ltto_decode_state *s = (ltto_decode_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ltto_decode_state));
}

static void ltto_decode_start(struct srd_decoder_inst *di)
{
    ltto_decode_state *s = (ltto_decode_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ir_ltto_decode");
}

static void ltto_decode_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void ltto_decode_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ir_ltto_decode_c_decoder = {
    .id = "ir_ltto_decode_c",
    .name = "IR LTTO Decode(C)",
    .longname = "LTTO laser tag IR Decode (C)",
    .desc = "A decoder for the LTTO laser tag IR protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = ltto_decode_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = ltto_decode_ann_rows,
    .inputs = ltto_decode_inputs,
    .num_inputs = 1,
    .outputs = ltto_decode_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = ltto_decode_tags,
    .num_tags = 1,
    .reset = ltto_decode_reset,
    .start = ltto_decode_start,
    .decode = ltto_decode_decode,
    .destroy = ltto_decode_destroy,
    .decode_upper = ltto_decode_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &ir_ltto_decode_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}