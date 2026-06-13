#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum cec_state {
    WAIT_START,
    GET_BITS,
    WAIT_EOM,
    WAIT_ACK,
};

enum cec_ann {
    ANN_START = 0,
    ANN_EOM_0,
    ANN_EOM_1,
    ANN_NACK,
    ANN_ACK,
    ANN_BITS,
    ANN_BYTES,
    ANN_FRAMES,
    ANN_SECTIONS,
    ANN_WARN,
    NUM_ANN,
};

enum cec_pulse {
    PULSE_INVALID,
    PULSE_START,
    PULSE_ZERO,
    PULSE_ONE,
};

#define CEC_CH 0
#define MAX_CMD_BYTES 32

#define START_LOW_MIN 3.5
#define START_LOW_MAX 3.9
#define START_TOTAL_MIN 4.3
#define START_TOTAL_MAX 4.7
#define ZERO_LOW_MIN 1.3
#define ZERO_LOW_MAX 1.7
#define ZERO_TOTAL_MIN 2.05
#define ZERO_TOTAL_MAX 2.75
#define ONE_LOW_MIN 0.4
#define ONE_LOW_MAX 0.8
#define ONE_TOTAL_MIN 2.05
#define ONE_TOTAL_MAX 2.75

typedef struct {
    enum cec_state state;
    uint64_t samplerate;
    uint64_t fall_start;
    uint64_t fall_end;
    uint64_t rise;
    uint64_t max_ack_len_samples;
    int eom;
    int bit_count;
    int byte_count;
    uint8_t byte_val;
    uint64_t byte_start;
    uint64_t frame_start;
    uint64_t frame_end;
    int is_nack;
    struct {
        uint64_t st;
        uint64_t ed;
        uint8_t val;
    } cmd_bytes[MAX_CMD_BYTES];
    int cmd_bytes_count;
    int out_ann;
} cec_priv;

static struct srd_channel cec_channels[] = {
    { "cec", "CEC", "CEC bus data", 0, SRD_CHANNEL_SDATA, "dec_cec_chan_cec" },
};

static const char* cec_ann_labels[][3] = {
    { "", "st", "Start" },
    { "", "eom-0", "End of message" },
    { "", "eom-1", "Message continued" },
    { "", "nack", "ACK not set" },
    { "", "ack", "ACK set" },
    { "", "bits", "Bits" },
    { "", "bytes", "Bytes" },
    { "", "frames", "Frames" },
    { "", "sections", "Sections" },
    { "", "warnings", "Warnings" },
};

static const int cec_row_bits_classes[] = { ANN_START, ANN_EOM_0, ANN_EOM_1, ANN_NACK, ANN_ACK, ANN_BITS };
static const int cec_row_bytes_classes[] = { ANN_BYTES };
static const int cec_row_frames_classes[] = { ANN_FRAMES };
static const int cec_row_sections_classes[] = { ANN_SECTIONS };
static const int cec_row_warnings_classes[] = { ANN_WARN };

static const struct srd_c_ann_row cec_ann_rows[] = {
    { "bits", "Bits", cec_row_bits_classes, 6 },
    { "bytes", "Bytes", cec_row_bytes_classes, 1 },
    { "frames", "Frames", cec_row_frames_classes, 1 },
    { "sections", "Sections", cec_row_sections_classes, 1 },
    { "warnings", "Warnings", cec_row_warnings_classes, 1 },
};

static const char* cec_inputs[] = { "logic", NULL };
static const char* cec_outputs[] = { NULL };
static const char* cec_tags[] = { "Display", "PC", NULL };

static const char* logical_addresses[] = {
    "TV",
    "Recording_1",
    "Recording_2",
    "Tuner_1",
    "Playback_1",
    "AudioSystem",
    "Tuner2",
    "Tuner3",
    "Playback_2",
    "Recording_3",
    "Tuner_4",
    "Playback_3",
    "Backup_1",
    "Backup_2",
    "FreeUse",
};

