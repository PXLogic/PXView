#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_FIELD_NAME_VAL = 0,
    ANN_FIELD_VAL = 1,
    NUM_ANN,
};

enum xfp_state {
    XFP_IDLE,
    XFP_GET_SLAVE_ADDR,
    XFP_READ_REGS,
};

/* MODULE_ID lookup table */
static const struct { int id; const char *name; } xfp_module_id[] = {
    {0x01, "GBIC"},
    {0x02, "Integrated module/connector"},
    {0x03, "SFP"},
    {0x04, "300-pin XBI"},
    {0x05, "XENPAK"},
    {0x06, "XFP"},
    {0x07, "XFF"},
    {0x08, "XFP-E"},
    {0x09, "XPAK"},
    {0x0a, "X2"},
    {-1, NULL},
};

/* ALARM_THRESHOLDS lookup table */
static const struct { int idx; const char *name; int type; } xfp_alarm_thresholds[] = {
    {0,  "Temp high alarm", 1},      /* type 1=temp, 2=current, 3=power */
    {2,  "Temp low alarm", 1},
    {4,  "Temp high warning", 1},
    {6,  "Temp low warning", 1},
    {16, "Bias high alarm", 2},
    {18, "Bias low alarm", 2},
    {20, "Bias high warning", 2},
    {22, "Bias low warning", 2},
    {24, "TX power high alarm", 3},
    {26, "TX power low alarm", 3},
    {28, "TX power high warning", 3},
    {30, "TX power low warning", 3},
    {32, "RX power high alarm", 3},
    {34, "RX power low alarm", 3},
    {36, "RX power high warning", 3},
    {38, "RX power low warning", 3},
    {40, "AUX 1 high alarm", 0},
    {42, "AUX 1 low alarm", 0},
    {44, "AUX 1 high warning", 0},
    {46, "AUX 1 low warning", 0},
    {48, "AUX 2 high alarm", 0},
    {50, "AUX 2 low alarm", 0},
    {52, "AUX 2 high warning", 0},
    {54, "AUX 2 low warning", 0},
    {-1, NULL, 0},
};

/* AD_READOUTS lookup table */
static const struct { int idx; const char *name; int type; } xfp_ad_readouts[] = {
    {0,  "Module temperature", 1},
    {4,  "TX bias current", 2},
    {6,  "Measured TX output power", 3},
    {8,  "Measured RX input power", 3},
    {10, "AUX 1 measurement", 0},
    {12, "AUX 2 measurement", 0},
    {-1, NULL, 0},
};

/* GCS_BITS */
static const char *xfp_gcs_bits[] = {
    "TX disable", "Soft TX disable", "MOD_NR", "P_Down",
    "Soft P_Down", "Interrupt", "RX_LOS", "Data_Not_Ready",
    "TX_NR", "TX_Fault", "TX_CDR not locked", "RX_NR",
    "RX_CDR not locked",
};

/* CONNECTOR lookup table */
static const struct { int id; const char *name; } xfp_connector[] = {
    {0x01, "SC"},
    {0x02, "Fibre Channel style 1 copper"},
    {0x03, "Fibre Channel style 2 copper"},
    {0x04, "BNC/TNC"},
    {0x05, "Fibre Channel coax"},
    {0x06, "FiberJack"},
    {0x07, "LC"},
    {0x08, "MT-RJ"},
    {0x09, "MU"},
    {0x0a, "SG"},
    {0x0b, "Optical pigtail"},
    {0x20, "HSSDC II"},
    {0x21, "Copper pigtail"},
    {-1, NULL},
};

/* TRANSCEIVER lookup table */
static const char *xfp_transceiver[8][8] = {
    /* 10GB Ethernet */
    {"10GBASE-SR", "10GBASE-LR", "10GBASE-ER", "10GBASE-LRM", "10GBASE-SW",
     "10GBASE-LW", "10GBASE-EW", NULL},
    /* 10GB Fibre Channel */
    {"1200-MX-SN-I", "1200-SM-LL-L", "Extended Reach 1550 nm",
     "Intermediate reach 1300 nm FP", NULL, NULL, NULL, NULL},
    /* 10GB Copper */
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
    /* 10GB low speed */
    {"1000BASE-SX / 1xFC MMF", "1000BASE-LX / 1xFC SMF", "2xFC MMF",
     "2xFC SMF", "OC48-SR", "OC48-IR", "OC48-LR", NULL},
    /* 10GB SONET/SDH interconnect */
    {"I-64.1r", "I-64.1", "I-64.2r", "I-64.2", "I-64.3", "I-64.5", NULL, NULL},
    /* 10GB SONET/SDH short haul */
    {"S-64.1", "S-64.2a", "S-64.2b", "S-64.3a", "S-64.3b", "S-64.5a", "S-64.5b", NULL},
    /* 10GB SONET/SDH long haul */
    {"L-64.1", "L-64.2a", "L-64.2b", "L-64.2c", "L-64.3", "G.959.1 P1L1-2D2", NULL, NULL},
    /* 10GB SONET/SDH very long haul */
    {"V-64.2a", "V-64.2b", "V-64.3", NULL, NULL, NULL, NULL, NULL},
};

