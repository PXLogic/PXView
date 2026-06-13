/*
 * ccd_c.c — CCD (Chrysler Collision Detection) Data Bus decoder (C implementation)
 *
 * CCD is a Chrysler vehicle single-wire serial bus protocol
 * with fixed 7812.5 bps UART encoding.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glib.h>
#include "libsigrokdecode.h"

static void fmt_binary(uint8_t val, char *buf, int bufsize) {
    if (bufsize < 9) { buf[0] = '\0'; return; }
    for (int i = 7; i >= 0; i--) buf[7 - i] = (val & (1 << i)) ? '1' : '0';
    buf[8] = '\0';
}

enum ccd_ann {
    ANN_BUS_BITS = 0,
    ANN_BUS_BYTES,
    ANN_IDLE,
    ANN_FRAME_ERROR,
    ANN_CHECKSUM,
    ANN_BUS_DECODED,
    ANN_BUS_MESSAGE,
    NUM_ANN,
};

enum ccd_uart_state {
    UART_WAIT_START = 0,
    UART_GET_DATA,
    UART_GET_STOP,
};

enum ccd_idle_state {
    IDLE_IDLE = 0,
    IDLE_BUSY,
};

typedef struct {
    uint64_t samplerate;
    uint64_t bit_width;
    int out_ann;
    int out_python;

    /* IDLE/BUSY state */
    int idle;              /* 0=IDLE, 1=BUSY */
    uint64_t idlestart;
    uint64_t busystart;
    int oldbus;

    /* UART state */
    int uart_state;        /* WAIT_START, GET_DATA, GET_STOP */
    int databit;
    uint8_t databyte;
    uint64_t framestart;
    uint64_t waitfortime;

    /* Message collection */
    uint8_t ccd_message[256];
    int ccd_msg_len;
    int errors;

    /* VIN cache */
    char vin[18];

    /* Options */
    int opt_ignoreerrors;
    int opt_invert_bus;
    int opt_units;         /* 0=metric, 1=imperial, 2=both, 3=native */
} ccd_state;

static struct srd_channel ccd_channels[] = {
    { "bus", "bus", "CCD bidirectional shared data bus", 0, SRD_CHANNEL_SDATA, "dec_ccd_chan_bus" },
};

static struct srd_decoder_option ccd_options[] = {
    { "ignoreerrors", NULL, "Ignore checksum and frame errors", NULL, NULL },
    { "invert_bus", NULL, "Invert bus?", NULL, NULL },
    { "units", NULL, "Show metric/imperial/both/native units", NULL, NULL },
};

static const char *ccd_ann_labels[][3] = {
    { "", "bus-bits", "Bus data bits" },
    { "", "bus-bytes", "Bus data bytes" },
    { "", "idle", "Bus idle" },
    { "", "frame-error", "Frame errors" },
    { "", "checksum", "Message checksum errors" },
    { "", "bus-decoded", "Decoded bus message" },
    { "", "bus-message", "Message bytes" },
};

static const int ccd_row_bits_classes[] = { ANN_BUS_BITS, -1 };
static const int ccd_row_idle_classes[] = { ANN_IDLE, -1 };
static const int ccd_row_warnings_classes[] = { ANN_FRAME_ERROR, ANN_CHECKSUM, -1 };
static const int ccd_row_data_classes[] = { ANN_BUS_BYTES, -1 };
static const int ccd_row_message_classes[] = { ANN_BUS_MESSAGE, -1 };
static const int ccd_row_decoded_classes[] = { ANN_BUS_DECODED, -1 };

static const struct srd_c_ann_row ccd_ann_rows[] = {
    { "a-bus-bits", "Bus bits", ccd_row_bits_classes, 1 },
    { "a-idle", "Idle", ccd_row_idle_classes, 1 },
    { "a-bus-warnings", "Bus warnings", ccd_row_warnings_classes, 2 },
    { "a-bus-data", "Bus bytes", ccd_row_data_classes, 1 },
    { "a-bus-message", "Message bytes", ccd_row_message_classes, 1 },
    { "a-bus-decoded", "Message decoded", ccd_row_decoded_classes, 1 },
};

static const char *ccd_inputs[] = { "logic" };
static const char *ccd_outputs[] = { "ccd" };
static const char *ccd_tags[] = { "Automotive" };

