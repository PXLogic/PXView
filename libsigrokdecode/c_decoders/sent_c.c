#include "libsigrokdecode.h"
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SENT SAE J2716 decoder - Single Edge Nibble Transmission */

#define CH_DATA 0

/* CRC4 table */
static const int crc4Table[16] = {
    0, 13, 7, 10, 14, 3, 9, 4, 1, 12, 6, 11, 15, 2, 8, 5
};

/* CRC6 table */
static const int crc6Table[64] __attribute__((unused)) = {
     0, 25, 50, 43, 61, 36, 15, 22, 35, 58, 17,  8, 30,  7, 44, 53,
    31,  6, 45, 52, 34, 59, 16,  9, 60, 37, 14, 23,  1, 24, 51, 42,
    62, 39, 12, 21,  3, 26, 49, 40, 29,  4, 47, 54, 32, 57, 18, 11,
    33, 56, 19, 10, 28,  5, 46, 55,  2, 27, 48, 41, 63, 38, 13, 20
};

#define CRC4_INIT 5
#define CRC6_INIT 0x15
#define CRC6_POLY 0x59

#define DATA_BIT 4
#define START_BIT 8

#define PP_TICKS 769

enum sent_ann {
    ANN_TICK = 0,
    ANN_CAL,
    ANN_SC,
    ANN_DATA,
    ANN_CRC,
    ANN_PAUSE,
    ANN_SERIAL_START,
    ANN_SERIAL_DATA,
    ANN_SERIAL_ID,
    ANN_SERIAL_CONFIG,
    ANN_SERIAL_FRAME,
    ANN_SERIAL_SYNC,
    ANN_SERIAL_CRC,
    ANN_WARNING,
    NUM_ANN,
};

enum sent_state {
    STATE_SYNC_ST,
    STATE_DECODE_ST,
    STATE_RESYNC_ST,
};

enum serial_state {
    SER_SYNC_ST,
    SER_SYNCED_ST,
    SER_PATTERN_ST,
    SER_DECODE_ST,
};

struct sent_priv {
    uint64_t samplerate;
    int state;
    int serialState;

    int dataSize;       /* number of data nibbles + SC + CRC (+ pause if enabled) */
    int dataSizeOpt;    /* user option value */
    int tickPer;        /* tick period in us */
    int tickTol;        /* tick tolerance in % */
    int pausePulse;     /* 0=off, 1=on */
    int pauseTmp;       /* temporary pause pulse flag */
    int crcMode;        /* 0=legacy, 1=recommended */
    int serialMode;     /* 0=short, 1=enhanced, 2=off */
    int crcPos;         /* position of CRC nibble */

    uint64_t calPeriod;       /* calibration period in samples */
    uint64_t tickPeriod;      /* tick period in samples */
    uint64_t maxPausePulse;   /* max pause pulse in samples */
    uint64_t minCalTicks;
    uint64_t maxCalTicks;

    int pulseCtr;
    int statusNibble;
    int crc;

    /* Serial decoding - short */
    int serialCtr;
    int serialNibble;
    int serialCrc;
    uint64_t serialNibbleStart;

    /* Serial decoding - enhanced */
    int serialBit3;
    int serialBit2;
    int serialNibbleID;
    int serialCrcRx;
    uint64_t serialNibbleIDStart;
    uint64_t crcStart;
    uint64_t crcEnd;

    /* Calibration tracking */
    uint64_t calStart;
    uint64_t calEnd;

    int out_ann;
};

static struct srd_channel sent_channels[] = {
    { "data", "data", "Nibble Data", 0, SRD_CHANNEL_SDATA, "dec_sent_chan_data" },
};

static struct srd_decoder_option sent_options[] = {
    { "dataSize", "dec_sent_opt_datasize", "Number of data nibbles", NULL, NULL },
    { "tickPer", "dec_sent_opt_tickper", "Clock period (us)", NULL, NULL },
    { "tickTol", "dec_sent_opt_ticktol", "Tick tolerance (%)", NULL, NULL },
    { "pausePulse", "dec_sent_opt_pausepulse", "Pause Pulse", NULL, NULL },
    { "crcMode", "dec_sent_opt_crcmode", "CRC Mode", NULL, NULL },
    { "serialMode", "dec_sent_opt_serialmode", "Serial Decoding", NULL, NULL },
};