/* SERIAL_ENCODING */
static const char *xfp_serial_encoding[] = {
    "64B/66B", "8B/10B", "SONET scrambled", "NRZ", "RZ",
};

/* XMIT_TECH */
static const char *xfp_xmit_tech[] = {
    "850 nm VCSEL", "1310 nm VCSEL", "1550 nm VCSEL",
    "1310 nm FP", "1310 nm DFB", "1550 nm DFB",
    "1310 nm EML", "copper",
};

/* CDR */
static const char *xfp_cdr[] = {
    "9.95Gb/s", "10.3Gb/s", "10.5Gb/s", "10.7Gb/s",
    "11.1Gb/s", "(unknown)", "lineside loopback mode", "XFI loopback mode",
};

/* DEVICE_TECH */
static const char *xfp_device_tech[4][2] = {
    {"no wavelength control", "active wavelength control"},
    {"uncooled transmitter device", "cooled transmitter"},
    {"PIN detector", "APD detector"},
    {"transmitter not tunable", "transmitter tunable"},
};

/* ENHANCED_OPTS */
static const char *xfp_enhanced_opts[] = {
    "VPS", "soft TX_DISABLE", "soft P_Down", "VPS LV regulator mode",
    "VPS bypassed regulator mode", "active FEC control",
    "wavelength tunability", "CMU",
};

/* AUX_TYPES */
static const char *xfp_aux_types[] = {
    "not implemented", "APD bias voltage", "(unknown)", "TEC current",
    "laser temperature", "laser wavelength", "5V supply voltage",
    "3.3V supply voltage", "1.8V supply voltage", "-5.2V supply voltage",
    "5V supply current", "(unknown)", "(unknown)", "3.3V supply current",
    "1.8V supply current", "-5.2V supply current",
};

typedef struct {
    enum xfp_state state;
    int cnt;
    uint8_t buf[256];
    int buf_len;
    uint64_t sn[256][2];
    int cur_highmem_page;
    int have_clei;
    
    int out_ann;
    uint64_t ss;
    uint64_t es;
} xfp_state;

static const char *xfp_inputs[] = {"i2c", NULL};
static const char *xfp_tags[] = {"Networking", NULL};

static const char *xfp_ann_labels[][3] = {
    {"", "field-name-and-val", "Field name and value"},
    {"", "field-val", "Field value"},
};

static const int xfp_row_name_val_classes[] = {ANN_FIELD_NAME_VAL};
static const int xfp_row_val_classes[] = {ANN_FIELD_VAL};
static const struct srd_c_ann_row xfp_ann_rows[] = {
    {"field-names-and-vals", "Field names and values", xfp_row_name_val_classes, 1},
    {"field-vals", "Field values", xfp_row_val_classes, 1},
};

static void xfp_annotate(struct srd_decoder_inst *di, xfp_state *s,
    const char *key, const char *value, int start_cnt, int end_cnt)
{
    if (start_cnt < 0 || end_cnt < 0 || start_cnt > 255 || end_cnt > 255)
        return;
    char buf[512];
    snprintf(buf, sizeof(buf), "%s: %s", key, value);
    c_put(di, s->sn[start_cnt][0], s->sn[end_cnt][1], s->out_ann, ANN_FIELD_NAME_VAL, buf);
    c_put(di, s->sn[start_cnt][0], s->sn[end_cnt][1], s->out_ann, ANN_FIELD_VAL, value);
}

/* Convert 16-bit two's complement to temperature string */
static void xfp_to_temp(int value, char *buf, int buflen)
{
    if (value & 0x8000)
        value = -((value ^ 0xffff) + 1);
    double temp = value / 256.0;
    snprintf(buf, buflen, "%.1f C", temp);
}

/* TX bias current in uA -> mA */
static void xfp_to_current(int value, char *buf, int buflen)
{
    double current = value / 500000.0;
    snprintf(buf, buflen, "%.1f mA", current);
}