static const struct {
    uint8_t code;
    const char* name;
} cec_opcodes[] = {
    { 0x82, "ACTIVE_SOURCE" },
    { 0x04, "IMAGE_VIEW_ON" },
    { 0x0D, "TEXT_VIEW_ON" },
    { 0x9D, "INACTIVE_SOURCE" },
    { 0x85, "REQUEST_ACTIVE_SOURCE" },
    { 0x80, "ROUTING_CHANGE" },
    { 0x81, "ROUTING_INFORMATION" },
    { 0x86, "SET_STREAM_PATH" },
    { 0x36, "STANDBY" },
    { 0x0B, "RECORD_OFF" },
    { 0x09, "RECORD_ON" },
    { 0x0A, "RECORD_STATUS" },
    { 0x0F, "RECORD_TV_SCREEN" },
    { 0x33, "CLEAR_ANALOGUE_TIMER" },
    { 0x99, "CLEAR_DIGITAL_TIMER" },
    { 0xA1, "CLEAR_EXTERNAL_TIMER" },
    { 0x34, "SET_ANALOGUE_TIMER" },
    { 0x97, "SET_DIGITAL_TIMER" },
    { 0xA2, "SET_EXTERNAL_TIMER" },
    { 0x67, "SET_TIMER_PROGRAM_TITLE" },
    { 0x43, "TIMER_CLEARED_STATUS" },
    { 0x35, "TIMER_STATUS" },
    { 0x9E, "CEC_VERSION" },
    { 0x9F, "GET_CEC_VERSION" },
    { 0x83, "GIVE_PHYSICAL_ADDRESS" },
    { 0x91, "GET_MENU_LANGUAGE" },
    { 0x84, "REPORT_PHYSICAL_ADDRESS" },
    { 0x32, "SET_MENU_LANGUAGE" },
    { 0x42, "DECK_CONTROL" },
    { 0x1B, "DECK_STATUS" },
    { 0x1A, "GIVE_DECK_STATUS" },
    { 0x41, "PLAY" },
    { 0x08, "GIVE_TUNER_DEVICE_STATUS" },
    { 0x92, "SELECT_ANALOGUE_SERVICE" },
    { 0x93, "SELECT_DIGITAL_SERVICE" },
    { 0x07, "TUNER_DEVICE_STATUS" },
    { 0x06, "TUNER_STEP_DECREMENT" },
    { 0x05, "TUNER_STEP_INCREMENT" },
    { 0x87, "DEVICE_VENDOR_ID" },
    { 0x8C, "GIVE_DEVICE_VENDOR_ID" },
    { 0x89, "VENDOR_COMMAND" },
    { 0xA0, "VENDOR_COMMAND_WITH_ID" },
    { 0x8A, "VENDOR_REMOTE_BUTTON_DOWN" },
    { 0x8B, "VENDOR_REMOTE_BUTTON_UP" },
    { 0x64, "SET_OSD_STRING" },
    { 0x46, "GIVE_OSD_NAME" },
    { 0x47, "SET_OSD_NAME" },
    { 0x8D, "MENU_REQUEST" },
    { 0x8E, "MENU_STATUS" },
    { 0x44, "USER_CONTROL_PRESSED" },
    { 0x45, "USER_CONTROL_RELEASE" },
    { 0x8F, "GIVE_DEVICE_POWER_STATUS" },
    { 0x90, "REPORT_POWER_STATUS" },
    { 0x00, "FEATURE_ABORT" },
    { 0xFF, "ABORT" },
    { 0x71, "GIVE_AUDIO_STATUS" },
    { 0x7D, "GIVE_SYSTEM_AUDIO_MODE_STATUS" },
    { 0x7A, "REPORT_AUDIO_STATUS" },
    { 0x72, "SET_SYSTEM_AUDIO_MODE" },
    { 0x70, "SYSTEM_AUDIO_MODE_REQUEST" },
    { 0x7E, "SYSTEM_AUDIO_MODE_STATUS" },
    { 0x9A, "SET_AUDIO_RATE" },
};