static void ccd_decode_message(struct srd_decoder_inst *di, ccd_state *s)
{
    if (s->ccd_msg_len < 2)
        return;

    char ann_text[256];
    uint8_t *m = s->ccd_message;

    if (m[0] == 0x24 && s->ccd_msg_len >= 4) {
        /* Speed */
        char kmh[16], mph[16];
        snprintf(kmh, sizeof(kmh), "%d", m[2]);
        snprintf(mph, sizeof(mph), "%d", m[1]);
        char t2[64];
        snprintf(t2, sizeof(t2), "%skm/h", kmh);
        snprintf(ann_text, sizeof(ann_text), "Speed: %s km/h, %s mph", kmh, mph);
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_DECODED,
                  ann_text, t2);
    } else if (m[0] == 0xE4 && s->ccd_msg_len >= 4) {
        /* RPM + MAP */
        int rpm = m[1] * 32;
        int mapsensor = (int)(round(m[2] * 0.41));
        char rpm_str[16], map_str[16];
        snprintf(rpm_str, sizeof(rpm_str), "%d", rpm);
        snprintf(map_str, sizeof(map_str), "%d", mapsensor);
        char t2[64], t3[64];
        snprintf(t2, sizeof(t2), "RPM=%s,MAP=%s", rpm_str, map_str);
        snprintf(t3, sizeof(t3), "R%s,M%s", rpm_str, map_str);
        snprintf(ann_text, sizeof(ann_text), "RPM: %s rpm, MAP: %s kPa", rpm_str, map_str);
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_DECODED,
                  ann_text, t2, t3);
    } else if (m[0] == 0x6D && s->ccd_msg_len >= 4) {
        /* VIN character */
        int pos = m[1];
        char ch = (char)m[2];
        if (pos >= 1 && pos <= 17)
            s->vin[pos - 1] = ch;
        char t2[64], t3[32];
        snprintf(t2, sizeof(t2), "VIN[%d]:%c,VIN:%s", pos, ch, s->vin);
        snprintf(t3, sizeof(t3), "VIN[%d]:%c", pos, ch);
        snprintf(ann_text, sizeof(ann_text), "VIN character %d: %c, VIN: %s", pos, ch, s->vin);
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_DECODED,
                  ann_text, t2, t3);
    } else if (m[0] == 0x86 && s->ccd_msg_len >= 4) {
        /* Door lock/alarm */
        char t2[16], t3[16];
        if (m[1] == 0x80) {
            snprintf(t2, sizeof(t2), "DDM:%x", m[2]);
            snprintf(t3, sizeof(t3), "DDM:%x", m[2]);
            snprintf(ann_text, sizeof(ann_text), "from DDM: 0x%02X", m[2]);
        } else if (m[1] == 0x81) {
            snprintf(t2, sizeof(t2), "PDM:%x", m[2]);
            snprintf(t3, sizeof(t3), "PDM:%x", m[2]);
            snprintf(ann_text, sizeof(ann_text), "from PDM: 0x%02X", m[2]);
        } else {
            snprintf(t2, sizeof(t2), "UNK:%x%x", m[1], m[2]);
            snprintf(t3, sizeof(t3), "UNK:%x%x", m[1], m[2]);
            snprintf(ann_text, sizeof(ann_text), "unknown DDM/PDM: 0x%02X 0x%02X", m[1], m[2]);
        }
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_DECODED,
                  ann_text, t2, t3);
    } else if (m[0] == 0x42 && s->ccd_msg_len >= 4) {
        /* TPS/Cruise */
        char tps[16], cruise[16];
        snprintf(tps, sizeof(tps), "%d", m[1]);
        snprintf(cruise, sizeof(cruise), "%d", m[2]);
        char t2[64];
        snprintf(t2, sizeof(t2), "TPS:%s,CRUISE:%s", tps, cruise);
        snprintf(ann_text, sizeof(ann_text), "TPS: %s, CRUISE: %s", tps, cruise);
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_DECODED,
                  ann_text, t2);
    } else if (m[0] == 0x35 && s->ccd_msg_len >= 4) {
        /* Ignition switch */
        char ignstr[32];
        { char bbuf[9]; fmt_binary(m[1], bbuf, sizeof(bbuf)); snprintf(ignstr, sizeof(ignstr), "%s %d", bbuf, m[2]); }
        char t2[64];
        snprintf(t2, sizeof(t2), "IGN:%s", ignstr);
        snprintf(ann_text, sizeof(ann_text), "Ignition switch: %s", ignstr);
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_DECODED,
                  ann_text, t2, t2);
    } else if (m[0] == 0xA4 && s->ccd_msg_len >= 4) {
        /* Instrument cluster lamps */
        char lampsstr[32];
        { char bbuf[9]; fmt_binary(m[1], bbuf, sizeof(bbuf)); snprintf(lampsstr, sizeof(lampsstr), "%s %02x", bbuf, m[2]); }
        char t2[64];
        snprintf(t2, sizeof(t2), "LAMPS:%s", lampsstr);
        snprintf(ann_text, sizeof(ann_text), "Instrumental cluster lamps: %s", lampsstr);
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_DECODED,
                  ann_text, t2);
    } else if (m[0] == 0x8C && s->ccd_msg_len >= 4) {
        /* Temperatures */
        int engtemp = m[1] - 128;
        int battemp = m[2] - 128;
        char eng_str[16], bat_str[16];
        snprintf(eng_str, sizeof(eng_str), "%d", engtemp);
        snprintf(bat_str, sizeof(bat_str), "%d", battemp);
        char t2[64];
        snprintf(t2, sizeof(t2), "EngTemp=%s,BatTemp=%s", eng_str, bat_str);
        snprintf(ann_text, sizeof(ann_text), "Engine temperature: %s, battery temperature: %s", eng_str, bat_str);
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_DECODED,
                  ann_text, t2);
    } else if (m[0] == 0x84 && s->ccd_msg_len >= 4) {
        /* Increment odometer */
        snprintf(ann_text, sizeof(ann_text), "Increment odometer: %d", 256 * m[1] + m[2]);
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_DECODED,
                  ann_text);
    } else if (m[0] == 0x7B && s->ccd_msg_len >= 3) {
        /* Ambient temperature */
        int ambientf = m[1] - 70;
        int ambientc = (int)floor((ambientf - 32) * 5.0 / 9.0);
        char af_str[16], ac_str[16];
        snprintf(af_str, sizeof(af_str), "%d", ambientf);
        snprintf(ac_str, sizeof(ac_str), "%d", ambientc);
        char t2[64];
        snprintf(t2, sizeof(t2), "AmbTemp:%s", af_str);
        snprintf(ann_text, sizeof(ann_text), "Ambient temperature: %s F (%s C)", af_str, ac_str);
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_DECODED,
                  ann_text, t2);
    } else if (m[0] == 0x82 && s->ccd_msg_len >= 5) {
        /* Steering wheel volume */
        char vol[32];
        snprintf(vol, sizeof(vol), "%x %x %x", m[1], m[2], m[3]);
        char t2[48];
        snprintf(t2, sizeof(t2), "volume: %s", vol);
        snprintf(ann_text, sizeof(ann_text), "Steering wheel volume buttons: %s", vol);
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_DECODED,
                  ann_text, t2);
    } else if (m[0] == 0x8E && s->ccd_msg_len >= 3) {
        /* Doors */
        char doors_str[128] = "Doors:";
        if (m[1] & 0x01) strcat(doors_str, " LeftFront");
        if (m[1] & 0x02) strcat(doors_str, " RightFront");
        if (m[1] & 0x04) strcat(doors_str, " LeftRear");
        if (m[1] & 0x08) strcat(doors_str, " RightRear");
        if (m[1] & 0x10) strcat(doors_str, " Liftgate");
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_DECODED,
                  doors_str);
    } else if (m[0] == 0xFE && s->ccd_msg_len >= 3) {
        /* Panel lamp dim */
        int pwm = (int)floor(m[1] / 2.55);
        snprintf(ann_text, sizeof(ann_text), "Pannel lamp dim: %d%%", pwm);
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_DECODED,
                  ann_text);
    } else if (m[0] == 0xEE && s->ccd_msg_len >= 5) {
        /* Trip distance */
        double trip = ((65536.0 * m[1] + 256.0 * m[2] + m[3]) * 128.0) / 4971.0;
        snprintf(ann_text, sizeof(ann_text), "Trip distance: %.1f km", trip);
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_DECODED,
                  ann_text);
    } else if (m[0] == 0x50 && s->ccd_msg_len >= 3) {
        /* Airbag lamp */
        const char *airbag = (m[1] == 0) ? "OFF" : "PROBLEM";
        snprintf(ann_text, sizeof(ann_text), "Airbag lamp: %s", airbag);
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_DECODED,
                  ann_text);
    } else if (m[0] == 0x25 && s->ccd_msg_len >= 3) {
        /* Fuel level */
        int fuel = (int)floor(m[1] / 2.55);
        snprintf(ann_text, sizeof(ann_text), "Fuel level: %d%%", fuel);
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_DECODED,
                  ann_text);
    } else if (m[0] == 0x0C && s->ccd_msg_len >= 6) {
        /* Voltage + temperatures + oil pressure */
        double voltage = m[1] / 8.0;
        int oil = (int)(m[2] * 3.4473785 + 0.5);
        int engtemp = m[3] - 64;
        int battemp = m[4] - 64;
        char t2[256];
        snprintf(t2, sizeof(t2), "EngTemp=%d,BatTemp=%d", engtemp, battemp);
        snprintf(ann_text, sizeof(ann_text),
                 "Engine temperature: %d C, battery temperature: %d C, battery voltage: %.3g V, oil pressure: %d kPa",
                 engtemp, battemp, voltage, oil);
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_DECODED,
                  ann_text, t2);
    } else if (m[0] == 0xDA && s->ccd_msg_len >= 3) {
        /* Check engine lamp */
        const char *mil = (m[1] & 0x40) ? "PROBLEM" : "OFF";
        snprintf(ann_text, sizeof(ann_text), "Check engine lamp: %s (0x%02X)", mil, m[1]);
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_DECODED,
                  ann_text);
    } else if (m[0] == 0xCE && s->ccd_msg_len >= 6) {
        /* Odometer */
        uint64_t odo = ((uint64_t)m[1] << 24) | ((uint64_t)m[2] << 16) |
                        ((uint64_t)m[3] << 8) | m[4];
        odo /= 4971;
        snprintf(ann_text, sizeof(ann_text), "Odo: %llu km", (unsigned long long)odo);
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_DECODED,
                  ann_text);
    } else if (m[0] == 0x62 && s->ccd_msg_len >= 4) {
        /* Electric doors/mirrors */
        snprintf(ann_text, sizeof(ann_text), "Windows: 0x%02X, Mirrors: 0x%02X", m[1], m[2]);
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_DECODED,
                  ann_text);
    } else {
        /* Unknown message */
        char msg_str[256] = "";
        int pos = 0;
        for (int i = 0; i < s->ccd_msg_len && pos < 200; i++)
            pos += snprintf(msg_str + pos, sizeof(msg_str) - pos, "%x ", m[i]);
        char t2[256];
        snprintf(t2, sizeof(t2), "CCD: %s", msg_str);
        snprintf(ann_text, sizeof(ann_text), "Unknown CCD message: %s", msg_str);
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_DECODED,
                  ann_text, t2, msg_str);
    }

    /* Log whole message */
    {
        char msg_str[256] = "";
        int pos = 0;
        for (int i = 0; i < s->ccd_msg_len && pos < 200; i++)
            pos += snprintf(msg_str + pos, sizeof(msg_str) - pos, "%x ", m[i]);
        c_put(di, s->busystart, s->idlestart - 1, s->out_ann, ANN_BUS_MESSAGE, msg_str);
    }
}