/* Power in 0.1uW -> mW */
static void xfp_to_power(int value, char *buf, int buflen)
{
    double power = value / 10000.0;
    snprintf(buf, buflen, "%.2f mW", power);
}

/* Wavelength in 0.05nm increments */
static void xfp_to_wavelength(int value, char *buf, int buflen)
{
    int wl = value / 20;
    snprintf(buf, buflen, "%d nm", wl);
}

/* Wavelength tolerance in 0.005nm increments */
static void xfp_to_wavelength_tolerance(int value, char *buf, int buflen)
{
    double wl = value / 200.0;
    snprintf(buf, buflen, "%.1f nm", wl);
}

static const char *xfp_lookup_module_id(int id)
{
    for (int i = 0; xfp_module_id[i].name != NULL; i++) {
        if (xfp_module_id[i].id == id)
            return xfp_module_id[i].name;
    }
    return "Unknown";
}

static const char *xfp_lookup_connector(int id)
{
    for (int i = 0; xfp_connector[i].name != NULL; i++) {
        if (xfp_connector[i].id == id)
            return xfp_connector[i].name;
    }
    return NULL;
}

static void xfp_handle_module_id(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    const char *name = xfp_lookup_module_id(s->buf[0]);
    xfp_annotate(di, s, "Module identifier", name, s->cnt, s->cnt);
}

static void xfp_handle_signal_cc(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    if (s->buf[0] != 0x00) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.2x", s->buf[0]);
        xfp_annotate(di, s, "Signal Conditioner Control", buf, s->cnt, s->cnt);
    }
}

static void xfp_handle_alarm_warnings(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    int cnt_idx = s->cnt - s->buf_len;
    int idx = 0;
    while (idx < 56) {
        if (idx == 8)
            idx += 8;  /* Skip reserved A/D flag thresholds */
        if (idx + 1 >= s->buf_len)
            break;
        int value = (s->buf[idx] << 8) | s->buf[idx + 1];
        if (value != 0) {
            const char *name = "...";
            int type = 0;
            for (int i = 0; xfp_alarm_thresholds[i].name != NULL; i++) {
                if (xfp_alarm_thresholds[i].idx == idx) {
                    name = xfp_alarm_thresholds[i].name;
                    type = xfp_alarm_thresholds[i].type;
                    break;
                }
            }
            char val_buf[32];
            switch (type) {
            case 1: xfp_to_temp(value, val_buf, sizeof(val_buf)); break;
            case 2: xfp_to_current(value, val_buf, sizeof(val_buf)); break;
            case 3: xfp_to_power(value, val_buf, sizeof(val_buf)); break;
            default: snprintf(val_buf, sizeof(val_buf), "%d", value); break;
            }
            xfp_annotate(di, s, name, val_buf, cnt_idx + idx, cnt_idx + idx + 1);
        }
        idx += 2;
    }
}

static void xfp_handle_vps(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    if (s->buf_len >= 2 && (s->buf[0] != 0 || s->buf[1] != 0)) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.2x%.2x", s->buf[0], s->buf[1]);
        xfp_annotate(di, s, "VPS", buf, s->cnt - 1, s->cnt);
    }
}

static void xfp_handle_ber(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    if (s->buf_len >= 2 && (s->buf[0] != 0 || s->buf[1] != 0)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "[%d, %d]", s->buf[0], s->buf[1]);
        xfp_annotate(di, s, "BER", buf, s->cnt - 1, s->cnt);
    }
}

static void xfp_handle_wavelength_cr(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    if (s->buf_len >= 4 && (s->buf[0] || s->buf[1] || s->buf[2] || s->buf[3])) {
        char buf[64];
        snprintf(buf, sizeof(buf), "[%d,%d,%d,%d]", s->buf[0], s->buf[1], s->buf[2], s->buf[3]);
        xfp_annotate(di, s, "WCR", buf, s->cnt - 3, s->cnt);
    }
}

static void xfp_handle_fec_cr(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    if (s->buf_len >= 4 && (s->buf[0] || s->buf[1] || s->buf[2] || s->buf[3])) {
        char buf[64];
        snprintf(buf, sizeof(buf), "[%d,%d,%d,%d]", s->buf[0], s->buf[1], s->buf[2], s->buf[3]);
        xfp_annotate(di, s, "FEC", buf, s->cnt - 3, s->cnt);
    }
}