static const char* resolve_logical_address(int id, int is_initiator)
{
    if (id < 0 || id > 0x0F)
        return "Invalid";
    if (id == 0x0F)
        return is_initiator ? "Unregistered" : "Broadcast";
    return logical_addresses[id];
}

static const char* lookup_opcode(uint8_t code)
{
    int i;
    for (i = 0; i < (int)(sizeof(cec_opcodes) / sizeof(cec_opcodes[0])); i++) {
        if (cec_opcodes[i].code == code)
            return cec_opcodes[i].name;
    }
    return "Invalid";
}

static void reset_frame_vars(cec_priv* s)
{
    s->eom = 0;
    s->bit_count = 0;
    s->byte_count = 0;
    s->byte_val = 0;
    s->byte_start = 0;
    s->frame_start = 0;
    s->frame_end = 0;
    s->is_nack = 0;
    s->cmd_bytes_count = 0;
}

static void handle_frame(struct srd_decoder_inst* di, cec_priv* s, int is_nack)
{
    char hex_str[256];
    char sec_str[1024];
    int pos, i, operands;

    pos = 0;
    for (i = 0; i < s->cmd_bytes_count; i++) {
        if (pos > 0)
            hex_str[pos++] = ':';
        pos += snprintf(hex_str + pos, sizeof(hex_str) - pos, "%02x", s->cmd_bytes[i].val);
    }
    hex_str[pos] = '\0';
    c_put(di, s->frame_start, s->frame_end, s->out_ann, ANN_FRAMES, hex_str);

    pos = 0;
    operands = 0;
    for (i = 0; i < s->cmd_bytes_count; i++) {
        if (i == 0) {
            uint8_t header = s->cmd_bytes[i].val;
            int src = (header >> 4) & 0x0F;
            int dst = header & 0x0F;
            pos += snprintf(sec_str + pos, sizeof(sec_str) - pos,
                "HDR: %s, %s", resolve_logical_address(src, 1), resolve_logical_address(dst, 0));
        } else if (i == 1) {
            pos += snprintf(sec_str + pos, sizeof(sec_str) - pos,
                " | OPC: %s", lookup_opcode(s->cmd_bytes[i].val));
        } else {
            if (operands == 0)
                pos += snprintf(sec_str + pos, sizeof(sec_str) - pos, " | OPS: ");
            operands++;
            pos += snprintf(sec_str + pos, sizeof(sec_str) - pos, "0x%02x", s->cmd_bytes[i].val);
            if (i != s->cmd_bytes_count - 1)
                pos += snprintf(sec_str + pos, sizeof(sec_str) - pos, ", ");
        }
    }

    if (s->cmd_bytes_count == 1) {
        if (s->eom)
            pos += snprintf(sec_str + pos, sizeof(sec_str) - pos, " | OPC: PING");
        else
            pos += snprintf(sec_str + pos, sizeof(sec_str) - pos, " | OPC: NONE. Aborted cmd");
    }

    pos += snprintf(sec_str + pos, sizeof(sec_str) - pos,
        " | R: %s", is_nack ? "NACK" : "ACK");
    c_put(di, s->frame_start, s->frame_end, s->out_ann, ANN_SECTIONS, sec_str);
}

