#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* 93xx Microwire EEPROM decoder */

enum {
    ANN_SI_DATA = 0,
    ANN_SO_DATA,
    ANN_WARN,
    NUM_ANN,
};

/* Microwire protocol entry - must match microwire_c.c struct mw_py_entry */
struct mw_py_entry {
    uint64_t ss;
    uint64_t es;
    int si;
    int so;
};

typedef struct {
    int out_ann;
    int addresssize;
    int wordsize;
} eeprom93xx_state;

static const char *eeprom93xx_inputs[] = {"microwire", NULL};
static const char *eeprom93xx_tags[] = {"IC", "Memory", NULL};

static struct srd_decoder_option eeprom93xx_options[] = {
    {"addresssize", "dec_eeprom93xx_opt_addresssize", "Address size", NULL, NULL},
    {"wordsize", "dec_eeprom93xx_opt_wordsize", "Word size", NULL, NULL},
};

static const char *eeprom93xx_ann_labels[][3] = {
    {"", "si-data", "SI data"},
    {"", "so-data", "SO data"},
    {"", "warning", "Warning"},
};

static const int eeprom93xx_row_data_classes[] = {ANN_SI_DATA, ANN_SO_DATA, -1};
static const int eeprom93xx_row_warnings_classes[] = {ANN_WARN, -1};

static const struct srd_c_ann_row eeprom93xx_ann_rows[] = {
    {"data", "Data", eeprom93xx_row_data_classes, 2},
    {"warnings", "Warnings", eeprom93xx_row_warnings_classes, 1},
};

static void eeprom93xx_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    eeprom93xx_state *s = (eeprom93xx_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "microwire") != 0)
        return;

    if (!fields || n_fields < 1 || fields[0].type != C_FIELD_BYTES)
        return;

    const unsigned char *raw = fields[0].bytes.data;
    int raw_len = (int)fields[0].bytes.len;
    int entry_size = 18; /* mw_py_entry: 8(ss) + 8(es) + 1(si) + 1(so) = 18 bytes */

    int num_entries = raw_len / entry_size;
    if (num_entries < 1)
        return;

    /* Reconstruct entries from the packed byte buffer sent by microwire_c */
    struct mw_py_entry *entries = (struct mw_py_entry *)g_malloc(num_entries * sizeof(struct mw_py_entry));
    if (!entries)
        return;
    for (int i = 0; i < num_entries; i++) {
        memcpy(&entries[i].ss, raw + i * entry_size, 8);
        memcpy(&entries[i].es, raw + i * entry_size + 8, 8);
        entries[i].si = raw[i * entry_size + 16];
        entries[i].so = raw[i * entry_size + 17];
    }

    /* Need at least start bit (1) + opcode (2) + address bits */
    if (num_entries < 2 + s->addresssize) {
        c_put(di, start_sample, end_sample, s->out_ann, ANN_WARN,
                  "Not enough packet bits");
        g_free(entries);
        return;
    }

    /* Decode opcode from first 2 SI bits (after start bit) */
    int opcode = (entries[0].si << 1) + entries[1].si;

    if (opcode == 2) {
        /* READ instruction */
        c_put(di, entries[0].ss, entries[1].es, s->out_ann, ANN_SI_DATA,
                  "Read word", "READ");

        /* Decode address (MSb first) */
        int addr = 0;
        for (int b = 0; b < s->addresssize; b++) {
            addr += (entries[2 + b].si << (s->addresssize - b - 1));
        }
        char addr_buf[32];
        snprintf(addr_buf, sizeof(addr_buf), "Address: 0x%04x", addr);
        c_put(di, entries[2].ss, entries[2 + s->addresssize - 1].es,
                  s->out_ann, ANN_SI_DATA, addr_buf);

        /* Get all words from SO data */
        int word_start = 2 + s->addresssize;
        while (num_entries - word_start > 0) {
            if (num_entries - word_start < s->wordsize) {
                c_put(di, entries[word_start].ss,
                          entries[num_entries - 1].es,
                          s->out_ann, ANN_WARN, "Not enough word bits");
                break;
            }
            /* Decode word (MSb first) from SO */
            int word = 0;
            for (int b = 0; b < s->wordsize; b++) {
                word += (entries[word_start + b].so << (s->wordsize - b - 1));
            }
            char word_buf[32];
            snprintf(word_buf, sizeof(word_buf), "Data: 0x%04x", word);
            c_put(di, entries[word_start].ss,
                      entries[word_start + s->wordsize - 1].es,
                      s->out_ann, ANN_SO_DATA, word_buf);
            word_start += s->wordsize;
        }
    } else if (opcode == 1) {
        /* WRITE instruction */
        c_put(di, entries[0].ss, entries[1].es, s->out_ann, ANN_SI_DATA,
                  "Write word", "WRITE");

        /* Decode address */
        int addr = 0;
        for (int b = 0; b < s->addresssize; b++) {
            addr += (entries[2 + b].si << (s->addresssize - b - 1));
        }
        char addr_buf[32];
        snprintf(addr_buf, sizeof(addr_buf), "Address: 0x%04x", addr);
        c_put(di, entries[2].ss, entries[2 + s->addresssize - 1].es,
                  s->out_ann, ANN_SI_DATA, addr_buf);

        /* Get word from SI data */
        if (num_entries < 2 + s->addresssize + s->wordsize) {
            c_put(di, entries[2 + s->addresssize].ss,
                      entries[num_entries - 1].es,
                      s->out_ann, ANN_WARN, "Not enough word bits");
        } else {
            int word = 0;
            for (int b = 0; b < s->wordsize; b++) {
                word += (entries[2 + s->addresssize + b].si << (s->wordsize - b - 1));
            }
            char word_buf[32];
            snprintf(word_buf, sizeof(word_buf), "Data: 0x%04x", word);
            c_put(di, entries[2 + s->addresssize].ss,
                      entries[2 + s->addresssize + s->wordsize - 1].es,
                      s->out_ann, ANN_SI_DATA, word_buf);
        }
    } else if (opcode == 3) {
        /* ERASE instruction */
        c_put(di, entries[0].ss, entries[1].es, s->out_ann, ANN_SI_DATA,
                  "Erase word", "ERASE");

        int addr = 0;
        for (int b = 0; b < s->addresssize; b++) {
            addr += (entries[2 + b].si << (s->addresssize - b - 1));
        }
        char addr_buf[32];
        snprintf(addr_buf, sizeof(addr_buf), "Address: 0x%04x", addr);
        c_put(di, entries[2].ss, entries[2 + s->addresssize - 1].es,
                  s->out_ann, ANN_SI_DATA, addr_buf);
    } else if (opcode == 0) {
        /* Special instructions based on address bits */
        int bit2 = entries[2].si;
        int bit3 = entries[3].si;

        if (bit2 == 1 && bit3 == 1) {
            /* EWEN - Write Enable */
            c_put(di, entries[0].ss,
                      entries[2 + s->addresssize - 1].es,
                      s->out_ann, ANN_SI_DATA,
                      "Write enable", "WEN");
        } else if (bit2 == 0 && bit3 == 0) {
            /* EWDS - Write Disable */
            c_put(di, entries[0].ss,
                      entries[2 + s->addresssize - 1].es,
                      s->out_ann, ANN_SI_DATA,
                      "Write disable", "WDS");
        } else if (bit2 == 1 && bit3 == 0) {
            /* ERAL - Erase All */
            c_put(di, entries[0].ss,
                      entries[2 + s->addresssize - 1].es,
                      s->out_ann, ANN_SI_DATA,
                      "Erase all memory", "Erase all", "ERAL");
        } else if (bit2 == 0 && bit3 == 1) {
            /* WRAL - Write All */
            c_put(di, entries[0].ss,
                      entries[2 + s->addresssize - 1].es,
                      s->out_ann, ANN_SI_DATA,
                      "Write all memory", "Write all", "WRAL");

            /* Get word from SI data */
            if (num_entries < 2 + s->addresssize + s->wordsize) {
                c_put(di, entries[2 + s->addresssize].ss,
                          entries[num_entries - 1].es,
                          s->out_ann, ANN_WARN, "Not enough word bits");
            } else {
                int word = 0;
                for (int b = 0; b < s->wordsize; b++) {
                    word += (entries[2 + s->addresssize + b].si << (s->wordsize - b - 1));
                }
                char word_buf[32];
                snprintf(word_buf, sizeof(word_buf), "Data: 0x%04x", word);
                c_put(di, entries[2 + s->addresssize].ss,
                          entries[2 + s->addresssize + s->wordsize - 1].es,
                          s->out_ann, ANN_SI_DATA, word_buf);
            }
        }
    }
    g_free(entries);
}

