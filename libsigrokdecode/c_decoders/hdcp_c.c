#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_MSG_0x00 = 0,
    ANN_MSG_0x01,
    ANN_MSG_0x02,
    ANN_MSG_0x03,
    ANN_MSG_0x04,
    ANN_MSG_0x05,
    ANN_MSG_0x06,
    ANN_MSG_0x07,
    ANN_MSG_0x08,
    ANN_MSG_0x09,
    ANN_MSG_0x0A,
    ANN_MSG_0x0B,
    ANN_MSG_0x0C,
    ANN_MSG_0x0D,
    ANN_MSG_0x0E,
    ANN_MSG_0x0F,
    ANN_MSG_0x10,
    ANN_MSG_0x11,
    ANN_SUMMARY = 18,
    ANN_WARNING = 19,
    NUM_ANN,
};

enum hdcp_state {
    HDCP_IDLE,
    HDCP_GET_SLAVE_ADDR,
    HDCP_WRITE_OFFSET,
    HDCP_BUFFER_DATA,
};

typedef struct {
    enum hdcp_state state;
    uint8_t stack[256];
    int stack_len;
    char type[64];
    
    uint64_t ss_block, es_block;
    int out_ann;
    uint64_t ss;
    uint64_t es;
} hdcp_state;

/* HDCP 2.2 message ID mapping */
static const struct { int id; const char *name; } hdcp_msg_ids[] = {
    {2,  "AKE_Init"},
    {3,  "AKE_Send_Cert"},
    {4,  "AKE_No_stored_km"},
    {5,  "AKE_Stored_km"},
    {7,  "AKE_Send_H_prime"},
    {8,  "AKE_Send_Pairing_Info"},
    {9,  "LC_Init"},
    {10, "LC_Send_L_prime"},
    {11, "SKE_Send_Eks"},
    {12, "RepeaterAuth_Send_ReceiverID_List"},
    {15, "RepeaterAuth_Send_Ack"},
    {16, "RepeaterAuth_Stream_Manage"},
    {17, "RepeaterAuth_Stream_Ready"},
    {-1, NULL},
};

/* Write offset mapping */
static const struct { int offset; const char *name; } hdcp_write_items[] = {
    {0x00, "1.4 Bksv - Receiver KSV"},
    {0x08, "1.4 Ri' - Link Verification"},
    {0x0a, "1.4 Pj' - Enhanced Link Verification"},
    {0x10, "1.4 Aksv - Transmitter KSV"},
    {0x15, "1.4 Ainfo - Transmitter KSV"},
    {0x18, "1.4 An - Session random number"},
    {0x20, "1.4 V'H0"},
    {0x24, "1.4 V'H1"},
    {0x28, "1.4 V'H2"},
    {0x2c, "1.4 V'H3"},
    {0x30, "1.4 V'H4"},
    {0x40, "1.4 Bcaps"},
    {0x41, "1.4 Bstatus"},
    {0x43, "1.4 KSV FIFO"},
    {0x50, "HDCP2Version"},
    {0x60, "Write_Message"},
    {0x70, "RxStatus"},
    {0x80, "Read_Message"},
    {-1, NULL},
};

static const char *hdcp_lookup_msg_name(int msg_id)
{
    for (int i = 0; hdcp_msg_ids[i].name != NULL; i++) {
        if (hdcp_msg_ids[i].id == msg_id)
            return hdcp_msg_ids[i].name;
    }
    return "Invalid";
}

static const char *hdcp_lookup_write_item(int offset)
{
    for (int i = 0; hdcp_write_items[i].name != NULL; i++) {
        if (hdcp_write_items[i].offset == offset)
            return hdcp_write_items[i].name;
    }
    return NULL;
}

static const char *hdcp_inputs[] = {"i2c", NULL};
static const char *hdcp_outputs[] = {"hdcp", NULL};
static const char *hdcp_tags[] = {"PC", "Security/crypto", NULL};