static void ccd_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(ccd_state)));
    ccd_state *s = (ccd_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ccd_state));
    s->out_ann = -1;
    s->out_python = -1;
    s->uart_state = UART_WAIT_START;
    s->idle = IDLE_IDLE;
    s->idlestart = (uint64_t)-1;  /* -1 = no idle period started yet, matches Python's idlestart = -1 */
    s->busystart = (uint64_t)-1;
    s->oldbus = 1;
    s->bit_width = 0;
    strcpy(s->vin, "_________________");
}

static void ccd_start(struct srd_decoder_inst *di)
{
    ccd_state *s = (ccd_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ccd");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "ccd");

    const char *ie_str = c_opt_str(di, "ignoreerrors", "no");
    s->opt_ignoreerrors = (strcmp(ie_str, "yes") == 0) ? 1 : 0;

    const char *ib_str = c_opt_str(di, "invert_bus", "no");
    s->opt_invert_bus = (strcmp(ib_str, "yes") == 0) ? 1 : 0;

    const char *u_str = c_opt_str(di, "units", "native");
    if (strcmp(u_str, "metric") == 0)       s->opt_units = 0;
    else if (strcmp(u_str, "imperial") == 0) s->opt_units = 1;
    else if (strcmp(u_str, "both") == 0)     s->opt_units = 2;
    else                                      s->opt_units = 3;
}

static void ccd_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    ccd_state *s = (ccd_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        s->bit_width = (uint64_t)ceil((double)s->samplerate / 7812.5);
    }
}

