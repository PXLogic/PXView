#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glib.h>
#include "libsigrokdecode.h"

#define SWI_MAX_WORDS 256
#define SWI_MAX_PACKETS 256
#define SWI_MAX_UID_DATA 64
#define SWI_BAUD_TIME 4.47e-6

enum swi_ann {
    ANN_BAUD_RATE = 0,
    ANN_BITS,
    ANN_BYTES,
    ANN_ERR,
    ANN_MEAN,
    ANN_PBYTES,
    ANN_NMBR,
    NUM_ANN,
};

enum swi_word_type {
    WORD_TYPE_UNICAST = 0,
    WORD_TYPE_BROADCAST = 1,
};

struct swi_word {
    uint64_t startN;
    uint64_t endN;
    int type_int;
    int data_int;
    char bit_string[16];
    int inverted;
};

struct swi_packet {
    uint64_t startN;
    uint64_t endN;
    int recieve;
    int first_two_bytes;
    int last_byte;
    int packetClass;
    int recieve2;
};

typedef struct {
    uint64_t samplerate;
    int strt;
    int halfRate;

    uint64_t pastNs[1024];
    int pastVs[1024];
    int log_count;

    struct swi_word pastWords[SWI_MAX_WORDS];
    int word_count;
    int lastHdrIdx;
    int packetClass;
    int recieveData;

    struct swi_packet pastPackets[SWI_MAX_PACKETS];
    int packet_count;
    int readPacketSeq;
    int polling;
    int readOdcNumber;

    char pastBits[256];
    int pastBits_len;
    struct { uint64_t startN; int data; } pastUidData[SWI_MAX_UID_DATA];
    int uidData_count;
    uint64_t startUidByte;
    int bitsIdx;
    int enumIdx;

    int out_ann;
} swi_state;

static struct srd_channel swi_channels[] = {
    {"swi", "SWI", "SWI channel", 0, SRD_CHANNEL_SDATA, NULL},
};

static const char *swi_ann_labels[][3] = {
    {"", "baud_rate", "Bauds"},
    {"", "bits", "Bits"},
    {"", "bytes", "Words"},
    {"", "err", "Errors"},
    {"", "mean", "Means"},
    {"", "pbytes", "Byte"},
    {"", "nmbr", "Number"},
};

static const int row_bauds_classes[] = {ANN_BAUD_RATE, -1};
static const int row_bits_classes[] = {ANN_BITS, -1};
static const int row_data_classes[] = {ANN_BYTES, -1};
static const int row_errors_classes[] = {ANN_ERR, -1};
static const int row_meanings_classes[] = {ANN_MEAN, -1};
static const int row_meanings_data_classes[] = {ANN_PBYTES, -1};
static const int row_numbs_classes[] = {ANN_NMBR, -1};

static const struct srd_c_ann_row swi_ann_rows[] = {
    {"bauds", "Timing", row_bauds_classes, 1},
    {"bits_a", "Bits", row_bits_classes, 1},
    {"data", "Words", row_data_classes, 1},
    {"errors", "Errors", row_errors_classes, 1},
    {"meanings", "Meaning", row_meanings_classes, 1},
    {"meanings_data", "Data", row_meanings_data_classes, 1},
    {"numbs", "Numbers", row_numbs_classes, 1},
};

static const char *swi_inputs[] = {"logic"};
static const char *swi_tags[] = {"Clock/timing", "Util"};

static int swi_calculate_bauds(uint64_t sampleN, uint64_t prevSampleN,
                                uint64_t samplerate, int *halfRate)
{
    double t = (double)(sampleN - prevSampleN) / (double)samplerate;
    int bauds = (int)round(t / SWI_BAUD_TIME);
    if (bauds % 2 == 0 && bauds > 0) {
        bauds /= 2;
        if (*halfRate < 1 && bauds == 1) {
            *halfRate = 1;
        }
    }
    return bauds;
}

static int swi_calculate_bit(int baud, int invert)
{
    return ((baud == 3 && !invert) || (baud == 1 && invert)) ? 1 : 0;
}

