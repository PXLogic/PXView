#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_CMD = 0,
    ANN_PARAMS,
    ANN_DISABLED,
    ANN_RETURN,
    ANN_DISABLED_RETURN,
    ANN_INTERPRETATION,
    NUM_ANN,
};

#define RFM12_MAX_BYTES 4

typedef struct {
    int out_ann;

    /* Collected bytes */
    uint8_t mosi_bytes[RFM12_MAX_BYTES];
    uint8_t miso_bytes[RFM12_MAX_BYTES];
    int num_bytes;
    uint64_t byte_ss[RFM12_MAX_BYTES];
    uint64_t byte_es[RFM12_MAX_BYTES];

    /* State tracking (Power-On-Reset values) */
    uint8_t last_status[2];
    uint8_t last_config;
    uint8_t last_power;
    uint16_t last_freq;
    uint8_t last_data_rate;
    uint8_t last_fifo_and_reset;
    uint8_t last_afc;
    uint8_t last_transceiver;
    uint8_t last_pll;
} rfm12_state;

static const char *rfm12_inputs[] = {"spi", NULL};
static const char *rfm12_tags[] = {"Wireless/RF", NULL};

static const char *rfm12_ann_labels[][3] = {
    {"", "cmd", "Command"},
    {"", "params", "Command parameters"},
    {"", "disabled", "Disabled bits"},
    {"", "return", "Returned values"},
    {"", "disabled_return", "Disabled returned values"},
    {"", "interpretation", "Interpretation"},
};

static const int rfm12_row_commands_classes[] = {ANN_CMD, ANN_PARAMS, ANN_DISABLED, -1};
static const int rfm12_row_return_classes[] = {ANN_RETURN, ANN_DISABLED_RETURN, -1};
static const int rfm12_row_interpretation_classes[] = {ANN_INTERPRETATION, -1};

static const struct srd_c_ann_row rfm12_ann_rows[] = {
    {"commands", "Commands", rfm12_row_commands_classes, 3},
    {"return", "Return", rfm12_row_return_classes, 2},
    {"interpretation", "Interpretation", rfm12_row_interpretation_classes, 1},
};

/* Helper: get ss/es for the full command span */
static void rfm12_get_span(rfm12_state *s, uint64_t *ss, uint64_t *es)
{
    if (s->num_bytes > 0) {
        *ss = s->byte_ss[0];
        *es = s->byte_es[s->num_bytes - 1];
    } else {
        *ss = 0; *es = 0;
    }
}

static void rfm12_handle_configuration_cmd(struct srd_decoder_inst *di, rfm12_state *s)
{
    
    uint64_t ss, es;
    rfm12_get_span(s, &ss, &es);
    uint8_t cmd1 = s->mosi_bytes[1];

    c_put(di, ss, es, s->out_ann, ANN_CMD, "Configuration command", "Configuration");

    /* Frequency band */
    static const char *frequencies[] = {"315", "433", "868", "915"};
    int freq_idx = (cmd1 & 0x30) >> 4;
    char buf[64];
    snprintf(buf, sizeof(buf), "Frequency: %sMHz", frequencies[freq_idx]);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf, frequencies[freq_idx]);

    if ((cmd1 & 0x30) != (s->last_config & 0x30))
        c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION, "Changed", "~");

    /* Capacitance */
    double cap = 8.5 + (cmd1 & 0xF) * 0.5;
    char cap_buf[32];
    snprintf(cap_buf, sizeof(cap_buf), "%.1fpF", cap);
    snprintf(buf, sizeof(buf), "Capacitance: %s", cap_buf);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf, cap_buf);

    if ((cmd1 & 0xF) != (s->last_config & 0xF))
        c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION, "Changed", "~");

    s->last_config = cmd1;
}