static void xfp_handle_int_ctrl(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    char out[128];
    int pos = 0;
    for (int i = 0; i < s->buf_len && i < 16; i++) {
        if (pos > 0) pos += snprintf(out + pos, sizeof(out) - pos, " ");
        pos += snprintf(out + pos, sizeof(out) - pos, "%.2x", s->buf[i]);
    }
    xfp_annotate(di, s, "Interrupt bits", out,
                 s->cnt - s->buf_len + 1, s->cnt);
}

static void xfp_handle_ad_readout(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    int cnt_idx = s->cnt - s->buf_len + 1;
    int idx = 0;
    while (idx < 14) {
        if (idx == 2)
            idx += 2;  /* Skip reserved field */
        if (idx + 1 >= s->buf_len)
            break;
        int value = (s->buf[idx] << 8) | s->buf[idx + 1];
        const char *name = "...";
        int type = 0;
        for (int i = 0; xfp_ad_readouts[i].name != NULL; i++) {
            if (xfp_ad_readouts[i].idx == idx) {
                name = xfp_ad_readouts[i].name;
                type = xfp_ad_readouts[i].type;
                break;
            }
        }
        if (value != 0) {
            char val_buf[32];
            switch (type) {
            case 1: xfp_to_temp(value, val_buf, sizeof(val_buf)); break;
            case 2: xfp_to_current(value, val_buf, sizeof(val_buf)); break;
            case 3: xfp_to_power(value, val_buf, sizeof(val_buf)); break;
            default: snprintf(val_buf, sizeof(val_buf), "%d", value); break;
            }
            xfp_annotate(di, s, name, val_buf, cnt_idx + idx, cnt_idx + idx + 1);
        }
        idx += 2;
    }
}

static void xfp_handle_gcs(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    if (s->buf_len < 2)
        return;
    int allbits = (s->buf[0] << 8) | s->buf[1];
    char out[256];
    int pos = 0;
    for (int b = 0; b < 13; b++) {
        if (allbits & 0x8000) {
            if (pos > 0) pos += snprintf(out + pos, sizeof(out) - pos, ", ");
            pos += snprintf(out + pos, sizeof(out) - pos, "%s", xfp_gcs_bits[b]);
        }
        allbits <<= 1;
    }
    if (pos > 0)
        xfp_annotate(di, s, "General Control/Status", out, s->cnt - 1, s->cnt);
}

static void xfp_handle_page_select(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    s->cur_highmem_page = s->buf[0];
}

static void xfp_handle_ext_module_id(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    char out[128];
    int pos = 0;
    pos += snprintf(out + pos, sizeof(out) - pos, "Power level %d module", ((s->buf[0] >> 6) + 1));
    if ((s->buf[0] & 0x20) == 0)
        pos += snprintf(out + pos, sizeof(out) - pos, ", CDR");
    if ((s->buf[0] & 0x10) == 0)
        pos += snprintf(out + pos, sizeof(out) - pos, ", TX ref clock input required");
    if ((s->buf[0] & 0x08) == 0) {
        s->have_clei = 1;
    }
    xfp_annotate(di, s, "Extended id", out, s->cnt, s->cnt);
}

static void xfp_handle_connector(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    const char *name = xfp_lookup_connector(s->buf[0]);
    if (name)
        xfp_annotate(di, s, "Connector", name, s->cnt, s->cnt);
}

static void xfp_handle_transceiver(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    char out[512];
    int pos = 0;
    for (int t = 0; t < 8 && t < s->buf_len; t++) {
        if (s->buf[t] == 0)
            continue;
        uint8_t value = s->buf[t];
        for (int b = 0; b < 8; b++) {
            if (value & 0x80) {
                if (xfp_transceiver[t][b]) {
                    if (pos > 0) pos += snprintf(out + pos, sizeof(out) - pos, ", ");
                    pos += snprintf(out + pos, sizeof(out) - pos, "%s", xfp_transceiver[t][b]);
                } else {
                    if (pos > 0) pos += snprintf(out + pos, sizeof(out) - pos, ", ");
                    pos += snprintf(out + pos, sizeof(out) - pos, "(unknown)");
                }
            }
            value <<= 1;
        }
    }
    if (pos > 0)
        xfp_annotate(di, s, "Transceiver compliance", out,
                     s->cnt - s->buf_len + 1, s->cnt);
}