static void eeprom93xx_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(eeprom93xx_state)));
    eeprom93xx_state *s = (eeprom93xx_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(eeprom93xx_state));
    s->addresssize = 8;
    s->wordsize = 16;
}

static void eeprom93xx_start(struct srd_decoder_inst *di)
{
    eeprom93xx_state *s = (eeprom93xx_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "eeprom93xx");
    s->addresssize = (int)c_opt_int(di, "addresssize", 8);
    s->wordsize = (int)c_opt_int(di, "wordsize", 16);
}

static void eeprom93xx_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void eeprom93xx_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder eeprom93xx_c_decoder = {
    .id = "eeprom93xx_c",
    .name = "93xx EEPROM(C)",
    .longname = "93xx Microwire EEPROM (C)",
    .desc = "93xx series Microwire EEPROM protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = eeprom93xx_options,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = eeprom93xx_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = eeprom93xx_ann_rows,
    .inputs = eeprom93xx_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = eeprom93xx_tags,
    .num_tags = 2,
    .reset = eeprom93xx_reset,
    .start = eeprom93xx_start,
    .decode = eeprom93xx_decode,
    .destroy = eeprom93xx_destroy,
    .decode_upper = eeprom93xx_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    eeprom93xx_options[0].def = g_variant_new_int64(8);
    GSList *addr_vals = NULL;
    addr_vals = g_slist_append(addr_vals, g_variant_new_int64(6));
    addr_vals = g_slist_append(addr_vals, g_variant_new_int64(7));
    addr_vals = g_slist_append(addr_vals, g_variant_new_int64(8));
    addr_vals = g_slist_append(addr_vals, g_variant_new_int64(9));
    addr_vals = g_slist_append(addr_vals, g_variant_new_int64(10));
    eeprom93xx_options[0].values = addr_vals;

    eeprom93xx_options[1].def = g_variant_new_int64(16);
    GSList *word_vals = NULL;
    word_vals = g_slist_append(word_vals, g_variant_new_int64(8));
    word_vals = g_slist_append(word_vals, g_variant_new_int64(16));
    eeprom93xx_options[1].values = word_vals;

    return &eeprom93xx_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}