static void ccd_decode(struct srd_decoder_inst *di)
{
    ccd_state *s = (ccd_state *)c_decoder_get_private(di);
    if (!s->samplerate) {
        s->samplerate = c_samplerate(di);
        if (s->samplerate > 0)
            s->bit_width = (uint64_t)ceil((double)s->samplerate / 7812.5);
    }
    if (s->samplerate == 0 || s->bit_width == 0)
        return;

    while (1) {
        /* Build wait conditions dynamically, matching Python's waituart/waitidle */
        /* Single combined wait: edge OR skip_uart OR skip_idle */
        int ret = c_wait(di, CW_E(0), CW_OR, CW_SKIP(1), CW_OR, CW_SKIP(1), CW_END);
        if (ret != SRD_OK)
            return;

        int bus = c_pin(di, 0);
        if (s->opt_invert_bus)
            bus = 1 - bus;

        /* IDLE handling */
        if (s->oldbus != bus) {
            /* Bus changed */
            if (s->idle == IDLE_BUSY) {
                s->idlestart = di_samplenum(di);
            } else {
                c_put(di, s->idlestart, di_samplenum(di) - 1, s->out_ann, ANN_IDLE, "Idle", "Id", "I");
                s->idle = IDLE_BUSY;
                s->idlestart = di_samplenum(di);
                s->busystart = di_samplenum(di);
            }
        } else {
            /* Bus not changed */
            if (s->idle == IDLE_BUSY && bus &&
                (di_samplenum(di) - s->idlestart) > s->bit_width * 10) {
                /* Idle for more than 10 bits */
                c_put(di, s->busystart, di_samplenum(di) - 1, s->out_ann, ANN_IDLE, "Busy", "Bsy", "B");
                s->idle = IDLE_IDLE;
                s->idlestart = di_samplenum(di);

                /* BUSY ended, decode collected bytes */
                /* Check checksum */
                if (s->ccd_msg_len >= 2) {
                    int chksum = 0;
                    for (int i = 0; i < s->ccd_msg_len - 1; i++)
                        chksum += s->ccd_message[i];
                    chksum = chksum % 256;
                    if (s->ccd_message[s->ccd_msg_len - 1] != chksum) {
                        c_put(di, s->busystart, di_samplenum(di) - 1, s->out_ann,
                                  ANN_CHECKSUM, "Checksum error", "Bad sum", "CHK");
                        s->errors++;
                    }

                    if (s->errors == 0 || s->opt_ignoreerrors)
                        ccd_decode_message(di, s);
                }

                s->ccd_msg_len = 0;
                s->errors = 0;
            }
        }

        /* UART state machine */
        if (s->uart_state == UART_WAIT_START) {
            if (s->oldbus && !bus) {
                /* Start bit detected */
                s->databit = 0;
                s->databyte = 0;
                s->framestart = di_samplenum(di);
                s->waitfortime = di_samplenum(di) + (uint64_t)ceil(1.5 * s->bit_width);
                s->uart_state = UART_GET_DATA;
                c_put(di, di_samplenum(di), di_samplenum(di) + s->bit_width - 1,
                          s->out_ann, ANN_BUS_BITS, "Start bit", "Start", "S");
            }
        } else if (s->uart_state == UART_GET_DATA) {
            if (di_samplenum(di) >= s->waitfortime) {
                /* Sample data bit */
                s->databyte = s->databyte >> 1;
                if (bus)
                    s->databyte += 128;

                char bit_str[8];
                snprintf(bit_str, sizeof(bit_str), "%d", bus);
                uint64_t bit_ss = di_samplenum(di) - (uint64_t)ceil(s->bit_width / 2.0);
                uint64_t bit_es = di_samplenum(di) + (uint64_t)floor(s->bit_width / 2.0) - 1;
                c_put(di, bit_ss, bit_es, s->out_ann, ANN_BUS_BITS,
                          bit_str, bit_str, bit_str);

                s->waitfortime = di_samplenum(di) + s->bit_width;
                s->databit++;

                if (s->databit == 8) {
                    /* Full byte acquired */
                    if (s->ccd_msg_len < 256)
                        s->ccd_message[s->ccd_msg_len++] = s->databyte;
                    s->uart_state = UART_GET_STOP;

                    char byte_str[8];
                    snprintf(byte_str, sizeof(byte_str), "0x%x", s->databyte);
                    char byte_short[4];
                    snprintf(byte_short, sizeof(byte_short), "%x", s->databyte);
                    c_put(di, s->framestart + s->bit_width,
                              di_samplenum(di) + (uint64_t)floor(s->bit_width / 2.0),
                              s->out_ann, ANN_BUS_BYTES, byte_str, byte_short, byte_short);
                }
            }
        } else if (s->uart_state == UART_GET_STOP) {
            if (di_samplenum(di) >= s->waitfortime) {
                if (!bus) {
                    /* Frame error */
                    s->errors++;
                    c_put(di, s->framestart,
                              di_samplenum(di) + (uint64_t)floor(s->bit_width / 2.0),
                              s->out_ann, ANN_FRAME_ERROR, "Frame error", "Fr.ERR", "FE");
                }
                /* Stop bit annotation */
                uint64_t stop_ss = di_samplenum(di) - (uint64_t)ceil(s->bit_width / 2.0);
                uint64_t stop_es = di_samplenum(di) + (uint64_t)floor(s->bit_width / 2.0) - 1;
                c_put(di, stop_ss, stop_es, s->out_ann, ANN_BUS_BITS,
                          "Stop bit", "Stop", "E");

                s->uart_state = UART_WAIT_START;
                s->idlestart = di_samplenum(di);
            }
        }

        s->oldbus = bus;
    }
}