static void xfp_handle_serial_encoding(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    char out[256];
    int pos = 0;
    uint8_t value = s->buf[0];
    for (int b = 0; b < 8; b++) {
        if (value & 0x80) {
            if (b < 5 && xfp_serial_encoding[b]) {
                if (pos > 0) pos += snprintf(out + pos, sizeof(out) - pos, ", ");
                pos += snprintf(out + pos, sizeof(out) - pos, "%s", xfp_serial_encoding[b]);
            } else {
                if (pos > 0) pos += snprintf(out + pos, sizeof(out) - pos, ", ");
                pos += snprintf(out + pos, sizeof(out) - pos, "(unknown)");
            }
        }
        value <<= 1;
    }
    if (pos > 0)
        xfp_annotate(di, s, "Serial encoding support", out, s->cnt, s->cnt);
}

static void xfp_handle_br_min(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    double rate = s->buf[0] / 10.0;
    char buf[32];
    snprintf(buf, sizeof(buf), "%.3f GB/s", rate);
    xfp_annotate(di, s, "Minimum bit rate", buf, s->cnt, s->cnt);
}

static void xfp_handle_br_max(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    double rate = s->buf[0] / 10.0;
    char buf[32];
    snprintf(buf, sizeof(buf), "%.3f GB/s", rate);
    xfp_annotate(di, s, "Maximum bit rate", buf, s->cnt, s->cnt);
}

static void xfp_handle_link_length(struct srd_decoder_inst *di, xfp_state *s,
    const char *name, int multiplier)
{
    char buf[64];
    if (s->buf[0] == 0) {
        snprintf(buf, sizeof(buf), "(standard)");
    } else if (s->buf[0] == 255) {
        if (multiplier == 1)
            snprintf(buf, sizeof(buf), "> 254 km");
        else
            snprintf(buf, sizeof(buf), "> %d m", 254 * multiplier);
    } else {
        if (multiplier == 1 && strcmp(name, "Link length (SMF)") == 0)
            snprintf(buf, sizeof(buf), "%d km", s->buf[0]);
        else
            snprintf(buf, sizeof(buf), "%d m", s->buf[0] * multiplier);
    }
    xfp_annotate(di, s, name, buf, s->cnt, s->cnt);
}

static void xfp_handle_device_tech(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    char out[256];
    int pos = 0;
    int xmit = s->buf[0] >> 4;
    if (xmit <= 7) {
        pos += snprintf(out + pos, sizeof(out) - pos, "%s transmitter", xfp_xmit_tech[xmit]);
    }
    int dev = s->buf[0] & 0x0f;
    for (int b = 0; b < 4; b++) {
        if (pos > 0) pos += snprintf(out + pos, sizeof(out) - pos, ", ");
        pos += snprintf(out + pos, sizeof(out) - pos, "%s", xfp_device_tech[b][(dev >> (3 - b)) & 0x01]);
    }
    xfp_annotate(di, s, "Device technology", out, s->cnt, s->cnt);
}

static void xfp_handle_vendor(struct srd_decoder_inst *di, xfp_state *s, const char *field_name)
{
    char name[64];
    int len = s->buf_len < 63 ? s->buf_len : 63;
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (s->buf[i] >= 0x20 && s->buf[i] < 0x7f)
            name[j++] = s->buf[i];
    }
    name[j] = '\0';
    /* Trim trailing nulls/spaces */
    while (j > 0 && (name[j-1] == '\0' || name[j-1] == ' ')) name[--j] = '\0';
    if (j > 0)
        xfp_annotate(di, s, field_name, name,
                     s->cnt - s->buf_len + 1, s->cnt);
}

static void xfp_handle_cdr(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    char out[256];
    int pos = 0;
    uint8_t value = s->buf[0];
    for (int b = 0; b < 8; b++) {
        if (value & 0x80) {
            if (pos > 0) pos += snprintf(out + pos, sizeof(out) - pos, ", ");
            pos += snprintf(out + pos, sizeof(out) - pos, "%s", xfp_cdr[b]);
        }
        value <<= 1;
    }
    if (pos > 0)
        xfp_annotate(di, s, "CDR support", out, s->cnt, s->cnt);
}

static void xfp_handle_vendor_oui(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    if (s->buf_len >= 3 && (s->buf[0] || s->buf[1] || s->buf[2])) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.2X-%.2X-%.2X", s->buf[0], s->buf[1], s->buf[2]);
        xfp_annotate(di, s, "Vendor OUI", buf, s->cnt - 2, s->cnt);
    }
}

static void xfp_handle_wavelength(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    if (s->buf_len >= 2) {
        int value = (s->buf[0] << 8) | s->buf[1];
        char buf[32];
        xfp_to_wavelength(value, buf, sizeof(buf));
        xfp_annotate(di, s, "Wavelength", buf, s->cnt - 1, s->cnt);
    }
}