static void rfm12_handle_power_management_cmd(struct srd_decoder_inst *di, rfm12_state *s)
{
    
    uint64_t ss, es;
    rfm12_get_span(s, &ss, &es);
    uint8_t cmd1 = s->mosi_bytes[1];

    c_put(di, ss, es, s->out_ann, ANN_CMD, "Power management", "Power");

    /* Describe enabled/disabled bits */
    static const char *names[] = {
        "Receiver chain (er)", "Baseband circuit (ebb)", "Transmission (et)",
        "Synthesizer (es)", "Crystal oscillator (ex)", "Low battery detector (eb)",
        "Wake-up timer (ew)", "Clock output off switch (dc)"
    };
    char buf[512];
    int pos = 0;
    for (int i = 7; i >= 0; i--) {
        if (cmd1 & (1 << i)) {
            if (pos > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ", ");
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%s", names[7 - i]);
        }
    }
    if (pos > 0)
        c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);

    s->last_power = cmd1;
}

static void rfm12_handle_frequency_setting_cmd(struct srd_decoder_inst *di, rfm12_state *s)
{
    
    uint64_t ss, es;
    rfm12_get_span(s, &ss, &es);

    c_put(di, ss, es, s->out_ann, ANN_CMD, "Frequency setting", "Frequency");

    if (s->num_bytes >= 3) {
        int f = ((s->mosi_bytes[1] & 0xF) << 8) + s->mosi_bytes[2];
        char buf[64];
        snprintf(buf, sizeof(buf), "F = %3.4f", (double)f);
        c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);
        if (s->last_freq != (uint16_t)f)
            c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION, "Changing", "~");
        s->last_freq = (uint16_t)f;
    }
}

static void rfm12_handle_data_rate_cmd(struct srd_decoder_inst *di, rfm12_state *s)
{
    
    uint64_t ss, es;
    rfm12_get_span(s, &ss, &es);
    uint8_t cmd1 = s->mosi_bytes[1];

    c_put(di, ss, es, s->out_ann, ANN_CMD, "Data rate command", "Data rate");

    int r = cmd1 & 0x7F;
    int cs = (cmd1 & 0x80) >> 7;
    double rate = 10000.0 / 29.0 / (r + 1) / (1 + 7 * cs);
    char buf[64];
    snprintf(buf, sizeof(buf), "%3.1fkbps", rate);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);

    if (s->last_data_rate != cmd1)
        c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION, "Changing", "~");
    s->last_data_rate = cmd1;
}

static void rfm12_handle_receiver_control_cmd(struct srd_decoder_inst *di, rfm12_state *s)
{
    
    uint64_t ss, es;
    rfm12_get_span(s, &ss, &es);

    c_put(di, ss, es, s->out_ann, ANN_CMD, "Receiver control command");

    const char *pin16 = (s->mosi_bytes[0] & 0x04) ? "interrupt input" : "VDI output";
    char buf[64];
    snprintf(buf, sizeof(buf), "pin16 = %s", pin16);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);

    static const char *vdi_names[] = {"Fast", "Medium", "Slow", "Always on"};
    int vdi_speed = s->mosi_bytes[0] & 0x3;
    snprintf(buf, sizeof(buf), "VDI: %s", vdi_names[vdi_speed]);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);

    static const char *bandwidth_names[] = {
        "Reserved", "400kHz", "340kHz", "270kHz", "200kHz",
        "134kHz", "67kHz", "Reserved"
    };
    int bw_idx = (s->mosi_bytes[1] & 0xE0) >> 5;
    snprintf(buf, sizeof(buf), "Bandwidth: %s", bandwidth_names[bw_idx]);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);

    static const int lna_gains[] = {0, -6, -14, -20};
    int lna_idx = (s->mosi_bytes[1] & 0x18) >> 3;
    snprintf(buf, sizeof(buf), "LNA gain: %ddB", lna_gains[lna_idx]);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);

    static const char *rssi_names[] = {
        "-103", "-97", "-91", "-85", "-79", "-73", "Reserved", "Reserved"
    };
    int rssi_idx = s->mosi_bytes[1] & 0x7;
    snprintf(buf, sizeof(buf), "RSSI threshold: %s", rssi_names[rssi_idx]);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);
}