static const char *hdcp_ann_labels[][3] = {
    {"", "message-0x00", "Message 0x00"},
    {"", "message-0x01", "Message 0x01"},
    {"", "message-0x02", "Message 0x02"},
    {"", "message-0x03", "Message 0x03"},
    {"", "message-0x04", "Message 0x04"},
    {"", "message-0x05", "Message 0x05"},
    {"", "message-0x06", "Message 0x06"},
    {"", "message-0x07", "Message 0x07"},
    {"", "message-0x08", "Message 0x08"},
    {"", "message-0x09", "Message 0x09"},
    {"", "message-0x0a", "Message 0x0A"},
    {"", "message-0x0b", "Message 0x0B"},
    {"", "message-0x0c", "Message 0x0C"},
    {"", "message-0x0d", "Message 0x0D"},
    {"", "message-0x0e", "Message 0x0E"},
    {"", "message-0x0f", "Message 0x0F"},
    {"", "message-0x10", "Message 0x10"},
    {"", "message-0x11", "Message 0x11"},
    {"", "summary", "Summary"},
    {"", "warning", "Warning"},
};

static const int hdcp_row_messages_classes[] = {
    ANN_MSG_0x00, ANN_MSG_0x01, ANN_MSG_0x02, ANN_MSG_0x03,
    ANN_MSG_0x04, ANN_MSG_0x05, ANN_MSG_0x06, ANN_MSG_0x07,
    ANN_MSG_0x08, ANN_MSG_0x09, ANN_MSG_0x0A, ANN_MSG_0x0B,
    ANN_MSG_0x0C, ANN_MSG_0x0D, ANN_MSG_0x0E, ANN_MSG_0x0F,
    ANN_MSG_0x10, ANN_MSG_0x11
};
static const int hdcp_row_summaries_classes[] = {ANN_SUMMARY};
static const int hdcp_row_warnings_classes[] = {ANN_WARNING};
static const struct srd_c_ann_row hdcp_ann_rows[] = {
    {"messages", "Messages", hdcp_row_messages_classes, 18},
    {"summaries", "Summaries", hdcp_row_summaries_classes, 1},
    {"warnings", "Warnings", hdcp_row_warnings_classes, 1},
};

static void hdcp_process_buffer(struct srd_decoder_inst *di, hdcp_state *s)
{
    if (s->type[0] == '\0')
        return;

    if (s->stack_len == 0) {
        c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_SUMMARY, s->type);
        return;
    }

    if (strcmp(s->type, "RxStatus") == 0) {
        int lo = (s->stack_len >= 1) ? s->stack[s->stack_len - 1] : 0;
        int hi = (s->stack_len >= 2) ? s->stack[s->stack_len - 2] : 0;
        int rxstatus = (hi << 8) | lo;
        int reauth_req = (rxstatus & 0x800) != 0;
        int ready = (rxstatus & 0x400) != 0;
        int length = rxstatus & 0x3ff;
        char buf[256];
        snprintf(buf, sizeof(buf), "%s, reauth %s, ready %s, length %s",
                 s->type, reauth_req ? "True" : "False",
                 ready ? "True" : "False",
                 length ? "True" : "False");
        c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_SUMMARY, buf);
    } else if (strcmp(s->type, "1.4 Bstatus") == 0) {
        int lo = (s->stack_len >= 1) ? s->stack[s->stack_len - 1] : 0;
        int hi = (s->stack_len >= 2) ? s->stack[s->stack_len - 2] : 0;
        int bstatus = (hi << 8) | lo;
        int device_count = bstatus & 0x7f;
        int depth = (bstatus & 0x700) >> 8;
        int hdmi_mode = (bstatus & 0x1000) != 0;
        char buf[256];
        snprintf(buf, sizeof(buf), "%s, %d devices, depth %d, hdmi mode %s",
                 s->type, device_count, depth, hdmi_mode ? "True" : "False");
        c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_SUMMARY, buf);
    } else if (strcmp(s->type, "Read_Message") == 0 || strcmp(s->type, "Write_Message") == 0) {
        int msg = s->stack[0];
        const char *msg_name = hdcp_lookup_msg_name(msg);
        int ann_cls = (msg >= 0 && msg < 18) ? msg : ANN_SUMMARY;
        char buf[256];
        snprintf(buf, sizeof(buf), "%s, %s", s->type, msg_name);
        c_put(di, s->ss_block, s->es_block, s->out_ann, ann_cls, buf);
    } else if (strcmp(s->type, "HDCP2Version") == 0) {
        int version = s->stack[0];
        if (version & 0x04) {
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_SUMMARY, "HDCP2");
        } else {
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_SUMMARY, "NOT HDCP2");
        }
    } else {
        c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_SUMMARY, s->type);
    }
}

