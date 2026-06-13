#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum lfast_ann {
    ANN_BIT = 0,
    ANN_SYNC,
    ANN_HEADER_PL_SIZE,
    ANN_HEADER_CH_TYPE,
    ANN_HEADER_CTS,
    ANN_PAYLOAD,
    ANN_CTRL_DATA,
    ANN_SLEEP,
    ANN_WARNING,
    NUM_ANN,
};

enum {
    STATE_SYNC = 0,
    STATE_HEADER,
    STATE_PAYLOAD,
    STATE_SLEEPBIT,
};

struct lfast_priv {
    int state;
    
    uint64_t ss_bit, es_bit;
    uint64_t ss_sync, ss_header, ss_byte;
    uint64_t ss_payload, es_payload;
    uint64_t bit_len;
    uint64_t prev_bit_len;
    uint64_t timeout;
    uint8_t bits[64];
    int bit_count;
    int payload_size;
    int ch_type_id;
    uint8_t payload_bytes[64];
    int payload_byte_count;
    int out_ann;
    int out_proto;
    uint64_t ss;
    uint64_t es;
};

static const char *payload_sizes[] = {
    "8 bit", "32 bit / 4 byte", "64 bit / 8 byte",
    "96 bit / 12 byte", "128 bit / 16 byte", "256 bit / 32 byte",
    "512 bit / 64 byte", "288 bit / 36 byte"
};
static const int payload_byte_sizes[] = {1, 4, 8, 12, 16, 32, 64, 36};

static const char *channel_types[] = {
    "Interface Control / PING", "Unsolicited Status (32 bit)",
    "Slave Interface Control / Read", "CTS Transfer",
    "Data Channel A", "Data Channel B", "Data Channel C", "Data Channel D",
    "Data Channel E", "Data Channel F", "Data Channel G", "Data Channel H",
    "Reserved", "Reserved", "Reserved", "Reserved"
};

static struct srd_channel lfast_channels[] = {
    {"data", "Data", "TXP or RXP", 0, SRD_CHANNEL_SDATA, "dec_lfast_chan_data"},
};

static const char *lfast_ann_labels[][3] = {
    {"", "bit", "Bits"},
    {"", "sync", "Sync Pattern"},
    {"", "header_pl_size", "Payload Size"},
    {"", "header_ch_type", "Logical Channel Type"},
    {"", "header_cts", "Clear To Send"},
    {"", "payload", "Payload"},
    {"", "ctrl_data", "Control Data"},
    {"", "sleep", "Sleep Bit"},
    {"", "warning", "Warning"},
};

static const int lfast_row_bits_classes[] = {ANN_BIT, -1};
static const int lfast_row_fields_classes[] = {ANN_SYNC, ANN_HEADER_PL_SIZE, ANN_HEADER_CH_TYPE, ANN_HEADER_CTS, ANN_PAYLOAD, ANN_CTRL_DATA, ANN_SLEEP, -1};
static const int lfast_row_warnings_classes[] = {ANN_WARNING, -1};

static const struct srd_c_ann_row lfast_ann_rows[] = {
    {"bits", "Bits", lfast_row_bits_classes, 1},
    {"fields", "Fields", lfast_row_fields_classes, 7},
    {"warnings", "Warnings", lfast_row_warnings_classes, 1},
};

static const char *lfast_inputs[] = {"logic"};
static const char *lfast_outputs[] = {"lfast"};
static const char *lfast_tags[] = {"Embedded/industrial"};

static uint32_t bitpack(uint8_t *bits, int count)
{
    uint32_t val = 0;
    for (int i = 0; i < count && i < 32; i++)
        val = (val << 1) | (bits[i] & 1);
    return val;
}