static void xfp_handle_wavelength_tolerance(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    if (s->buf_len >= 2) {
        int value = (s->buf[0] << 8) | s->buf[1];
        char buf[32];
        xfp_to_wavelength_tolerance(value, buf, sizeof(buf));
        xfp_annotate(di, s, "Wavelength tolerance", buf, s->cnt - 1, s->cnt);
    }
}

static void xfp_handle_max_case_temp(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    char buf[32];
    snprintf(buf, sizeof(buf), "%d C", s->buf[0]);
    xfp_annotate(di, s, "Maximum case temperature", buf, s->cnt, s->cnt);
}

static void xfp_handle_power_supply(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    if (s->buf_len < 4)
        return;
    char buf[64];
    double value;

    value = s->buf[0] * 0.02;
    snprintf(buf, sizeof(buf), "%.3f W", value);
    xfp_annotate(di, s, "Max power dissipation", buf, s->cnt - 3, s->cnt - 3);

    value = s->buf[1] * 0.01;
    snprintf(buf, sizeof(buf), "%.3f W", value);
    xfp_annotate(di, s, "Max power dissipation (powered down)", buf, s->cnt - 2, s->cnt - 2);

    value = (s->buf[2] >> 4) * 0.050;
    snprintf(buf, sizeof(buf), "%.3f A", value);
    xfp_annotate(di, s, "Max current required (5V supply)", buf, s->cnt - 1, s->cnt - 1);

    value = (s->buf[2] & 0x0f) * 0.100;
    snprintf(buf, sizeof(buf), "%.3f A", value);
    xfp_annotate(di, s, "Max current required (3.3V supply)", buf, s->cnt - 1, s->cnt - 1);

    value = (s->buf[3] >> 4) * 0.100;
    snprintf(buf, sizeof(buf), "%.3f A", value);
    xfp_annotate(di, s, "Max current required (1.8V supply)", buf, s->cnt, s->cnt);

    value = (s->buf[3] & 0x0f) * 0.050;
    snprintf(buf, sizeof(buf), "%.3f A", value);
    xfp_annotate(di, s, "Max current required (-5.2V supply)", buf, s->cnt, s->cnt);
}

static void xfp_handle_diag_mon(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    char out[128];
    int pos = 0;
    if (s->buf[0] & 0x10) {
        pos += snprintf(out + pos, sizeof(out) - pos, "BER support");
    } else {
        pos += snprintf(out + pos, sizeof(out) - pos, "no BER support");
    }
    if (s->buf[0] & 0x08) {
        pos += snprintf(out + pos, sizeof(out) - pos, ", average power measurement");
    } else {
        pos += snprintf(out + pos, sizeof(out) - pos, ", OMA power measurement");
    }
    xfp_annotate(di, s, "Diagnostic monitoring", out, s->cnt, s->cnt);
}

static void xfp_handle_enhanced_opts(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    char out[256];
    int pos = 0;
    uint8_t value = s->buf[0];
    for (int b = 0; b < 8; b++) {
        if (value & 0x80) {
            if (pos > 0) pos += snprintf(out + pos, sizeof(out) - pos, ", ");
            pos += snprintf(out + pos, sizeof(out) - pos, "%s", xfp_enhanced_opts[b]);
        }
        value <<= 1;
    }
    if (pos > 0)
        xfp_annotate(di, s, "Enhanced option support", out, s->cnt, s->cnt);
}

static void xfp_handle_aux_mon(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    int aux1_idx = s->buf[0] >> 4;
    int aux2_idx = s->buf[0] & 0x0f;
    const char *aux1 = (aux1_idx < 16) ? xfp_aux_types[aux1_idx] : "(unknown)";
    const char *aux2 = (aux2_idx < 16) ? xfp_aux_types[aux2_idx] : "(unknown)";
    xfp_annotate(di, s, "AUX1 monitoring", aux1, s->cnt, s->cnt);
    xfp_annotate(di, s, "AUX2 monitoring", aux2, s->cnt, s->cnt);
}