static const char *sent_ann_labels[][3] = {
    { "", "Pulse length", "Pulse" },
    { "", "Calibration Pulse", "CAL" },
    { "", "Status&Comm Nibble", "SC" },
    { "", "Nibble Data", "Data" },
    { "", "CRC Nibble", "CRC" },
    { "", "Pause Pulse", "Pause" },
    { "", "Start of Frame", "Start" },
    { "", "Serial Data", "SerData" },
    { "", "Serial ID", "SerID" },
    { "", "Serial Config", "SerCfg" },
    { "", "Serial Frame Bit", "SerFrame" },
    { "", "Serial Sync Bit", "SerSync" },
    { "", "Serial CRC", "SerCRC" },
    { "", "Warning", "Warn" },
};

static const int sent_row_sent_classes[] = { ANN_CAL, ANN_SC, ANN_DATA, ANN_CRC, ANN_PAUSE, -1 };
static const int sent_row_pulses_classes[] = { ANN_TICK, -1 };
static const int sent_row_serial_classes[] = { ANN_SERIAL_START, ANN_SERIAL_DATA, ANN_SERIAL_CRC, -1 };
static const int sent_row_serialid_classes[] = { ANN_SERIAL_ID, ANN_SERIAL_CONFIG, ANN_SERIAL_FRAME, ANN_SERIAL_SYNC, -1 };
static const int sent_row_warnings_classes[] = { ANN_WARNING, -1 };

static const struct srd_c_ann_row sent_ann_rows[] = {
    { "sent", "SENT fast messages", sent_row_sent_classes, 5 },
    { "pulses", "Pulse lengths", sent_row_pulses_classes, 1 },
    { "serial", "SENT slow messages", sent_row_serial_classes, 3 },
    { "serialID", "Enhanced slow message IDs", sent_row_serialid_classes, 4 },
    { "warnings", "Warnings", sent_row_warnings_classes, 1 },
};

static const char *sent_inputs[] = { "logic" };
static const char *sent_outputs[] = { "sent" };
static const char *sent_tags[] = { "Embedded/automotive" };

static void sent_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(struct sent_priv)));
    struct sent_priv *s = (struct sent_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct sent_priv));
    s->state = STATE_SYNC_ST;
    s->serialState = SER_SYNC_ST;
    s->out_ann = -1;
}

static void sent_start(struct srd_decoder_inst *di)
{
    struct sent_priv *s = (struct sent_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "sent");
}