static void cec_process(struct srd_decoder_inst* di, cec_priv* s)
{
    double zero_time = ((double)(s->rise - s->fall_start) / (double)s->samplerate) * 1000.0;
    double total_time = ((double)(s->fall_end - s->fall_start) / (double)s->samplerate) * 1000.0;
    enum cec_pulse pulse = PULSE_INVALID;
    int bit = 0;

    if (zero_time >= START_LOW_MIN && zero_time <= START_LOW_MAX)
        pulse = PULSE_START;
    else if (zero_time >= ZERO_LOW_MIN && zero_time <= ZERO_LOW_MAX)
        pulse = PULSE_ZERO;
    else if (zero_time >= ONE_LOW_MIN && zero_time <= ONE_LOW_MAX)
        pulse = PULSE_ONE;

    if (pulse == PULSE_INVALID) {
        s->state = WAIT_START;
        c_put(di, s->fall_start, s->fall_end, s->out_ann, ANN_WARN,
            "Invalid pulse: Wrong timing");
        return;
    }

    if (s->state == WAIT_START && pulse != PULSE_START) {
        c_put(di, s->fall_start, s->fall_end, s->out_ann, ANN_WARN,
            "Expected START: BIT found");
        return;
    }

    if ((s->state == WAIT_ACK || s->state == WAIT_EOM) && pulse == PULSE_START) {
        c_put(di, s->fall_start, s->fall_end, s->out_ann, ANN_WARN,
            "Expected BIT: START received)");
        s->state = WAIT_START;
    }

    if (s->state == WAIT_ACK && pulse != PULSE_START) {
        double total_min = (pulse == PULSE_ZERO) ? ZERO_TOTAL_MIN : ONE_TOTAL_MIN;
        if (total_time < total_min) {
            c_put(di, s->fall_start, s->fall_end, s->out_ann, ANN_WARN,
                "ACK pulse below minimun time");
            s->state = WAIT_START;
            return;
        }
    }

    if (s->state == GET_BITS && pulse == PULSE_START) {
        if (s->bit_count == 0) {
            handle_frame(di, s, s->is_nack);
        } else {
            c_put(di, s->frame_start, s->fall_end, s->out_ann, ANN_WARN,
                "ERROR: Incomplete byte received");
        }
        s->state = WAIT_START;
    }

    if (s->state != WAIT_ACK && pulse != PULSE_START) {
        double total_min = (pulse == PULSE_ZERO) ? ZERO_TOTAL_MIN : ONE_TOTAL_MIN;
        double total_max = (pulse == PULSE_ZERO) ? ZERO_TOTAL_MAX : ONE_TOTAL_MAX;
        if (total_time < total_min || total_time > total_max) {
            c_put(di, s->fall_start, s->fall_end, s->out_ann, ANN_WARN,
                "Bit pulse exceeds total pulse timespan");
            pulse = PULSE_INVALID;
            s->state = WAIT_START;
            return;
        }
    }

    if (pulse == PULSE_ZERO)
        bit = 0;
    else if (pulse == PULSE_ONE)
        bit = 1;

    switch (s->state) {

    case WAIT_START:
        s->state = GET_BITS;
        reset_frame_vars(s);
        c_put(di, s->fall_start, s->fall_end, s->out_ann, ANN_START, "ST");
        break;

    case GET_BITS:
        if (s->bit_count == 0) {
            s->byte_start = s->fall_start;
            s->byte_val = 0;
            if (s->cmd_bytes_count == 0)
                s->frame_start = s->fall_start;
        }
        s->byte_val |= (bit << (7 - s->bit_count));
        s->bit_count++;
        {
            char bit_str[16];
            snprintf(bit_str, sizeof(bit_str), "%d", bit);
            c_put(di, s->fall_start, s->fall_end, s->out_ann, ANN_BITS, bit_str);
        }
        if (s->bit_count == 8) {
            char byte_str[8];
            s->bit_count = 0;
            s->byte_count++;
            s->state = WAIT_EOM;
            snprintf(byte_str, sizeof(byte_str), "0x%02x", s->byte_val);
            c_put(di, s->byte_start, s->fall_end, s->out_ann, ANN_BYTES, byte_str);
            if (s->cmd_bytes_count < MAX_CMD_BYTES) {
                s->cmd_bytes[s->cmd_bytes_count].st = s->byte_start;
                s->cmd_bytes[s->cmd_bytes_count].ed = s->fall_end;
                s->cmd_bytes[s->cmd_bytes_count].val = s->byte_val;
                s->cmd_bytes_count++;
            }
        }
        break;

    case WAIT_EOM:
        s->eom = bit;
        s->frame_end = s->fall_end;
        if (s->eom)
            c_put(di, s->fall_start, s->fall_end, s->out_ann, ANN_EOM_1, "EOM=Y");
        else
            c_put(di, s->fall_start, s->fall_end, s->out_ann, ANN_EOM_0, "EOM=N");
        s->state = WAIT_ACK;
        break;

    case WAIT_ACK: {
        int ack_bit = bit;
        if (s->cmd_bytes_count > 0 && (s->cmd_bytes[0].val & 0x0F) == 0x0F)
            ack_bit = ~ack_bit & 0x01;
        {
            uint64_t ann_end;
            if ((s->fall_end - s->fall_start) > s->max_ack_len_samples)
                ann_end = s->fall_start + s->max_ack_len_samples;
            else
                ann_end = s->fall_end;
            if (ack_bit) {
                s->is_nack = 1;
                c_put(di, s->fall_start, ann_end, s->out_ann, ANN_NACK, "NACK");
            } else {
                c_put(di, s->fall_start, ann_end, s->out_ann, ANN_ACK, "ACK");
            }
        }
        if (s->eom || s->is_nack) {
            s->state = WAIT_START;
            handle_frame(di, s, s->is_nack);
        } else {
            s->state = GET_BITS;
        }
        break;
    }
    }
}