static void reset_state(struct lfast_priv *s)
{
    s->state = STATE_SYNC;
    s->bit_count = 0;
    s->payload_size = 0;
    s->ch_type_id = 0;
    s->payload_byte_count = 0;
    s->timeout = 0;
    s->prev_bit_len = s->bit_len;
    s->bit_len = 0;
    s->ss = 0;
    s->es = 0;
    s->ss_payload = 0;
    s->es_payload = 0;
    s->ss_byte = 0;
    memset(s->bits, 0, sizeof(s->bits));
    memset(s->payload_bytes, 0, sizeof(s->payload_bytes));
}

static void lfast_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(struct lfast_priv)));
    struct lfast_priv *s = (struct lfast_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct lfast_priv));
    s->out_ann = -1;
    s->out_proto = -1;
}

static void lfast_start(struct srd_decoder_inst *di)
{
    struct lfast_priv *s = (struct lfast_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "lfast");
    s->out_proto = c_reg_out(di, SRD_OUTPUT_PROTO, "lfast");
}

static const char *get_control_name(uint8_t value)
{
    switch (value) {
        case 0x00: return "PING";
        case 0x01: return "Reserved";
        case 0x02: return "Slave interface clock multiplier start";
        case 0x04: return "Slave interface clock multiplier stop";
        case 0x08: return "Use 5 MBaud for M->S";
        case 0x10: return "Use 320 MBaud for M->S";
        case 0x20: return "Use 5 MBaud for S->M";
        case 0x40: return "Use 20 MBaud for S->M (needs 20 MHz SysClk)";
        case 0x80: return "Use 320 MBaud for S->M";
        case 0x31: return "Enable slave interface transmitter";
        case 0x32: return "Disable slave interface transmitter";
        case 0x34: return "Enable clock test mode";
        case 0x38: return "Disable clock test mode and payload loopback";
        case 0xFF: return "Enable payload loopback";
        default: return NULL;
    }
}