static void swi_save_log(swi_state *s, uint64_t samplenum, int pin_val)
{
    if (s->log_count < 1024) {
        s->pastNs[s->log_count] = samplenum;
        s->pastVs[s->log_count] = pin_val;
        s->log_count++;
    }
}

static void swi_put_word_ann(struct srd_decoder_inst *di, swi_state *s,
                              struct swi_word *w)
{
    char buf[64];
    const char *type_str = (w->type_int == WORD_TYPE_UNICAST) ? "Unicast" : "Broadcast";
    snprintf(buf, sizeof(buf), "%s: %d (0x%03X)", type_str, w->data_int, w->data_int);
    c_put(di, w->startN, w->endN, s->out_ann, ANN_BYTES, buf, type_str, w->bit_string);

    snprintf(buf, sizeof(buf), "0x%03X", w->data_int);
    c_put(di, w->startN, w->endN, s->out_ann, ANN_PBYTES, buf);
}

static void swi_parse_enumerate(struct srd_decoder_inst *di, swi_state *s,
                                 int start_idx)
{
    (void)start_idx;
    /* Enumerate/Select parsing */
    c_put(di, s->pastWords[start_idx].startN, s->pastWords[start_idx + 4].endN,
              s->out_ann, ANN_MEAN, "Enumerate/Select", "Enum/Select", "Enum");

    /* Parse UID from following words */
    s->enumIdx = start_idx;
    s->uidData_count = 0;
    s->pastBits_len = 0;
    s->bitsIdx = 0;
}

static void swi_parse_broadcast(struct srd_decoder_inst *di, swi_state *s,
                                 int start_idx)
{
    (void)start_idx;
    if (s->word_count < start_idx + 5)
        return;

    struct swi_word *w3 = &s->pastWords[start_idx + 3];
    if (w3->data_int == 0) {
        c_put(di, w3->startN, w3->endN, s->out_ann, ANN_MEAN,
                  "Initialize", "Init", "I");
        return;
    }

    struct swi_word *w4 = &s->pastWords[start_idx + 4];
    int w4_bits = w4->data_int;

    /* Check bits 2-7 of word 4 for command type */
    int cmd = (w4_bits >> 3) & 0x3F;
    if (cmd == 0x03) {
        /* Enumerate/Select */
        c_put(di, w4->startN, w4->endN, s->out_ann, ANN_MEAN,
                  "Enumerate/Select", "Enum/Select", "Enum");
        swi_parse_enumerate(di, s, start_idx);
    } else if (cmd == 0x02) {
        /* Packet Header */
        c_put(di, w4->startN, w4->endN, s->out_ann, ANN_MEAN,
                  "Packet Header", "Pkt Header", "PH");
        s->lastHdrIdx = start_idx + 4;
    } else if (cmd == 0x05) {
        /* Packet Class */
        c_put(di, w4->startN, w4->endN, s->out_ann, ANN_MEAN,
                  "Packet Class", "Pkt Class", "PC");
        s->packetClass = w4_bits & 0x03;
    } else {
        int sel_bits = (w4_bits >> 4) & 0x03;
        if (sel_bits == 0x01 || sel_bits == 0x02) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Selected device byte: 0x%03X", w4_bits);
            c_put(di, w4->startN, w4->endN, s->out_ann, ANN_MEAN,
                      buf, "Sel Dev", "SD");
        }
    }
}

static void swi_parse_packet(struct srd_decoder_inst *di, swi_state *s)
{
    int idx = s->word_count - 1;
    if (idx < 0)
        return;

    struct swi_packet *pkt = &s->pastPackets[s->packet_count];
    pkt->packetClass = s->packetClass;

    if (s->packetClass == 0) {
        /* Packet class 0: UID, polling */
        c_put(di, s->pastWords[idx].startN, s->pastWords[idx].endN,
                  s->out_ann, ANN_MEAN, "UID/Polling", "UID", "U");
    } else if (s->packetClass == 1) {
        /* Packet class 1: read, request */
        c_put(di, s->pastWords[idx].startN, s->pastWords[idx].endN,
                  s->out_ann, ANN_MEAN, "Read/Request", "Read", "R");
    }
}

