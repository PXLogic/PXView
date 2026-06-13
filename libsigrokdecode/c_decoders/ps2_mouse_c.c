#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_MOVEMENT = 0,
    NUM_ANN,
};

enum {
    BINARY_BYTES = 0,
    BINARY_MOVEMENT,
};

#define PS2MOUSE_MAX_PACKETS 16

typedef struct {
    uint8_t val;
    int is_host;
} ps2mouse_packet_entry;

typedef struct {
    int out_ann;
    int out_binary;
    ps2mouse_packet_entry packets[PS2MOUSE_MAX_PACKETS];
    int num_packets;
    uint64_t ss;
    uint64_t es;
} ps2mouse_state;

static const char *ps2mouse_inputs[] = {"ps2", NULL};
static const char *ps2mouse_outputs[] = {NULL};
static const char *ps2mouse_tags[] = {"PC", NULL};

static const char *ps2mouse_ann_labels[][3] = {
    {"", "Movement", "Mouse movement packets"},
};

static const int ps2mouse_row_mov_classes[] = {ANN_MOVEMENT, -1};
static const struct srd_c_ann_row ps2mouse_ann_rows[] = {
    {"mov", "Mouse Movement", ps2mouse_row_mov_classes, 1},
};

static const struct srd_decoder_binary ps2mouse_binary[] = {
    {0, "bytes", "Bytes without explanation"},
    {1, "movement", "Explanation of mouse movement"},
};

static void ps2mouse_mouse_movement(struct srd_decoder_inst *di, ps2mouse_state *s)
{
    if (s->num_packets < 3) return;
    if (s->packets[0].is_host) return;

    uint8_t flags = s->packets[0].val;
    int x = s->packets[1].val;
    int y = s->packets[2].val;

    char msg[128];
    int pos = 0;

    if (flags & 1) pos += snprintf(msg + pos, sizeof(msg) - pos, "L");
    if (flags & 2) pos += snprintf(msg + pos, sizeof(msg) - pos, "M");
    if (flags & 4) pos += snprintf(msg + pos, sizeof(msg) - pos, "R");

    if (flags & 0x10) x -= 256;
    if (flags & 0x20) y -= 256;

    if (x != 0) pos += snprintf(msg + pos, sizeof(msg) - pos, " X%+d", x);
    if (flags & 0x40) pos += snprintf(msg + pos, sizeof(msg) - pos, "!!");
    if (y != 0) pos += snprintf(msg + pos, sizeof(msg) - pos, " Y%+d", y);
    if (flags & 0x80) pos += snprintf(msg + pos, sizeof(msg) - pos, "!!");

    if (pos == 0) snprintf(msg, sizeof(msg), "No Movement");

    c_put(di, s->ss, s->es, s->out_ann, ANN_MOVEMENT, msg);

    /* Binary movement output */
    char bin_msg[256];
    int bpos = snprintf(bin_msg, sizeof(bin_msg), "\n%s", msg);
    c_put_bin(di, s->ss, s->es, s->out_binary,
                         BINARY_MOVEMENT, bpos, (const uint8_t *)bin_msg);
}

static void ps2mouse_print_packets(struct srd_decoder_inst *di, ps2mouse_state *s)
{
    ps2mouse_mouse_movement(di, s);

    /* Binary bytes output */
    const char *tag = s->packets[s->num_packets - 1].is_host ? "Host: " : "Mouse:";
    char octets[128];
    int opos = 0;
    for (int i = 0; i < s->num_packets && opos < 100; i++) {
        opos += snprintf(octets + opos, sizeof(octets) - opos, "%s%02X",
                         (i > 0) ? " " : "", s->packets[i].val);
    }
    char bin_out[256];
    int bpos = snprintf(bin_out, sizeof(bin_out), "\n%s %s", tag, octets);
    c_put_bin(di, s->ss, s->es, s->out_binary,
                         BINARY_BYTES, bpos, (const uint8_t *)bin_out);

    /* Reset */
    s->num_packets = 0;
}

static void ps2mouse_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    ps2mouse_state *s = (ps2mouse_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "BYTE") != 0) return;
    if (!fields || n_fields < 4) return;

    uint8_t val = fields[0].u8;
    int is_host = fields[1].u8;

    if (s->num_packets == 0) {
        s->ss = start_sample;
    } else if (is_host != s->packets[s->num_packets - 1].is_host) {
        ps2mouse_print_packets(di, s);
        s->ss = start_sample;
        /* Special handling for ACK byte */
        if (val == 0xFA && !is_host) {
            char ack_bin[] = "\n ACK";
            c_put_bin(di, start_sample, end_sample, s->out_binary,
                                 BINARY_BYTES, sizeof(ack_bin) - 1, (const uint8_t *)ack_bin);
            s->num_packets = 0;
            return;
        }
    }

    if (s->num_packets < PS2MOUSE_MAX_PACKETS) {
        s->packets[s->num_packets].val = val;
        s->packets[s->num_packets].is_host = is_host;
        s->num_packets++;
    }
    s->es = end_sample;

    if (s->num_packets > 2) {
        ps2mouse_print_packets(di, s);
    }
}

static void ps2mouse_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(ps2mouse_state)));
    }
    ps2mouse_state *s = (ps2mouse_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ps2mouse_state));
}

static void ps2mouse_start(struct srd_decoder_inst *di)
{
    ps2mouse_state *s = (ps2mouse_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ps2_mouse");
    s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "bytes");
}

static void ps2mouse_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void ps2mouse_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ps2_mouse_c_decoder = {
    .id = "ps2_mouse_c",
    .name = "PS/2 Mouse(C)",
    .longname = "PS/2 Mouse (C)",
    .desc = "PS/2 mouse interface. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = ps2mouse_ann_labels,
    .num_annotation_rows = 1,
    .annotation_rows = ps2mouse_ann_rows,
    .inputs = ps2mouse_inputs,
    .num_inputs = 1,
    .outputs = ps2mouse_outputs,
    .num_outputs = 0,
    .binary = ps2mouse_binary,
    .num_binary = 2,
    .tags = ps2mouse_tags,
    .num_tags = 1,
    .reset = ps2mouse_reset,
    .start = ps2mouse_start,
    .decode = ps2mouse_decode,
    .destroy = ps2mouse_destroy,
    .decode_upper = ps2mouse_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &ps2_mouse_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}