static void hdcp_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    hdcp_state *s = (hdcp_state *)c_decoder_get_private(di);
    if (!s)
        return;

    s->ss = start_sample;
    s->es = end_sample;

    if (s->state == HDCP_IDLE) {
        if (strcmp(cmd, "START") == 0) {
            s->stack_len = 0;
            s->type[0] = '\0';
            s->ss_block = start_sample;
            s->state = HDCP_GET_SLAVE_ADDR;
        } else if (strcmp(cmd, "START REPEAT") != 0) {
            return;
        }
        s->state = HDCP_GET_SLAVE_ADDR;
    } else if (s->state == HDCP_GET_SLAVE_ADDR) {
        if (strcmp(cmd, "ADDRESS READ") == 0) {
            uint8_t addr = (n_fields > 0) ? fields[0].u8 : 0;
            if (addr != 0x3a) {
                s->state = HDCP_IDLE;
                return;
            }
            s->state = HDCP_BUFFER_DATA;
        } else if (strcmp(cmd, "ADDRESS WRITE") == 0) {
            uint8_t addr = (n_fields > 0) ? fields[0].u8 : 0;
            if (addr != 0x3a) {
                s->state = HDCP_IDLE;
                return;
            }
            s->state = HDCP_WRITE_OFFSET;
        }
    } else if (s->state == HDCP_WRITE_OFFSET) {
        if (strcmp(cmd, "DATA WRITE") == 0) {
            uint8_t databyte = (n_fields > 0) ? fields[0].u8 : 0;
            const char *type_name = hdcp_lookup_write_item(databyte);
            if (type_name)
                strncpy(s->type, type_name, sizeof(s->type) - 1);
            if (databyte == 0x10 || databyte == 0x15 || databyte == 0x18 || databyte == 0x60) {
                s->state = HDCP_BUFFER_DATA;
            } else if (s->type[0] != '\0') {
                s->state = HDCP_IDLE;
            }
        }
    } else if (s->state == HDCP_BUFFER_DATA) {
        if (strcmp(cmd, "STOP") == 0 || strcmp(cmd, "NACK") == 0) {
            s->es_block = end_sample;
            hdcp_process_buffer(di, s);
            s->state = HDCP_IDLE;
        } else if (strcmp(cmd, "DATA READ") == 0 || strcmp(cmd, "DATA WRITE") == 0) {
            uint8_t databyte = (n_fields > 0) ? fields[0].u8 : 0;
            if (s->stack_len < (int)sizeof(s->stack))
                s->stack[s->stack_len++] = databyte;
        }
    }
}

static void hdcp_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(hdcp_state)));
    }
    hdcp_state *s = (hdcp_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(hdcp_state));
    s->state = HDCP_IDLE;
}

static void hdcp_start(struct srd_decoder_inst *di)
{
    hdcp_state *s = (hdcp_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "hdcp");
}

static void hdcp_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void hdcp_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder hdcp_c_decoder = {
    .id = "hdcp_c",
    .name = "HDCP(C)",
    .longname = "HDCP over HDMI (C)",
    .desc = "HDCP protocol over HDMI. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = hdcp_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = hdcp_ann_rows,
    .inputs = hdcp_inputs,
    .num_inputs = 1,
    .outputs = hdcp_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = hdcp_tags,
    .num_tags = 2,
    .reset = hdcp_reset,
    .start = hdcp_start,
    .decode = hdcp_decode,
    .destroy = hdcp_destroy,
    .decode_upper = hdcp_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &hdcp_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}