static void sent_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    struct sent_priv *s = (struct sent_priv *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void sent_decode(struct srd_decoder_inst *di)
{
    struct sent_priv *s = (struct sent_priv *)c_decoder_get_private(di);
    if (!s->samplerate)
        s->samplerate = c_samplerate(di);
    if (!s->samplerate)
        return;

    /* Read options */
    s->dataSizeOpt = (int)c_opt_int(di, "dataSize", 6);
    s->tickPer = (int)c_opt_int(di, "tickPer", 3);
    s->tickTol = (int)c_opt_int(di, "tickTol", 5);

    const char *pp_str = c_opt_str(di, "pausePulse", "on");
    s->pausePulse = (strcmp(pp_str, "off") == 0) ? 0 : 1;
    s->pauseTmp = s->pausePulse;

    const char *crc_str = c_opt_str(di, "crcMode", "recommended");
    s->crcMode = (strcmp(crc_str, "legacy") == 0) ? 0 : 1;

    const char *ser_str = c_opt_str(di, "serialMode", "off");
    if (strcmp(ser_str, "short") == 0)
        s->serialMode = 0;
    else if (strcmp(ser_str, "enhanced") == 0)
        s->serialMode = 1;
    else
        s->serialMode = 2;

    s->dataSize = s->dataSizeOpt + 2; /* +2 for SC and CRC */
    s->crcPos = s->dataSize;
    if (s->pauseTmp != 0)
        s->dataSize += 1;

    /* Calculate timing */
    s->calPeriod = (uint64_t)round(56.0 * s->tickPer * s->samplerate / 1000000.0);
    s->maxPausePulse = (uint64_t)round((double)PP_TICKS * s->tickPer * s->samplerate / 1000000.0);
    s->minCalTicks = (uint64_t)round(s->calPeriod * (1.0 - s->tickTol / 100.0));
    s->maxCalTicks = (uint64_t)round(s->calPeriod * (1.0 + s->tickTol / 100.0));

    s->state = STATE_SYNC_ST;
    s->serialState = SER_SYNC_ST;
    s->pulseCtr = 0;
    s->statusNibble = -1;
    s->serialCtr = 0;

    /* Wait for first falling edge */
    int ret = c_wait(di, CW_F(CH_DATA), CW_END);
    if (ret != SRD_OK)
        return;

    uint64_t last_samplenum = di_samplenum(di);

    while (1) {
        ret = c_wait(di, CW_F(CH_DATA), CW_END);
        if (ret != SRD_OK)
            return;

        int storeCal = 0;
        int nibble = -1;
        uint64_t period = di_samplenum(di) - last_samplenum;
        const char *fault = "OK";

        if (s->state == STATE_SYNC_ST) {
            if (period <= s->maxCalTicks) {
                if (period >= s->minCalTicks) {
                    s->pauseTmp = 2;
                    s->pulseCtr = 0;
                    s->state = STATE_DECODE_ST;
                    storeCal = 1;
                } else {
                    if (s->pulseCtr == s->dataSize) {
                        fault = "TOO_MANY_PULSES";
                        s->state = STATE_RESYNC_ST;
                    } else {
                        s->pulseCtr++;
                    }
                }
            } else {
                if ((s->pauseTmp == 0) || (period > s->maxPausePulse)) {
                    fault = "PULSE_TOO_LONG";
                    s->state = STATE_RESYNC_ST;
                } else {
                    s->pauseTmp = 0;
                }
            }
        } else {
            /* DECODE state */
            if (period > s->maxPausePulse) {
                fault = "PULSE_TOO_LONG";
                s->state = STATE_RESYNC_ST;
            } else if ((s->pauseTmp != 1) && (period >= s->minCalTicks) && (period <= s->maxCalTicks)) {
                storeCal = 1;
                if (s->pulseCtr == s->dataSize) {
                    /* Message complete - serial decoding handled inline */
                } else {
                    if (s->pauseTmp != 2) {
                        fault = "TOO_FEW_PULSES";
                    }
                    s->pauseTmp = 2;
                }
                s->pulseCtr = 0;
            } else {
                s->pauseTmp = 0;
                if (s->pulseCtr == s->dataSize) {
                    fault = "TOO_MANY_PULSES";
                    s->state = STATE_RESYNC_ST;
                } else {
                    s->pulseCtr++;
                    /* Decode nibble */
                    nibble = (int)round((double)period / (double)s->tickPeriod);
                    if (nibble < 12) {
                        fault = "DATA_TOO_SMALL";
                        s->state = STATE_RESYNC_ST;
                        nibble = -1;
                    } else if ((s->pausePulse == 0) || (s->pulseCtr < s->dataSize)) {
                        if (nibble > 27) {
                            if (period > s->maxCalTicks) {
                                fault = "PULSE_TOO_LONG";
                            } else {
                                fault = "DATA_TOO_LARGE";
                                nibble = -1;
                            }
                            s->state = STATE_RESYNC_ST;
                        } else {
                            nibble -= 12;
                            if (s->pulseCtr == 1) {
                                s->statusNibble = nibble;
                                s->crc = CRC4_INIT;
                            } else {
                                int tmp = crc4Table[s->crc];
                                if (s->pulseCtr == s->crcPos) {
                                    s->pauseTmp = 1;
                                    int crcRx = nibble;
                                    if (s->crcMode != 0)
                                        s->crc = tmp;
                                    if (s->crc != crcRx)
                                        fault = "CRC_ERROR";
                                } else {
                                    s->crc = tmp ^ nibble;
                                }
                            }
                        }
                    }
                }
            }
        }

        /* Display CAL */
        if (storeCal != 0) {
            s->calPeriod = period;
            s->tickPeriod = (uint64_t)round((double)s->calPeriod / 56.0);
            s->maxPausePulse = (uint64_t)round((double)s->calPeriod * PP_TICKS / 56);
            s->calStart = last_samplenum;
            s->calEnd = di_samplenum(di);
        }

        /* Display tick pulse length */
        {
            double perMicroSec = (double)period * 1000000.0 / (double)s->samplerate;
            char tick_long[32], tick_short[8];
            snprintf(tick_long, sizeof(tick_long), "%4.1f\xC2\xB5s", perMicroSec);
            snprintf(tick_short, sizeof(tick_short), "%3.0f", perMicroSec);
            c_put(di, last_samplenum, di_samplenum(di), s->out_ann, ANN_TICK, tick_long, tick_short);
        }

        /* Display nibbles */
        if ((storeCal == 0) && (nibble != -1)) {
            if (s->pulseCtr == 1) {
                char txt[64];
                snprintf(txt, sizeof(txt), "Status&Comm 0x%01X", nibble);
                c_put_v(di, last_samplenum, di_samplenum(di), s->out_ann, ANN_SC, nibble, txt, "SC 0x%01X", "%01X");
            } else if (s->pulseCtr == s->crcPos) {
                char txt[32];
                snprintf(txt, sizeof(txt), "CRC 0x%01X", nibble);
                c_put_v(di, last_samplenum, di_samplenum(di), s->out_ann, ANN_CRC, nibble, txt, "CRC %01X");
            } else if ((s->pausePulse != 0) && (s->pulseCtr == s->dataSize)) {
                c_put(di, last_samplenum, di_samplenum(di), s->out_ann, ANN_PAUSE, "Pause Pulse", "Pause", "P");
            } else {
                int dataIndex = s->pulseCtr - 2;
                char txt[32];
                snprintf(txt, sizeof(txt), "DATA_%d 0x%01X", dataIndex, nibble);
                char short_txt[8];
                snprintf(short_txt, sizeof(short_txt), "%01X", nibble);
                c_put_v(di, last_samplenum, di_samplenum(di), s->out_ann, ANN_DATA, nibble, txt, "0x%01X", short_txt);
            }
        }

        /* Display faults */
        if (strcmp(fault, "OK") != 0) {
            if (strcmp(fault, "CRC_ERROR") == 0) {
                char txt[64];
                snprintf(txt, sizeof(txt), "CRC_ERROR");
                c_put(di, last_samplenum, di_samplenum(di), s->out_ann, ANN_WARNING, txt, "CRC_ERROR", "CRC");
            } else {
                c_put(di, last_samplenum, di_samplenum(di), s->out_ann, ANN_WARNING, fault, "Fault", "F");
            }
            s->serialState = SER_SYNC_ST;
        }

        /* Display CAL annotation */
        if (storeCal != 0) {
            c_put(di, last_samplenum, di_samplenum(di), s->out_ann, ANN_CAL,
                "Calibration Pulse", "Calibration", "CAL", "C");
        }

        /* Handle resync */
        if (s->state == STATE_RESYNC_ST) {
            s->pulseCtr = 0;
            s->state = STATE_SYNC_ST;
            s->pauseTmp = 1;
        }

        if (s->pausePulse == 0)
            s->pauseTmp = 0;

        last_samplenum = di_samplenum(di);
    }
}

static void sent_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder sent_c_decoder = {
    .id = "sent_c",
    .name = "SENT(C)",
    .longname = "Single Edge Nibble Transmission(C)",
    .desc = "Single line, one-directional, nibble based protocol.(C implementation)",
    .license = "gplv2+",
    .channels = sent_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = sent_options,
    .num_options = 6,
    .num_annotations = NUM_ANN,
    .ann_labels = sent_ann_labels,
    .num_annotation_rows = 5,
    .annotation_rows = sent_ann_rows,
    .inputs = sent_inputs,
    .num_inputs = 1,
    .outputs = sent_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = sent_tags,
    .num_tags = 1,
    .reset = sent_reset,
    .start = sent_start,
    .decode = sent_decode,
    .destroy = sent_destroy,
    .state_size = 0,
    .metadata = sent_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    sent_options[0].def = g_variant_new_int64(6);
    GSList *ds_vals = NULL;
    for (int i = 1; i <= 8; i++)
        ds_vals = g_slist_append(ds_vals, g_variant_new_int64(i));
    sent_options[0].values = ds_vals;

    sent_options[1].def = g_variant_new_int64(3);
    GSList *tp_vals = NULL;
    for (int i = 2; i <= 100; i++)
        tp_vals = g_slist_append(tp_vals, g_variant_new_int64(i));
    sent_options[1].values = tp_vals;

    sent_options[2].def = g_variant_new_int64(5);
    GSList *tt_vals = NULL;
    for (int i = 0; i <= 20; i++)
        tt_vals = g_slist_append(tt_vals, g_variant_new_int64(i));
    sent_options[2].values = tt_vals;

    sent_options[3].def = g_variant_new_string("on");
    GSList *pp_vals = NULL;
    pp_vals = g_slist_append(pp_vals, g_variant_new_string("off"));
    pp_vals = g_slist_append(pp_vals, g_variant_new_string("on"));
    sent_options[3].values = pp_vals;

    sent_options[4].def = g_variant_new_string("recommended");
    GSList *cm_vals = NULL;
    cm_vals = g_slist_append(cm_vals, g_variant_new_string("legacy"));
    cm_vals = g_slist_append(cm_vals, g_variant_new_string("recommended"));
    sent_options[4].values = cm_vals;

    sent_options[5].def = g_variant_new_string("off");
    GSList *sm_vals = NULL;
    sm_vals = g_slist_append(sm_vals, g_variant_new_string("short"));
    sm_vals = g_slist_append(sm_vals, g_variant_new_string("enhanced"));
    sm_vals = g_slist_append(sm_vals, g_variant_new_string("off"));
    sent_options[5].values = sm_vals;

    return &sent_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}