static void rfm12_handle_data_filter_cmd(struct srd_decoder_inst *di, rfm12_state *s)
{
    
    uint64_t ss, es;
    rfm12_get_span(s, &ss, &es);
    uint8_t cmd1 = s->mosi_bytes[1];

    c_put(di, ss, es, s->out_ann, ANN_CMD, "Data filter command");

    const char *clock_recovery;
    if (cmd1 & 0x80)
        clock_recovery = "auto";
    else if (cmd1 & 0x40)
        clock_recovery = "fast";
    else
        clock_recovery = "slow";
    char buf[64];
    snprintf(buf, sizeof(buf), "Clock recovery: %s mode", clock_recovery);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);

    const char *data_filter = (cmd1 & 0x10) ? "analog" : "digital";
    snprintf(buf, sizeof(buf), "Data filter: %s", data_filter);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);

    snprintf(buf, sizeof(buf), "DQD threshold: %d", cmd1 & 0x7);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);
}

static void rfm12_handle_fifo_and_reset_cmd(struct srd_decoder_inst *di, rfm12_state *s)
{
    
    uint64_t ss, es;
    rfm12_get_span(s, &ss, &es);
    uint8_t cmd1 = s->mosi_bytes[1];

    c_put(di, ss, es, s->out_ann, ANN_CMD, "FIFO and reset command");

    int fifo_level = (cmd1 & 0xF0) >> 4;
    char buf[64];
    snprintf(buf, sizeof(buf), "FIFO trigger level: %d", fifo_level);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);
    if (fifo_level != ((s->last_fifo_and_reset & 0xF0) >> 4))
        c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION, "Changing", "~");

    const char *sync_len = (cmd1 & 0x08) ? "one byte" : "two bytes";
    snprintf(buf, sizeof(buf), "Synchron length: %s", sync_len);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);
    if ((cmd1 & 0x08) != (s->last_fifo_and_reset & 0x08))
        c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION, "Changing", "~");

    const char *fifo_fill;
    if (cmd1 & 0x04)
        fifo_fill = "Always";
    else if (cmd1 & 0x02)
        fifo_fill = "After synchron pattern";
    else
        fifo_fill = "Never";
    snprintf(buf, sizeof(buf), "FIFO fill: %s", fifo_fill);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);
    if ((cmd1 & 0x06) != (s->last_fifo_and_reset & 0x06))
        c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION, "Changing", "~");

    const char *reset_mode = (cmd1 & 0x01) ? "non-sensitive" : "sensitive";
    snprintf(buf, sizeof(buf), "Reset mode: %s", reset_mode);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);
    if ((cmd1 & 0x01) != (s->last_fifo_and_reset & 0x01))
        c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION, "Changing", "~");

    s->last_fifo_and_reset = cmd1;
}

static void rfm12_handle_synchron_pattern_cmd(struct srd_decoder_inst *di, rfm12_state *s)
{
    
    uint64_t ss, es;
    rfm12_get_span(s, &ss, &es);

    c_put(di, ss, es, s->out_ann, ANN_CMD, "Synchron pattern command");

    if (s->num_bytes >= 2) {
        char buf[64];
        if (s->last_fifo_and_reset & 0x08) {
            snprintf(buf, sizeof(buf), "Pattern: 0x2D%02X", s->mosi_bytes[1]);
        } else {
            snprintf(buf, sizeof(buf), "Pattern: 0x%02X", s->mosi_bytes[1]);
        }
        c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);
    }
}

static void rfm12_handle_fifo_read_cmd(struct srd_decoder_inst *di, rfm12_state *s)
{
    
    uint64_t ss, es;
    rfm12_get_span(s, &ss, &es);

    c_put(di, ss, es, s->out_ann, ANN_CMD, "FIFO read command", "FIFO read");

    if (s->num_bytes >= 2) {
        char long_str[64], short_str[32];
        snprintf(long_str, sizeof(long_str), "Data = \"{$}\"");
        snprintf(short_str, sizeof(short_str), "@%02X", s->miso_bytes[1]);
        c_put(di, ss, es, s->out_ann, ANN_RETURN, long_str, short_str);
    }
}