static void swi_parse_unicast(struct srd_decoder_inst *di, swi_state *s,
                                int start_idx)
{
    (void)start_idx;
    if (s->lastHdrIdx < 2)
        return;

    int idx = s->word_count - 1;
    if (idx < 0)
        return;

    int offset = idx - s->lastHdrIdx;
    if (offset == 2) {
        /* First byte after header */
    } else if (offset == 3) {
        /* Second byte */
    } else if (offset == 4) {
        /* Third byte - assemble packet */
        swi_parse_packet(di, s);
    }
}

static void swi_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(swi_state)));
    swi_state *s = (swi_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(swi_state));
    s->out_ann = -1;
}

static void swi_start(struct srd_decoder_inst *di)
{
    swi_state *s = (swi_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "swi");
}

static void swi_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    swi_state *s = (swi_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void swi_decode(struct srd_decoder_inst *di)
{
    swi_state *s = (swi_state *)c_decoder_get_private(di);
    if (!s->samplerate)
        return;

    while (1) {
        int ret = c_wait(di, CW_E(0), CW_END);
        if (ret != SRD_OK)
            return;

        int pin_val = c_pin(di, 0);
        swi_save_log(s, di_samplenum(di), pin_val);

        if (s->log_count < 2)
            continue;

        if (s->strt) {
            c_put(di, s->pastNs[s->log_count - 3], di_samplenum(di), s->out_ann, ANN_BYTES, "[START]");
            s->strt = 0;
        }

        uint64_t prevN = s->pastNs[s->log_count - 2];
        int bauds = swi_calculate_bauds(di_samplenum(di), prevN, s->samplerate, &s->halfRate);

        /* Check for valid data baud interval (1 or 3) */
        if (bauds != 1 && bauds != 3) {
            if (s->pastVs[s->log_count - 2] != 1) {
                if (bauds < 3) {
                    c_put(di, prevN, di_samplenum(di), s->out_ann, ANN_ERR, "Error");
                    c_put(di, prevN, di_samplenum(di), s->out_ann, ANN_BYTES, "[ACK]");
                } else {
                    s->strt = 1;
                }
            }
            continue;
        }

        /* Check if previous gap >= 5 bauds and measuring low pulse (word separator) */
        int have_word_gap = 0;
        if (s->log_count >= 3) {
            uint64_t prev2N = s->pastNs[s->log_count - 3];
            int gap_bauds = swi_calculate_bauds(prevN, prev2N, s->samplerate, &s->halfRate);
            if (gap_bauds >= 5 && s->pastVs[s->log_count - 2] != 1)
                have_word_gap = 1;
        } else {
            have_word_gap = 1; /* First edge, treat as word start */
        }

        if (!have_word_gap) {
            if (s->pastVs[s->log_count - 2] != 1) {
                c_put(di, prevN, di_samplenum(di), s->out_ann, ANN_BYTES, "[ACK]");
            }
            continue;
        }

        /* Collect 13 baud intervals for a word */
        uint64_t data_ns[13];
        int data_bauds[13];
        data_ns[0] = prevN;
        data_bauds[0] = bauds;

        if (bauds == 1)
            c_put(di, prevN, di_samplenum(di), s->out_ann, ANN_BAUD_RATE, "B1");
        else if (bauds == 3)
            c_put(di, prevN, di_samplenum(di), s->out_ann, ANN_BAUD_RATE, "B3");

        int collected = 1;
        int inverted = 0;

        for (int i = 1; i < 13; i++) {
            ret = c_wait(di, CW_E(0), CW_END);
            if (ret != SRD_OK)
                return;

            pin_val = c_pin(di, 0);
            swi_save_log(s, di_samplenum(di), pin_val);

            int b = swi_calculate_bauds(di_samplenum(di), s->pastNs[s->log_count - 2],
                                         s->samplerate, &s->halfRate);

            if (b == 1) {
                c_put(di, s->pastNs[s->log_count - 2], di_samplenum(di),
                          s->out_ann, ANN_BAUD_RATE, "B1");
            } else if (b == 3) {
                c_put(di, s->pastNs[s->log_count - 2], di_samplenum(di),
                          s->out_ann, ANN_BAUD_RATE, "B3");
            } else if (b == 2) {
                /* Round 2 bauds to nearest valid: treat as B3 */
                b = 3;
                c_put(di, s->pastNs[s->log_count - 2], di_samplenum(di),
                          s->out_ann, ANN_BAUD_RATE, "B3");
            } else if (b >= 4) {
                /* Round >=4 bauds to B3 */
                b = 3;
                c_put(di, s->pastNs[s->log_count - 2], di_samplenum(di),
                          s->out_ann, ANN_BAUD_RATE, "B3");
            } else {
                /* b == 0 or negative, abort word collection */
                break;
            }

            data_ns[i] = s->pastNs[s->log_count - 2];
            data_bauds[i] = b;
            collected++;
        }

        if (collected < 13) {
            c_put(di, data_ns[0], di_samplenum(di), s->out_ann, ANN_ERR,
                      "Incomplete word", "Err", "!");
            continue;
        }

        /* 13th baud determines inversion */
        inverted = (data_bauds[12] == 3) ? 1 : 0;

        /* Parse word: 2 training bits + 10 data bits */
        struct swi_word w;
        memset(&w, 0, sizeof(w));
        w.startN = data_ns[0];
        w.endN = data_ns[12];
        w.inverted = inverted;

        int data_val = 0;
        int bit_pos = 0;
        for (int i = 2; i < 12; i++) {
            int bit = swi_calculate_bit(data_bauds[i], inverted);
            data_val |= (bit << (9 - bit_pos));
            w.bit_string[bit_pos] = bit + '0';
            bit_pos++;
        }
        w.bit_string[bit_pos] = '\0';
        w.data_int = data_val;

        /* Determine word type from training bits */
        int t0 = swi_calculate_bit(data_bauds[0], inverted);
        int t1 = swi_calculate_bit(data_bauds[1], inverted);
        if (t0 == 0 && t1 == 1)
            w.type_int = WORD_TYPE_UNICAST;
        else if (t0 == 1 && t1 == 0)
            w.type_int = WORD_TYPE_BROADCAST;
        else
            w.type_int = -1;

        /* Output bit annotations */
        for (int i = 2; i < 12; i++) {
            int bit = swi_calculate_bit(data_bauds[i], inverted);
            char bit_str[16];
            snprintf(bit_str, sizeof(bit_str), "%d", bit);
            c_put(di, data_ns[i], data_ns[i + 1],
                      s->out_ann, ANN_BITS, bit_str);
        }

        /* Store word */
        if (s->word_count < SWI_MAX_WORDS) {
            s->pastWords[s->word_count] = w;
            s->word_count++;
        }

        /* Output word annotation */
        swi_put_word_ann(di, s, &w);

        /* Dispatch based on word type */
        int word_idx = s->word_count - 1;
        if (w.type_int == WORD_TYPE_BROADCAST) {
            swi_parse_broadcast(di, s, word_idx);
        } else if (w.type_int == WORD_TYPE_UNICAST) {
            swi_parse_unicast(di, s, word_idx);
        } else {
            c_put(di, w.startN, w.endN, s->out_ann, ANN_ERR,
                      "Invalid training bits", "Err", "!");
        }
    }
}

static void swi_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder swi_c_decoder = {
    .id = "swi_c",
    .name = "SWI(C)",
    .longname = "Infineon SWI(C)",
    .desc = "Infineon Single Wire Interface protocol (C implementation)",
    .license = "gplv2+",
    .channels = swi_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = swi_ann_labels,
    .num_annotation_rows = 7,
    .annotation_rows = swi_ann_rows,
    .inputs = swi_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = swi_tags,
    .num_tags = 2,
    .reset = swi_reset,
    .start = swi_start,
    .decode = swi_decode,
    .destroy = swi_destroy,
    .state_size = 0,
    .metadata = swi_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &swi_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}