static void cec_metadata(struct srd_decoder_inst* di, int key, uint64_t value)
{
    cec_priv* s = (cec_priv*)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        s->max_ack_len_samples = (uint64_t)((4.1 / 1000.0) * (double)value + 0.5);
    }
}

static void cec_reset(struct srd_decoder_inst* di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(cec_priv)));
    }
    cec_priv* s = (cec_priv*)c_decoder_get_private(di);
    memset(s, 0, sizeof(cec_priv));
    s->state = WAIT_START;
}

static void cec_start(struct srd_decoder_inst* di)
{
    cec_priv* s = (cec_priv*)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "cec");
    s->samplerate = c_samplerate(di);
    if (s->samplerate > 0)
        s->max_ack_len_samples = (uint64_t)((4.1 / 1000.0) * (double)s->samplerate + 0.5);
}

static void cec_decode(struct srd_decoder_inst* di)
{
    cec_priv* s = (cec_priv*)c_decoder_get_private(di);
    int ret;

    if (!s->samplerate) {
        s->samplerate = c_samplerate(di);
        if (s->samplerate > 0)
            s->max_ack_len_samples = (uint64_t)((4.1 / 1000.0) * (double)s->samplerate + 0.5);
    }
    if (s->samplerate == 0)
        return;

    {
        ret = c_wait(di, CW_F(CEC_CH), CW_END);
        if (ret != SRD_OK)
            return;
        s->fall_end = di_samplenum(di);
    }

    while (1) {
        {
            ret = c_wait(di, CW_R(CEC_CH), CW_END);
            if (ret != SRD_OK)
                return;
            s->rise = di_samplenum(di);
        }

        {
            if (s->state == WAIT_ACK)
                ret = c_wait(di, CW_F(CEC_CH), CW_OR, CW_SKIP(s->max_ack_len_samples), CW_END);
            else
                ret = c_wait(di, CW_F(CEC_CH), CW_END);
            if (ret != SRD_OK)
                return;
        }

        s->fall_start = s->fall_end;
        s->fall_end = di_samplenum(di);
        cec_process(di, s);

        if (di_matched(di) & 2) {
            ret = c_wait(di, CW_F(CEC_CH), CW_END);
            if (ret != SRD_OK)
                return;
            s->fall_end = di_samplenum(di);
        }
    }
}

static void cec_destroy(struct srd_decoder_inst* di)
{
    void* priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder cec_c_decoder = {
    .id = "cec_c",
    .name = "CEC(C)",
    .longname = "HDMI-CEC(C)",
    .desc = "HDMI Consumer Electronics Control (CEC) protocol. (C implementation)",
    .license = "gplv2+",
    .channels = cec_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = cec_ann_labels,
    .num_annotation_rows = 5,
    .annotation_rows = cec_ann_rows,
    .inputs = cec_inputs,
    .num_inputs = 1,
    .outputs = cec_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = cec_tags,
    .num_tags = 2,
    .metadata = cec_metadata,
    .reset = cec_reset,
    .start = cec_start,
    .decode = cec_decode,
    .destroy = cec_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder* srd_c_decoder_entry(void)
{
    return &cec_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}