static void rfm12_handle_afc_cmd(struct srd_decoder_inst *di, rfm12_state *s)
{
    
    uint64_t ss, es;
    rfm12_get_span(s, &ss, &es);
    uint8_t cmd1 = s->mosi_bytes[1];

    c_put(di, ss, es, s->out_ann, ANN_CMD, "AFC command");

    static const char *afc_modes[] = {"Off", "Once", "During receiving", "Always"};
    int mode = (cmd1 & 0xC0) >> 6;
    char buf[64];
    snprintf(buf, sizeof(buf), "Mode: %s", afc_modes[mode]);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);
    if ((cmd1 & 0xC0) != (s->last_afc & 0xC0))
        c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION, "Changing", "~");

    int range_limit = (cmd1 & 0x30) >> 4;
    static const double freq_table[] = {0.0, 2.5, 5.0, 7.5};
    double freq_delta = freq_table[(s->last_config & 0x30) >> 4];

    if (range_limit == 0)
        snprintf(buf, sizeof(buf), "Range: No limit");
    else if (range_limit == 1)
        snprintf(buf, sizeof(buf), "Range: +/-%.0fkHz", 15 * freq_delta);
    else if (range_limit == 2)
        snprintf(buf, sizeof(buf), "Range: +/-%.0fkHz", 7 * freq_delta);
    else
        snprintf(buf, sizeof(buf), "Range: +/-%.0fkHz", 3 * freq_delta);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);
    if ((cmd1 & 0x30) != (s->last_afc & 0x30))
        c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION, "Changing", "~");

    s->last_afc = cmd1;
}

static void rfm12_handle_transceiver_control_cmd(struct srd_decoder_inst *di, rfm12_state *s)
{
    
    uint64_t ss, es;
    rfm12_get_span(s, &ss, &es);
    uint8_t cmd1 = s->mosi_bytes[1];

    c_put(di, ss, es, s->out_ann, ANN_CMD, "Transceiver control command");

    int fsk_delta = 15 * ((cmd1 & 0xF0) >> 4);
    char buf[64];
    snprintf(buf, sizeof(buf), "FSK frequency delta: %dkHz", fsk_delta);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);
    if ((cmd1 & 0xF0) != (s->last_transceiver & 0xF0))
        c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION, "Changing", "~");

    snprintf(buf, sizeof(buf), "Relative power: %ddB", cmd1 & 0x07);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);
    if ((cmd1 & 0x07) != (s->last_transceiver & 0x07))
        c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION, "Changing", "~");

    s->last_transceiver = cmd1;
}

static void rfm12_handle_pll_setting_cmd(struct srd_decoder_inst *di, rfm12_state *s)
{
    
    uint64_t ss, es;
    rfm12_get_span(s, &ss, &es);
    uint8_t cmd1 = s->mosi_bytes[1];

    c_put(di, ss, es, s->out_ann, ANN_CMD, "PLL setting command");

    c_put(di, ss, es, s->out_ann, ANN_PARAMS, "Clock buffer rise and fall time");

    const char *max_bit_rate = (cmd1 & 0x01) ? "256kbps, high noise" : "86.2kbps, low noise";
    char buf[64];
    snprintf(buf, sizeof(buf), "Max bit rate: %s", max_bit_rate);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);
    if ((cmd1 & 0x01) != (s->last_pll & 0x01))
        c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION, "Changing", "~");

    s->last_pll = cmd1;
}

static void rfm12_handle_transmitter_register_cmd(struct srd_decoder_inst *di, rfm12_state *s)
{
    
    uint64_t ss, es;
    rfm12_get_span(s, &ss, &es);

    c_put(di, ss, es, s->out_ann, ANN_CMD, "Transmitter register command", "Transmit");

    if (s->num_bytes >= 2) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Data: 0x%02X", s->mosi_bytes[1]);
        c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);
    }
}

static void rfm12_handle_software_reset_cmd(struct srd_decoder_inst *di, rfm12_state *s)
{
    
    uint64_t ss, es;
    rfm12_get_span(s, &ss, &es);
    c_put(di, ss, es, s->out_ann, ANN_CMD, "Software reset command");
}

static void rfm12_handle_wake_up_timer_cmd(struct srd_decoder_inst *di, rfm12_state *s)
{
    
    uint64_t ss, es;
    rfm12_get_span(s, &ss, &es);

    c_put(di, ss, es, s->out_ann, ANN_CMD, "Wake-up timer command", "Timer");

    if (s->num_bytes >= 2) {
        int r = s->mosi_bytes[0] & 0x1F;
        int m = s->mosi_bytes[1];
        double time = 1.03 * m * pow(2.0, r) + 0.5;
        char buf[64];
        snprintf(buf, sizeof(buf), "Time: %7.2f", time);
        c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf);
    }
}