static void lfast_decode(struct srd_decoder_inst *di)
{
    struct lfast_priv *s = (struct lfast_priv *)c_decoder_get_private(di);
    uint64_t samplerate = c_samplerate(di);
    if (!samplerate)
        return;

    while (1) {
        int ret;
        if (s->timeout > 0)
            ret = c_wait(di, CW_E(0), CW_OR, CW_SKIP(s->timeout), CW_END);
        else
            ret = c_wait(di, CW_E(0), CW_END);
        if (ret != SRD_OK)
            return;

        int is_timeout = (s->timeout > 0) &&
                         (di_matched(di) & (1ULL << 1)) && !(di_matched(di) & (1ULL << 0));

        /* Handle timeout conditions */
        if (is_timeout) {
            if (s->state == STATE_SYNC) {
                reset_state(s);
                continue;
            } else if (s->state == STATE_SLEEPBIT) {
                s->ss_bit += s->bit_len;
                s->es_bit = s->ss_bit + s->bit_len;
                /* Timeout = no edge = sleep bit is 0 (no sleep request) */
                c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_SLEEP,
                          "No LVDS sleep mode request", "No sleep", "N");
                reset_state(s);
                continue;
            }
            /* For other states, fall through to process the timeout interval
               as bits, then reset at the end */
        }

        s->es = di_samplenum(di);

        /* If this is the first edge, we only update ss */
        if (s->ss == 0) {
            s->ss = s->es;
            s->timeout = (uint64_t)(16.2 * s->prev_bit_len);
            continue;
        }

        /* Shouldn't happen but we check just in case */
        if (s->es - s->ss == 0)
            continue;

        /* We use the first bit to deduce the bit length */
        if (s->bit_len == 0)
            s->bit_len = s->es - s->ss;

        /* Determine number of bits covered by this edge */
        uint64_t edge_delta = s->es - s->ss;
        int bit_count = (int)((double)edge_delta / (double)s->bit_len + 0.5);

        if (bit_count == 0) {
            c_put(di, s->ss, s->es, s->out_ann, ANN_WARNING, "Bit time too short");
            reset_state(s);
            s->ss = s->es;
            continue;
        }

        /* Bit value: rising edge (val=1) means level was 0 before -> bit 0.
           Falling edge (val=0) means level was 1 before -> bit 1.
           This matches the Python decoder: rising=0, falling=1. */
        int val = c_pin(di, 0);
        int prev_val = 1 - val;

        double divided_len = (double)edge_delta / (double)bit_count;

        for (int i = 0; i < bit_count; i++) {
            int bval = prev_val;

            /* Append bit at end (bits[0]=oldest=MSB after bitpack) */
            if (s->bit_count < 64)
                s->bits[s->bit_count++] = bval;

            /* Calculate bit sample range */
            s->ss_bit = (uint64_t)(s->ss + (double)i * divided_len);
            s->es_bit = (uint64_t)(s->ss_bit + divided_len);

            /* Output bit annotation */
            char bit_str[16];
            snprintf(bit_str, sizeof(bit_str), "%d", bval);
            c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_BIT, bit_str);

            /* Process based on state - one bit at a time like Python */
            int did_reset = 0;

            if (s->state == STATE_SYNC) {
                if (s->bit_count == 1)
                    s->ss_sync = s->ss_bit;

                if (s->bit_count == 16) {
                    uint32_t sync_val = bitpack(s->bits, 16);
                    if (sync_val == 0xA84B) {
                        c_put(di, s->ss_sync, s->es_bit, s->out_ann, ANN_SYNC, "Sync OK");
                        s->bit_count = 0;
                        memset(s->bits, 0, sizeof(s->bits));
                        s->state = STATE_HEADER;
                        s->timeout = (uint64_t)(9.4 * s->bit_len);
                    } else {
                        char warn_str[64];
                        snprintf(warn_str, sizeof(warn_str), "Wrong Sync Value: %04X", sync_val);
                        c_put(di, s->ss_sync, s->es_bit, s->out_ann, ANN_WARNING, warn_str);
                        reset_state(s);
                        did_reset = 1;
                    }
                } else {
                    s->timeout = (uint64_t)(16.2 * s->bit_len);
                }
            } else if (s->state == STATE_HEADER) {
                if (s->bit_count == 1)
                    s->ss_header = s->ss_bit;

                if (s->bit_count == 8) {
                    uint32_t header_val = bitpack(s->bits, 8);
                    int pl_size_id = (header_val >> 5) & 0x07;
                    s->ch_type_id = (header_val >> 1) & 0x0F;
                    int cts = header_val & 0x01;

                    if (pl_size_id < 0 || pl_size_id > 7)
                        pl_size_id = 0;
                    s->payload_size = payload_byte_sizes[pl_size_id];

                    /* Match Python's annotation layout */
                    uint64_t bit_len_hdr = (s->es_bit - s->ss_header) / 8;
                    uint64_t ss_f, es_f;

                    /* Payload size: bits 7-5 (3 bits) */
                    ss_f = s->ss_header;
                    es_f = ss_f + 3 * bit_len_hdr;
                    c_put(di, ss_f, es_f, s->out_ann, ANN_HEADER_PL_SIZE, payload_sizes[pl_size_id]);

                    /* Channel type: bits 4-1 (4 bits) */
                    ss_f = es_f;
                    es_f = ss_f + 4 * bit_len_hdr;
                    c_put(di, ss_f, es_f, s->out_ann, ANN_HEADER_CH_TYPE, channel_types[s->ch_type_id]);

                    /* CTS: bit 0 (1 bit) */
                    ss_f = es_f;
                    es_f = ss_f + bit_len_hdr;
                    char cts_str[8];
                    snprintf(cts_str, sizeof(cts_str), "%d", cts);
                    c_put(di, ss_f, es_f, s->out_ann, ANN_HEADER_CTS, cts_str);

                    s->bit_count = 0;
                    memset(s->bits, 0, sizeof(s->bits));
                    s->state = STATE_PAYLOAD;
                    s->timeout = (uint64_t)(9.4 * s->bit_len);
                }
            } else if (s->state == STATE_PAYLOAD) {
                s->timeout = (uint64_t)((s->payload_size - s->payload_byte_count) * 8 * s->bit_len);

                if (s->bit_count == 1) {
                    s->ss_byte = s->ss_bit;
                    if (s->ss_payload == 0)
                        s->ss_payload = s->ss_bit;
                }

                if (s->bit_count == 8) {
                    uint32_t value = bitpack(s->bits, 8);
                    char hex_str[8];
                    snprintf(hex_str, sizeof(hex_str), "%02X", value);

                    int is_data_channel = (s->ch_type_id >= 4 && s->ch_type_id <= 11);

                    if (is_data_channel) {
                        c_put(di, s->ss_byte, s->es_bit, s->out_ann, ANN_PAYLOAD, hex_str);
                    } else {
                        if (s->payload_byte_count == 0) {
                            const char *ctrl_name = get_control_name((uint8_t)value);
                            if (ctrl_name)
                                c_put(di, s->ss_byte, s->es_bit, s->out_ann, ANN_CTRL_DATA, ctrl_name);
                            else
                                c_put(di, s->ss_byte, s->es_bit, s->out_ann, ANN_CTRL_DATA, hex_str);
                        } else {
                            c_put(di, s->ss_byte, s->es_bit, s->out_ann, ANN_CTRL_DATA, hex_str);
                        }
                    }

                    s->payload_bytes[s->payload_byte_count++] = (uint8_t)value;
                    s->bit_count = 0;
                    memset(s->bits, 0, sizeof(s->bits));
                    s->es_payload = s->es_bit;

                    if (s->payload_byte_count == s->payload_size) {
                        /* Output protocol data for data channels */
                        if (is_data_channel) {
                            c_proto(di, s->ss_payload, s->es_payload, s->out_proto,
                                                "DATA", C_BYTES(s->payload_bytes, s->payload_byte_count), C_END);
                        }
                        s->timeout = (uint64_t)(1.4 * s->bit_len);
                        s->state = STATE_SLEEPBIT;
                    }
                }
            } else if (s->state == STATE_SLEEPBIT) {
                if (s->bit_count == 0) {
                    c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_SLEEP,
                              "No LVDS sleep mode request", "No sleep", "N");
                } else if (s->bit_count > 1) {
                    char warn_str[64];
                    snprintf(warn_str, sizeof(warn_str),
                             "Expected only the sleep bit, got %d bits instead", s->bit_count);
                    c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_WARNING, warn_str);
                } else {
                    /* bit_count == 1 */
                    if (s->bits[0] == 1) {
                        c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_SLEEP,
                                  "LVDS sleep mode request", "Sleep", "Y");
                    } else {
                        c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_SLEEP,
                                  "No LVDS sleep mode request", "No sleep", "N");
                    }
                }
                reset_state(s);
                did_reset = 1;
            }

            if (did_reset)
                break;
        }

        /* Only update ss if we didn't just perform a reset */
        if (s->ss > 0)
            s->ss = di_samplenum(di);

        /* If we got here when a timeout occurred, we have processed all null
           bits that we could and should reset now to find the next packet */
        if (is_timeout)
            reset_state(s);
    }
}

static void lfast_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

static struct srd_c_decoder lfast_c_decoder = {
    .id = "lfast_c",
    .name = "LFAST(C)",
    .longname = "NXP LFAST interface (C)",
    .desc = "Differential high-speed P2P interface (C implementation)",
    .license = "gplv2+",
    .channels = lfast_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = lfast_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = lfast_ann_rows,
    .inputs = lfast_inputs,
    .num_inputs = 1,
    .outputs = lfast_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = lfast_tags,
    .num_tags = 1,
    .reset = lfast_reset,
    .start = lfast_start,
    .decode = lfast_decode,
    .destroy = lfast_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &lfast_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}