static void xfp_handle_manuf_date(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    if (s->buf_len < 6)
        return;
    int y = s->buf[0] * 10 + s->buf[1] + 2000;
    int m = s->buf[2] * 10 + s->buf[3];
    int d = s->buf[4] * 10 + s->buf[5];
    char buf[64];
    snprintf(buf, sizeof(buf), "%.4d-%.2d-%.2d", y, m, d);
    /* Append lot code if present */
    if (s->buf_len > 6) {
        char lot[32];
        int j = 0;
        for (int i = 6; i < s->buf_len && j < 31; i++) {
            if (s->buf[i] >= 0x20 && s->buf[i] < 0x7f)
                lot[j++] = s->buf[i];
        }
        lot[j] = '\0';
        while (j > 0 && (lot[j-1] == '\0' || lot[j-1] == ' ')) lot[--j] = '\0';
        if (j > 0) {
            int blen = strlen(buf);
            snprintf(buf + blen, sizeof(buf) - blen, " lot %s", lot);
        }
    }
    xfp_annotate(di, s, "Manufacturing date", buf,
                 s->cnt - s->buf_len + 1, s->cnt);
}

/* Lower memory handler dispatch table */
typedef struct {
    int end_idx;
    void (*handler)(struct srd_decoder_inst *, xfp_state *);
} xfp_map_entry;

static const xfp_map_entry xfp_map_lower_memory[] = {
    {0,   xfp_handle_module_id},
    {1,   xfp_handle_signal_cc},
    {57,  xfp_handle_alarm_warnings},
    {59,  xfp_handle_vps},
    {69,  NULL},  /* ignore */
    {71,  xfp_handle_ber},
    {75,  xfp_handle_wavelength_cr},
    {79,  xfp_handle_fec_cr},
    {95,  xfp_handle_int_ctrl},
    {109, xfp_handle_ad_readout},
    {111, xfp_handle_gcs},
    {117, NULL},  /* ignore */
    {118, NULL},  /* ignore */
    {122, NULL},  /* ignore */
    {126, NULL},  /* ignore */
    {127, xfp_handle_page_select},
    {-1,  NULL},
};

static const xfp_map_entry xfp_map_high_table_1[] = {
    {128, xfp_handle_module_id},
    {129, xfp_handle_ext_module_id},
    {130, xfp_handle_connector},
    {138, xfp_handle_transceiver},
    {139, xfp_handle_serial_encoding},
    {140, xfp_handle_br_min},
    {141, xfp_handle_br_max},
    {142, NULL},  /* link_length_smf - handled below */
    {143, NULL},  /* link_length_e50 */
    {144, NULL},  /* link_length_50um */
    {145, NULL},  /* link_length_625um */
    {146, NULL},  /* link_length_copper */
    {147, xfp_handle_device_tech},
    {163, 0},     /* vendor - special */
    {164, xfp_handle_cdr},
    {167, xfp_handle_vendor_oui},
    {183, 0},     /* vendor_pn - special */
    {185, 0},     /* vendor_rev - special */
    {187, 0},     /* wavelength - special */
    {189, 0},     /* wavelength_tolerance - special */
    {190, xfp_handle_max_case_temp},
    {191, NULL},  /* ignore */
    {195, xfp_handle_power_supply},
    {211, 0},     /* vendor_sn - special */
    {219, 0},     /* manuf_date - special */
    {220, xfp_handle_diag_mon},
    {221, xfp_handle_enhanced_opts},
    {222, xfp_handle_aux_mon},
    {223, NULL},  /* ignore */
    {255, 0},     /* maybe_ascii - special */
    {-1,  NULL},
};

static void xfp_dispatch_lower_memory(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    for (int i = 0; xfp_map_lower_memory[i].end_idx != -1; i++) {
        if (s->cnt == xfp_map_lower_memory[i].end_idx) {
            if (xfp_map_lower_memory[i].handler)
                xfp_map_lower_memory[i].handler(di, s);
            s->buf_len = 0;
            return;
        }
    }
}