static void rfm12_handle_low_duty_cycle_cmd(struct srd_decoder_inst *di, rfm12_state *s)
{
    
    uint64_t ss, es;
    rfm12_get_span(s, &ss, &es);
    c_put(di, ss, es, s->out_ann, ANN_CMD, "Low duty cycle command");
}

static void rfm12_handle_low_battery_detector_cmd(struct srd_decoder_inst *di, rfm12_state *s)
{
    
    uint64_t ss, es;
    rfm12_get_span(s, &ss, &es);
    uint8_t cmd1 = s->mosi_bytes[1];

    c_put(di, ss, es, s->out_ann, ANN_CMD, "Low battery detector command");

    static const char *clock_names[] = {"1", "1.25", "1.66", "2", "2.5", "3.33", "5", "10"};
    int clock_idx = (cmd1 & 0xE0) >> 5;
    char buf[64];
    snprintf(buf, sizeof(buf), "Clock output: %sMHz", clock_names[clock_idx]);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf, clock_names[clock_idx]);

    double v = 2.25 + (cmd1 & 0x0F) * 0.1;
    char v_buf[16];
    snprintf(v_buf, sizeof(v_buf), "%.2fV", v);
    snprintf(buf, sizeof(buf), "Low battery voltage: %s", v_buf);
    c_put(di, ss, es, s->out_ann, ANN_PARAMS, buf, v_buf);
}

static void rfm12_handle_status_read_cmd(struct srd_decoder_inst *di, rfm12_state *s)
{
    
    uint64_t ss, es;
    rfm12_get_span(s, &ss, &es);

    c_put(di, ss, es, s->out_ann, ANN_CMD, "Status read command", "Status");

    if (s->num_bytes >= 2) {
        int receiver_enabled = (s->last_power & 0x80) >> 7;
        uint8_t ret0 = s->miso_bytes[0];
        uint8_t ret1 = s->miso_bytes[1];

        /* Status bit interpretations */
        if (ret0 & 0x80) {
            c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION,
                      receiver_enabled ? "Received data in FIFO" : "Transmit register ready");
        }
        if (ret0 & 0x40)
            c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION, "Power on Reset");
        if (ret0 & 0x20) {
            c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION,
                      receiver_enabled ? "RX FIFO overflow" : "Transmit register under run");
        }
        if (ret0 & 0x10)
            c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION, "Wake-up timer");
        if (ret0 & 0x08)
            c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION, "External interrupt");
        if (ret0 & 0x04)
            c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION, "Low battery");
        if (ret0 & 0x02)
            c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION, "FIFO is empty");
        if (ret0 & 0x01) {
            c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION,
                      receiver_enabled ? "Incoming signal above limit" : "Antenna detected RF signal");
        }
        if (ret1 & 0x80)
            c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION, "Data quality detector");
        if (ret1 & 0x40)
            c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION, "Clock recovery locked");

        /* AFC offset */
        c_put(di, ss, es, s->out_ann, ANN_RETURN, "AFC offset");
        if ((s->last_status[1] & 0x1F) != (ret1 & 0x1F))
            c_put(di, ss, es, s->out_ann, ANN_INTERPRETATION, "Changed", "~");

        s->last_status[0] = ret0;
        s->last_status[1] = ret1;
    }
}

