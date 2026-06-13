#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_CTRL_TX = 0,
    ANN_ACK_TX,
    ANN_CTRL_RX,
    ANN_ACK_RX,
    NUM_ANN,
};

#define SSI32_MAX_BYTES 128

typedef struct {
    int out_ann;
    int msgsize;

    uint8_t mosi_bytes[SSI32_MAX_BYTES];
    uint8_t miso_bytes[SSI32_MAX_BYTES];
    uint64_t es_array[SSI32_MAX_BYTES];
    int num_bytes;
    uint64_t ss_cmd;
} ssi32_state;

static const char *ssi32_inputs[] = {"spi", NULL};
static const char *ssi32_tags[] = {"Embedded/industrial", NULL};

static struct srd_decoder_option ssi32_options[] = {
    {"msgsize", "dec_ssi32_opt_msgsize", "Message size", NULL, NULL},
};

static const char *ssi32_ann_labels[][3] = {
    {"", "ctrl-tx", "CTRL TX"},
    {"", "ack-tx", "ACK TX"},
    {"", "ctrl-rx", "CTRL RX"},
    {"", "ack-rx", "ACK RX"},
};

static const int ssi32_row_tx_classes[] = {ANN_CTRL_TX, ANN_ACK_TX, -1};
static const int ssi32_row_rx_classes[] = {ANN_CTRL_RX, ANN_ACK_RX, -1};

static const struct srd_c_ann_row ssi32_ann_rows[] = {
    {"tx", "TX", ssi32_row_tx_classes, 2},
    {"rx", "RX", ssi32_row_rx_classes, 2},
};

static void ssi32_handle_ack(struct srd_decoder_inst *di, ssi32_state *s)
{
    char buf[64];
    uint64_t es = s->es_array[0];

    snprintf(buf, sizeof(buf), "> ACK:0x%02x", s->mosi_bytes[0]);
    c_put(di, s->ss_cmd, es, s->out_ann, ANN_ACK_TX, buf);

    snprintf(buf, sizeof(buf), "< ACK:0x%02x", s->miso_bytes[0]);
    c_put(di, s->ss_cmd, es, s->out_ann, ANN_ACK_RX, buf);
}

static void ssi32_handle_ctrl(struct srd_decoder_inst *di, ssi32_state *s)
{
    int tx_size = s->mosi_bytes[2];
    int rx_size = s->miso_bytes[2];
    char buf[512];
    int pos;

    /* TX CTRL */
    pos = snprintf(buf, sizeof(buf), "> CTRL:0x%02x, LUN:0x%02x, SIZE:0x%02x, CRC:0x%02x",
                   s->mosi_bytes[0], s->mosi_bytes[1], s->mosi_bytes[2], s->mosi_bytes[3]);
    if (tx_size > 0 && s->num_bytes > 4) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, ", DATA:0x");
        for (int i = 4; i < tx_size + 4 && i < s->num_bytes; i++)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%02x", s->mosi_bytes[i]);
    }
    {
        int idx = (tx_size + 3 < s->num_bytes) ? tx_size + 3 : s->num_bytes - 1;
        uint64_t tx_es = s->es_array[idx];
        c_put(di, s->ss_cmd, tx_es, s->out_ann, ANN_CTRL_TX, buf);
    }

    /* RX CTRL */
    pos = snprintf(buf, sizeof(buf), "< CTRL:0x%02x, LUN:0x%02x, SIZE:0x%02x, CRC:0x%02x",
                   s->miso_bytes[0], s->miso_bytes[1], s->miso_bytes[2], s->miso_bytes[3]);
    if (rx_size > 0 && s->num_bytes > 4) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, ", DATA:0x");
        for (int i = 4; i < rx_size + 4 && i < s->num_bytes; i++)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%02x", s->miso_bytes[i]);
    }
    {
        int idx = (rx_size + 3 < s->num_bytes) ? rx_size + 3 : s->num_bytes - 1;
        uint64_t rx_es = s->es_array[idx];
        c_put(di, s->ss_cmd, rx_es, s->out_ann, ANN_CTRL_RX, buf);
    }
}

static void ssi32_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    ssi32_state *s = (ssi32_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "CS-CHANGE") == 0) {
        /* CS change resets data */
        s->num_bytes = 0;
        return;
    }

    if (strcmp(cmd, "DATA") != 0 || n_fields < 17)
        return;

    
    
    uint64_t mosi_val = 0, miso_val = 0;
    for (int i = 0; i < 8; i++)
        mosi_val |= ((uint64_t)fields[1 + i].u8 << (8 * i));
    for (int i = 0; i < 8; i++)
        miso_val |= ((uint64_t)fields[9 + i].u8 << (8 * i));

    if (s->num_bytes == 0)
        s->ss_cmd = start_sample;

    if (s->num_bytes < SSI32_MAX_BYTES) {
        s->mosi_bytes[s->num_bytes] = (uint8_t)mosi_val;
        s->miso_bytes[s->num_bytes] = (uint8_t)miso_val;
        s->es_array[s->num_bytes] = end_sample;
        s->num_bytes++;
    }

    /* Determine frame type from first MOSI byte bit7 */
    if (s->mosi_bytes[0] & 0x80) {
        /* ACK frame: 4 bytes */
        if (s->num_bytes < 4)
            return;
        ssi32_handle_ack(di, s);
        s->num_bytes = 0;
    } else {
        /* CTRL frame: msgsize bytes */
        if (s->num_bytes < s->msgsize)
            return;
        ssi32_handle_ctrl(di, s);
        s->num_bytes = 0;
    }
}

static void ssi32_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(ssi32_state)));
    }
    ssi32_state *s = (ssi32_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ssi32_state));
    s->msgsize = 64;
}

static void ssi32_start(struct srd_decoder_inst *di)
{
    ssi32_state *s = (ssi32_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ssi32");

    s->msgsize = (int)c_opt_int(di, "msgsize", 64);
}

static void ssi32_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void ssi32_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ssi32_c_decoder = {
    .id = "ssi32_c",
    .name = "SSI32(C)",
    .longname = "Synchronous Serial Interface (32bit) (C)",
    .desc = "Synchronous Serial Interface (32bit) protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = ssi32_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = ssi32_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = ssi32_ann_rows,
    .inputs = ssi32_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = ssi32_tags,
    .num_tags = 1,
    .reset = ssi32_reset,
    .start = ssi32_start,
    .decode = ssi32_decode,
    .destroy = ssi32_destroy,
    .decode_upper = ssi32_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    ssi32_options[0].def = g_variant_new_int64(64);
    GSList *msgsize_vals = NULL;
    msgsize_vals = g_slist_append(msgsize_vals, g_variant_new_int64(64));
    ssi32_options[0].values = msgsize_vals;

    return &ssi32_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}