static void xfp_dispatch_high_table_1(struct srd_decoder_inst *di, xfp_state *s)
{
    (void)di;

    /* Handle special cases first */
    if (s->cnt == 142) {
        xfp_handle_link_length(di, s, "Link length (SMF)", 1);
        s->buf_len = 0; return;
    }
    if (s->cnt == 143) {
        xfp_handle_link_length(di, s, "Link length (extended, 50μm MMF)", 2);
        s->buf_len = 0; return;
    }
    if (s->cnt == 144) {
        xfp_handle_link_length(di, s, "Link length (50μm MMF)", 1);
        s->buf_len = 0; return;
    }
    if (s->cnt == 145) {
        xfp_handle_link_length(di, s, "Link length (62.5μm MMF)", 1);
        s->buf_len = 0; return;
    }
    if (s->cnt == 146) {
        xfp_handle_link_length(di, s, "Link length (copper)", 2);
        s->buf_len = 0; return;
    }
    if (s->cnt == 163) {
        xfp_handle_vendor(di, s, "Vendor");
        s->buf_len = 0; return;
    }
    if (s->cnt == 183) {
        xfp_handle_vendor(di, s, "Vendor part number");
        s->buf_len = 0; return;
    }
    if (s->cnt == 185) {
        xfp_handle_vendor(di, s, "Vendor revision");
        s->buf_len = 0; return;
    }
    if (s->cnt == 187) {
        xfp_handle_wavelength(di, s);
        s->buf_len = 0; return;
    }
    if (s->cnt == 189) {
        xfp_handle_wavelength_tolerance(di, s);
        s->buf_len = 0; return;
    }
    if (s->cnt == 211) {
        xfp_handle_vendor(di, s, "Vendor serial number");
        s->buf_len = 0; return;
    }
    if (s->cnt == 219) {
        xfp_handle_manuf_date(di, s);
        s->buf_len = 0; return;
    }
    if (s->cnt == 255) {
        /* maybe_ascii - show ASCII if possible */
        for (int i = 0; i < s->buf_len; i++) {
            if (s->buf[i] >= 0x20 && s->buf[i] < 0x7f) {
                int cnt = s->cnt - s->buf_len + 1 + i;
                char c[2] = {(char)s->buf[i], '\0'};
                xfp_annotate(di, s, "Vendor ID", c, cnt, cnt);
            }
        }
        s->buf_len = 0; return;
    }

    for (int i = 0; xfp_map_high_table_1[i].end_idx != -1; i++) {
        if (s->cnt == xfp_map_high_table_1[i].end_idx) {
            if (xfp_map_high_table_1[i].handler)
                xfp_map_high_table_1[i].handler(di, s);
            s->buf_len = 0;
            return;
        }
    }
}

static void xfp_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    xfp_state *s = (xfp_state *)c_decoder_get_private(di);
    if (!s)
        return;

    s->ss = start_sample;
    s->es = end_sample;

    if (s->state == XFP_IDLE) {
        if (strcmp(cmd, "START") == 0)
            s->state = XFP_GET_SLAVE_ADDR;
    } else if (s->state == XFP_GET_SLAVE_ADDR) {
        if (strcmp(cmd, "ADDRESS READ") == 0) {
            /* XFP slave address = 0x50 (7-bit) */
            s->state = XFP_READ_REGS;
        } else if (strcmp(cmd, "ADDRESS WRITE") == 0) {
            /* Write operation to set register offset */
            s->state = XFP_READ_REGS;
        }
    } else if (s->state == XFP_READ_REGS) {
        if (strcmp(cmd, "DATA READ") == 0) {
            uint8_t databyte = (n_fields > 0) ? fields[0].u8 : 0;
            s->cnt++;
            if (s->cnt < 256) {
                s->sn[s->cnt][0] = start_sample;
                s->sn[s->cnt][1] = end_sample;
            }
            if (s->buf_len < 256)
                s->buf[s->buf_len++] = databyte;

            if (s->cnt < 0x80) {
                xfp_dispatch_lower_memory(di, s);
            } else if (s->cnt < 0x0100 && s->cur_highmem_page == 0x01) {
                xfp_dispatch_high_table_1(di, s);
            }
        } else if (strcmp(cmd, "DATA WRITE") == 0) {
            /* Write data to set register offset - just track */
        } else if (strcmp(cmd, "STOP") == 0) {
            s->state = XFP_IDLE;
        } else if (strcmp(cmd, "START REPEAT") == 0) {
            /* Continue reading */
        }
    }
}

static void xfp_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(xfp_state)));
    }
    xfp_state *s = (xfp_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(xfp_state));
    s->state = XFP_IDLE;
    s->cnt = -1;
}

static void xfp_start(struct srd_decoder_inst *di)
{
    xfp_state *s = (xfp_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "xfp");
}

static void xfp_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void xfp_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder xfp_c_decoder = {
    .id = "xfp_c",
    .name = "XFP(C)",
    .longname = "10 Gigabit Small Form Factor Pluggable Module (XFP) (C)",
    .desc = "XFP I²C management interface structures/protocol. (C implementation)",
    .license = "gplv3+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = xfp_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = xfp_ann_rows,
    .inputs = xfp_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = xfp_tags,
    .num_tags = 1,
    .reset = xfp_reset,
    .start = xfp_start,
    .decode = xfp_decode,
    .destroy = xfp_destroy,
    .decode_upper = xfp_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &xfp_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}