static void rfm12_handle_cmd(struct srd_decoder_inst *di, rfm12_state *s)
{
    uint8_t cmd0 = s->mosi_bytes[0];

    if (cmd0 == 0x80)
        rfm12_handle_configuration_cmd(di, s);
    else if (cmd0 == 0x82)
        rfm12_handle_power_management_cmd(di, s);
    else if ((cmd0 & 0xF0) == 0xA0)
        rfm12_handle_frequency_setting_cmd(di, s);
    else if (cmd0 == 0xC6)
        rfm12_handle_data_rate_cmd(di, s);
    else if ((cmd0 & 0xF8) == 0x90)
        rfm12_handle_receiver_control_cmd(di, s);
    else if (cmd0 == 0xC2)
        rfm12_handle_data_filter_cmd(di, s);
    else if (cmd0 == 0xCA)
        rfm12_handle_fifo_and_reset_cmd(di, s);
    else if (cmd0 == 0xCE)
        rfm12_handle_synchron_pattern_cmd(di, s);
    else if (cmd0 == 0xB0)
        rfm12_handle_fifo_read_cmd(di, s);
    else if (cmd0 == 0xC4)
        rfm12_handle_afc_cmd(di, s);
    else if ((cmd0 & 0xFE) == 0x98)
        rfm12_handle_transceiver_control_cmd(di, s);
    else if (cmd0 == 0xCC)
        rfm12_handle_pll_setting_cmd(di, s);
    else if (cmd0 == 0xB8)
        rfm12_handle_transmitter_register_cmd(di, s);
    else if (cmd0 == 0xFE)
        rfm12_handle_software_reset_cmd(di, s);
    else if ((cmd0 & 0xE0) == 0xE0)
        rfm12_handle_wake_up_timer_cmd(di, s);
    else if (cmd0 == 0xC8)
        rfm12_handle_low_duty_cycle_cmd(di, s);
    else if (cmd0 == 0xC0)
        rfm12_handle_low_battery_detector_cmd(di, s);
    else if (cmd0 == 0x00)
        rfm12_handle_status_read_cmd(di, s);
    else {
        
        uint64_t ss, es;
    rfm12_get_span(s, &ss, &es);
        char buf[64];
        snprintf(buf, sizeof(buf), "Unknown command: 0x%02X 0x%02X", s->mosi_bytes[0],
                 (s->num_bytes >= 2) ? s->mosi_bytes[1] : 0);
        c_put(di, ss, es, s->out_ann, ANN_CMD, buf);
    }
}

static void rfm12_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    rfm12_state *s = (rfm12_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "DATA") == 0 && n_fields >= 17) {
        
        
        uint64_t mosi_val = 0, miso_val = 0;
        for (int i = 0; i < 8; i++)
            mosi_val |= ((uint64_t)fields[1 + i].u8 << (8 * i));
        for (int i = 0; i < 8; i++)
            miso_val |= ((uint64_t)fields[9 + i].u8 << (8 * i));

        if (s->num_bytes < RFM12_MAX_BYTES) {
            s->mosi_bytes[s->num_bytes] = (uint8_t)mosi_val;
            s->miso_bytes[s->num_bytes] = (uint8_t)miso_val;
            s->byte_ss[s->num_bytes] = start_sample;
            s->byte_es[s->num_bytes] = end_sample;
            s->num_bytes++;
        }

        /* All commands consist of 2 bytes, some need 3 */
        if (s->num_bytes >= 2) {
            /* Check if this command needs a 3rd byte */
            uint8_t cmd0 = s->mosi_bytes[0];
            int need_third = ((cmd0 & 0xF0) == 0xA0); /* frequency setting */

            if (need_third && s->num_bytes < 3)
                return;

            rfm12_handle_cmd(di, s);
            s->num_bytes = 0;
        }
    }
}

static void rfm12_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(rfm12_state)));
    }
    rfm12_state *s = (rfm12_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(rfm12_state));

    /* Initialize with Power-On-Reset values */
    s->last_status[0] = 0x00;
    s->last_status[1] = 0x00;
    s->last_config = 0x08;
    s->last_power = 0x08;
    s->last_freq = 0x680;
    s->last_data_rate = 0x23;
    s->last_fifo_and_reset = 0x80;
    s->last_afc = 0xF7;
    s->last_transceiver = 0x00;
    s->last_pll = 0x77;
}

static void rfm12_start(struct srd_decoder_inst *di)
{
    rfm12_state *s = (rfm12_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "rfm12");
}

static void rfm12_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void rfm12_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder rfm12_c_decoder = {
    .id = "rfm12_c",
    .name = "RFM12(C)",
    .longname = "HopeRF RFM12 (C)",
    .desc = "HopeRF RFM12 wireless transceiver control protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = rfm12_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = rfm12_ann_rows,
    .inputs = rfm12_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = rfm12_tags,
    .num_tags = 1,
    .reset = rfm12_reset,
    .start = rfm12_start,
    .decode = rfm12_decode,
    .destroy = rfm12_destroy,
    .decode_upper = rfm12_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &rfm12_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}