static void ccd_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ccd_c_decoder = {
    .id = "ccd_c",
    .name = "CCD(C)",
    .longname = "CCD (Chrysler Collision Detection) Data Bus (C)",
    .desc = "CCD (Chrysler Collision Detection) Data Bus (C implementation)",
    .license = "gplv2+",
    .channels = ccd_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = ccd_options,
    .num_options = 3,
    .num_annotations = NUM_ANN,
    .ann_labels = ccd_ann_labels,
    .num_annotation_rows = 6,
    .annotation_rows = ccd_ann_rows,
    .inputs = ccd_inputs,
    .num_inputs = 1,
    .outputs = ccd_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = ccd_tags,
    .num_tags = 1,
    .reset = ccd_reset,
    .start = ccd_start,
    .decode = ccd_decode,
    .destroy = ccd_destroy,
    .state_size = 0,
    .metadata = ccd_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    GVariant *vals_yesno[] = {
        g_variant_new_string("yes"),
        g_variant_new_string("no"),
    };
    GSList *list_yesno = NULL;
    list_yesno = g_slist_append(list_yesno, vals_yesno[0]);
    list_yesno = g_slist_append(list_yesno, vals_yesno[1]);

    ccd_options[0].id = "ignoreerrors";
    ccd_options[0].idn = "dec_ccd_opt_ignoreerrors";
    ccd_options[0].desc = "Ignore checksum and frame errors";
    ccd_options[0].def = g_variant_new_string("no");
    ccd_options[0].values = list_yesno;

    GSList *list_yesno2 = NULL;
    GVariant *vals_yesno2[] = {
        g_variant_new_string("yes"),
        g_variant_new_string("no"),
    };
    list_yesno2 = g_slist_append(list_yesno2, vals_yesno2[0]);
    list_yesno2 = g_slist_append(list_yesno2, vals_yesno2[1]);

    ccd_options[1].id = "invert_bus";
    ccd_options[1].idn = "dec_ccd_opt_invert_bus";
    ccd_options[1].desc = "Invert bus?";
    ccd_options[1].def = g_variant_new_string("no");
    ccd_options[1].values = list_yesno2;

    GVariant *vals_units[] = {
        g_variant_new_string("metric"),
        g_variant_new_string("imperial"),
        g_variant_new_string("both"),
        g_variant_new_string("native"),
    };
    GSList *list_units = NULL;
    for (int i = 0; i < 4; i++)
        list_units = g_slist_append(list_units, vals_units[i]);

    ccd_options[2].id = "units";
    ccd_options[2].idn = "dec_ccd_opt_units";
    ccd_options[2].desc = "Show metric/imperial/both/native units";
    ccd_options[2].def = g_variant_new_string("native");
    ccd_options[2].values = list_units;

    return &ccd_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}