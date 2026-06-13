#include "libsigrokdecode.h"
#include <glib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SRD_C_DECODER_DLL
#define _srd_err(fmt, ...) fprintf(stderr, "libsigrokdecode: " fmt "\n", ##__VA_ARGS__)
#else
#include "libsigrokdecode-internal.h"
#include "log.h"
#include <Python.h>
#define _srd_err srd_err
#endif

#ifndef SRD_C_DECODER_DLL
extern GSList* pd_list;
#endif

static struct srd_pd_callback* srd_pd_output_callback_find_c(struct srd_session* sess, int output_type)
{
    GSList* l;
    struct srd_pd_callback* cb;

    if (!sess)
        return NULL;

    for (l = sess->callbacks; l; l = l->next) {
        cb = l->data;
        if (cb->output_type == output_type)
            return cb;
    }

    return NULL;
}

SRD_API int c_decoder_put(struct srd_decoder_inst* di,
    uint64_t start_sample, uint64_t end_sample,
    int output_id, struct srd_c_annotation* ann)
{
    struct srd_pd_output* pdo;
    struct srd_pd_callback* cb;
    struct srd_proto_data pdata;
    struct srd_proto_data_annotation pda;

    if (!di)
        return SRD_ERR_ARG;

    GSList* out_list = g_slist_nth(di->pd_output, output_id);
    if (!out_list) {
        _srd_err("C decoder %s submitted invalid output ID %d.",
            di->c_dec_inst->name, output_id);
        return SRD_ERR_ARG;
    }
    pdo = out_list->data;

    pdata.start_sample = start_sample;
    pdata.end_sample = end_sample;
    pdata.pdo = pdo;
    pdata.data = NULL;

    switch (pdo->output_type) {
    case SRD_OUTPUT_ANN:
        if ((cb = srd_pd_output_callback_find_c(di->sess, pdo->output_type))) {
            pdata.data = &pda;
            memset(&pda, 0, sizeof(pda));
            pda.ann_class = ann->ann_class;
            if (ann->ann_class >= 0 && ann->ann_class < (int)g_slist_length(di->decoder->ann_types)) {
                pda.ann_type = GPOINTER_TO_INT(g_slist_nth_data(di->decoder->ann_types, ann->ann_class));
            } else {
                pda.ann_type = ann->ann_class;
            }
            pda.ann_text = ann->ann_text;
            strncpy(pda.str_number_hex, ann->str_number_hex, DECODE_NUM_HEX_MAX_LEN - 1);
            pda.str_number_hex[DECODE_NUM_HEX_MAX_LEN - 1] = '\0';
            pda.numberic_value = ann->numberic_value;
            cb->cb(&pdata, cb->cb_data);
        }
        break;

    case SRD_OUTPUT_PROTO:
        _srd_err("C decoder %s: Use c_proto() for PROTO output "
                 "instead of c_decoder_put().",
            di->c_dec_inst->name);
        return SRD_ERR_ARG;

    case SRD_OUTPUT_BINARY:
        _srd_err("C decoder %s: Use c_decoder_put_binary() for BINARY output "
                 "instead of c_decoder_put().",
            di->c_dec_inst->name);
        return SRD_ERR_ARG;
    case SRD_OUTPUT_LOGIC:
        _srd_err("C decoder %s: Use c_decoder_put_logic() for LOGIC output "
                 "instead of c_decoder_put().",
            di->c_dec_inst->name);
        return SRD_ERR_ARG;
    case SRD_OUTPUT_META:
        if ((cb = srd_pd_output_callback_find_c(di->sess, pdo->output_type))) {
            pdata.data = ann;
            cb->cb(&pdata, cb->cb_data);
        }
        break;

    default:
        _srd_err("C decoder %s submitted invalid output type %d.",
            di->c_dec_inst->name, pdo->output_type);
        return SRD_ERR_ARG;
    }

    return SRD_OK;
}

SRD_API int c_decoder_put_binary(struct srd_decoder_inst* di,
    uint64_t start_sample, uint64_t end_sample,
    int output_id, int bin_class, uint64_t size, const unsigned char* data)
{
    struct srd_pd_output* pdo;
    struct srd_pd_callback* cb;
    struct srd_proto_data pdata;
    struct srd_proto_data_binary pdb;

    if (!di)
        return SRD_ERR_ARG;

    GSList* out_list = g_slist_nth(di->pd_output, output_id);
    if (!out_list) {
        _srd_err("C decoder %s submitted invalid output ID %d.",
            di->c_dec_inst->name, output_id);
        return SRD_ERR_ARG;
    }
    pdo = out_list->data;

    if (pdo->output_type != SRD_OUTPUT_BINARY) {
        _srd_err("C decoder %s: c_decoder_put_binary() called for non-BINARY output type %d.",
            di->c_dec_inst->name, pdo->output_type);
        return SRD_ERR_ARG;
    }

    pdata.start_sample = start_sample;
    pdata.end_sample = end_sample;
    pdata.pdo = pdo;

    if ((cb = srd_pd_output_callback_find_c(di->sess, SRD_OUTPUT_BINARY))) {
        pdb.bin_class = bin_class;
        pdb.size = size;
        pdb.data = data;
        pdata.data = &pdb;
        cb->cb(&pdata, cb->cb_data);
    }

    return SRD_OK;
}

SRD_API int c_decoder_put_logic(struct srd_decoder_inst* di,
    uint64_t start_sample, uint64_t end_sample,
    int output_id, uint32_t channel_mask, const uint8_t* values, int num_channels)
{
    struct srd_pd_output* pdo;
    struct srd_pd_callback* cb;
    struct srd_proto_data pdata;
    struct srd_proto_data_logic pdl;

    if (!di)
        return SRD_ERR_ARG;

    GSList* out_list = g_slist_nth(di->pd_output, output_id);
    if (!out_list) {
        _srd_err("C decoder %s submitted invalid output ID %d.",
            di->c_dec_inst->name, output_id);
        return SRD_ERR_ARG;
    }
    pdo = out_list->data;

    if (pdo->output_type != SRD_OUTPUT_LOGIC) {
        _srd_err("C decoder %s: c_decoder_put_logic() called for non-LOGIC output type %d.",
            di->c_dec_inst->name, pdo->output_type);
        return SRD_ERR_ARG;
    }

    pdata.start_sample = start_sample;
    pdata.end_sample = end_sample;
    pdata.pdo = pdo;

    if ((cb = srd_pd_output_callback_find_c(di->sess, SRD_OUTPUT_LOGIC))) {
        pdl.channel_mask = channel_mask;
        pdl.num_channels = num_channels;
        pdl.values = values;
        pdata.data = &pdl;
        cb->cb(&pdata, cb->cb_data);
    }

    return SRD_OK;
}

SRD_API int c_decoder_wait(struct srd_decoder_inst* di,
    GSList* condition_list, uint64_t* samplenum, uint64_t* matched)
{
    if (!di)
        return SRD_ERR_ARG;

    if (di->runtime && di->runtime->wait)
        return di->runtime->wait(di, condition_list, samplenum, matched);

    return SRD_ERR_ARG;
}

SRD_API uint8_t c_decoder_get_initial_pin(struct srd_decoder_inst* di, int ch)
{
    if (!di || ch < 0)
        return 0xFF;

    if (!di->old_pins_array)
        return 0xFF;

    if (ch >= di->dec_num_channels)
        return 0xFF;

    return di->old_pins_array->data[ch];
}

SRD_API void* c_decoder_get_private(struct srd_decoder_inst* di)
{
    if (!di)
        return NULL;

    if (di->runtime && di->runtime->get_private)
        return di->runtime->get_private(di);

    return di->user_data;
}

SRD_API void c_decoder_set_private(struct srd_decoder_inst* di, void* data)
{
    if (!di)
        return;

    if (di->runtime && di->runtime->set_private) {
        di->runtime->set_private(di, data);
        return;
    }

    di->user_data = data;
}

SRD_API int c_decoder_has_channel(struct srd_decoder_inst* di, int ch)
{
    if (!di || ch < 0 || ch >= di->dec_num_channels)
        return 0;
    return (di->dec_channelmap[ch] >= 0) ? 1 : 0;
}

SRD_API int c_decoder_register_output(struct srd_decoder_inst* di,
    int output_type, const char* proto_id)
{
    struct srd_pd_output* pdo;

    if (!di)
        return SRD_ERR_ARG;

    pdo = g_malloc0(sizeof(struct srd_pd_output));
    pdo->pdo_id = g_slist_length(di->pd_output);
    pdo->output_type = output_type;
    pdo->di = di;
    pdo->proto_id = g_strdup(proto_id ? proto_id : "");

    if (output_type == SRD_OUTPUT_PROTO) {
        _srd_err("C decoder %s: Registering SRD_OUTPUT_PROTO output. "
                 "This output type is for C-to-C decoder stacking only; "
                 "upper-layer Python decoders cannot consume this output.",
            di->c_dec_inst->name);
    }

    di->pd_output = g_slist_append(di->pd_output, pdo);

    return pdo->pdo_id;
}

SRD_API uint64_t c_decoder_get_samplerate(struct srd_decoder_inst* di)
{
    if (!di)
        return 0;
    return di->samplerate;
}

SRD_API int64_t c_decoder_get_option_int(struct srd_decoder_inst* di,
    const char* key, int64_t defval)
{
    if (!di || !di->c_options || !key)
        return defval;
    GVariant* val = g_hash_table_lookup(di->c_options, key);
    if (!val)
        return defval;
    if (g_variant_is_of_type(val, G_VARIANT_TYPE_INT64))
        return g_variant_get_int64(val);
    if (g_variant_is_of_type(val, G_VARIANT_TYPE_UINT64))
        return (int64_t)g_variant_get_uint64(val);
    if (g_variant_is_of_type(val, G_VARIANT_TYPE_INT32))
        return (int64_t)g_variant_get_int32(val);
    if (g_variant_is_of_type(val, G_VARIANT_TYPE_UINT32))
        return (int64_t)g_variant_get_uint32(val);
    if (g_variant_is_of_type(val, G_VARIANT_TYPE_DOUBLE))
        return (int64_t)g_variant_get_double(val);
    return defval;
}

SRD_API double c_decoder_get_option_double(struct srd_decoder_inst* di,
    const char* key, double defval)
{
    if (!di || !di->c_options || !key)
        return defval;
    GVariant* val = g_hash_table_lookup(di->c_options, key);
    if (!val)
        return defval;
    if (g_variant_is_of_type(val, G_VARIANT_TYPE_DOUBLE))
        return g_variant_get_double(val);
    if (g_variant_is_of_type(val, G_VARIANT_TYPE_INT64))
        return (double)g_variant_get_int64(val);
    if (g_variant_is_of_type(val, G_VARIANT_TYPE_UINT64))
        return (double)g_variant_get_uint64(val);
    return defval;
}

SRD_API const char* c_decoder_get_option_string(struct srd_decoder_inst* di,
    const char* key, const char* defval)
{
    if (!di || !di->c_options || !key)
        return defval;
    GVariant* val = g_hash_table_lookup(di->c_options, key);
    if (!val)
        return defval;
    if (g_variant_is_of_type(val, G_VARIANT_TYPE_STRING))
        return g_variant_get_string(val, NULL);
    return defval;
}

SRD_API int c_decoder_register_output_meta(struct srd_decoder_inst* di,
    int output_type, const char* proto_id,
    const char* meta_type_str, const char* meta_name, const char* meta_descr)
{
    struct srd_pd_output* pdo;
    int pdo_id;

    if (!di)
        return SRD_ERR_ARG;

    pdo_id = c_decoder_register_output(di, output_type, proto_id);
    if (pdo_id < 0)
        return pdo_id;

    GSList* out_list = g_slist_nth(di->pd_output, pdo_id);
    if (!out_list)
        return SRD_ERR_ARG;

    pdo = out_list->data;
    if (meta_type_str) {
        if (strcmp(meta_type_str, "int") == 0)
            pdo->meta_type = G_VARIANT_TYPE_INT64;
        else if (strcmp(meta_type_str, "float") == 0 || strcmp(meta_type_str, "double") == 0)
            pdo->meta_type = G_VARIANT_TYPE_DOUBLE;
    }
    pdo->meta_name = g_strdup(meta_name ? meta_name : "");
    pdo->meta_descr = g_strdup(meta_descr ? meta_descr : "");

    return pdo_id;
}

SRD_API int c_decoder_put_meta_int(struct srd_decoder_inst* di,
    uint64_t start_sample, uint64_t end_sample,
    int output_id, int64_t value)
{
    struct srd_pd_output* pdo;
    struct srd_pd_callback* cb;
    struct srd_proto_data pdata;
    struct srd_proto_data_meta pdm;

    if (!di)
        return SRD_ERR_ARG;

    GSList* out_list = g_slist_nth(di->pd_output, output_id);
    if (!out_list)
        return SRD_ERR_ARG;
    pdo = out_list->data;

    pdata.start_sample = start_sample;
    pdata.end_sample = end_sample;
    pdata.pdo = pdo;
    pdata.data = &pdm;
    pdm.key = pdo->pdo_id;
    pdm.value = g_variant_new_int64(value);

    if ((cb = srd_pd_output_callback_find_c(di->sess, SRD_OUTPUT_META))) {
        cb->cb(&pdata, cb->cb_data);
    }

    g_variant_unref(pdm.value);
    return SRD_OK;
}

SRD_API int c_decoder_put_meta_double(struct srd_decoder_inst* di,
    uint64_t start_sample, uint64_t end_sample,
    int output_id, double value)
{
    struct srd_pd_output* pdo;
    struct srd_pd_callback* cb;
    struct srd_proto_data pdata;
    struct srd_proto_data_meta pdm;

    if (!di)
        return SRD_ERR_ARG;

    GSList* out_list = g_slist_nth(di->pd_output, output_id);
    if (!out_list)
        return SRD_ERR_ARG;
    pdo = out_list->data;

    pdata.start_sample = start_sample;
    pdata.end_sample = end_sample;
    pdata.pdo = pdo;
    pdata.data = &pdm;
    pdm.key = pdo->pdo_id;
    pdm.value = g_variant_new_double(value);

    if ((cb = srd_pd_output_callback_find_c(di->sess, SRD_OUTPUT_META))) {
        cb->cb(&pdata, cb->cb_data);
    }

    g_variant_unref(pdm.value);
    return SRD_OK;
}

SRD_API uint64_t c_decoder_get_last_samplenum(struct srd_decoder_inst* di)
{
    if (!di)
        return 0;
    return di->last_samplenum;
}

SRD_API int c_wait(struct srd_decoder_inst *di, ...)
{
    va_list ap;
    int arg;
    GSList *or_groups = NULL;
    GSList *current_and = NULL;
    uint64_t samplenum = 0, matched = 0;
    int ret;

    if (!di)
        return SRD_ERR_ARG;

    va_start(ap, di);

    while ((arg = va_arg(ap, int)) != END) {
        if (arg == OR) {
            /* Flush current AND group into OR groups */
            if (current_and) {
                or_groups = g_slist_append(or_groups, current_and);
                current_and = NULL;
            }
        } else {
            /* arg encodes (type << 16 | channel) */
            int type = (arg >> 16) & 0xFFFF;
            int ch = arg & 0xFFFF;
            struct srd_term *t = g_malloc0(sizeof(struct srd_term));
            t->type = type;
            if (type == SRD_TERM_SKIP) {
                t->channel = -1;
                t->num_samples_to_skip = va_arg(ap, uint64_t);
                t->num_samples_already_skipped = 0;
            } else {
                t->channel = ch;
            }
            current_and = g_slist_append(current_and, t);
        }
    }

    /* Flush final AND group */
    if (current_and) {
        or_groups = g_slist_append(or_groups, current_and);
    }

    va_end(ap);

    /* Call the underlying wait — ownership of or_groups transfers to
     * c_decoder_wait_impl which stores it in di->condition_list.
     * Do NOT free it here; it will be freed by condition_list_free()
     * on the next c_wait() call or during instance cleanup. */
    ret = c_decoder_wait(di, or_groups, &samplenum, &matched);

    return ret;
}

SRD_API uint8_t c_pin(struct srd_decoder_inst *di, int ch)
{
    if (!di || ch < 0)
        return 0xFF;

    /* Check channel connectivity */
    if (ch >= di->dec_num_channels || di->dec_channelmap[ch] < 0)
        return 0xFF;

    /* Read from pin cache populated by c_decoder_wait */
    if (di->c_pin_cache)
        return di->c_pin_cache[ch];

    return 0xFF;
}

SRD_API int c_proto(struct srd_decoder_inst *di,
    uint64_t start_sample, uint64_t end_sample,
    int output_id, const char *cmd, ...)
{
    va_list ap;
    c_field f;
    c_field fields[64]; /* Stack buffer, 64 fields max */
    int n_fields = 0;

    if (!di)
        return SRD_ERR_ARG;

    /* Collect variadic c_field arguments.
     * Sentinel: C_END macro ((c_field){.type=C_FIELD_SENTINEL}).
     * Each argument is a c_field struct value (e.g. C_U8(val), C_STR("text")).
     * C_END terminates the list — avoids UB from va_arg(ap, c_field) with NULL. */
    va_start(ap, cmd);
    while (n_fields < 64) {
        f = va_arg(ap, c_field);
        if (f.type == C_FIELD_SENTINEL)
            break; /* C_END sentinel */
        fields[n_fields++] = f;
    }
    va_end(ap);

    /* Dispatch to stacked decoders */
    GSList *out_list = g_slist_nth(di->pd_output, output_id);
    if (!out_list)
        return SRD_ERR_ARG;

    struct srd_pd_output *pdo = out_list->data;
    if (pdo->output_type == SRD_OUTPUT_PROTO) {
        if (di->next_di) {
            GSList *l;
            for (l = di->next_di; l; l = l->next) {
                struct srd_decoder_inst *next_di = l->data;
                if (next_di->is_c_inst && next_di->c_dec_inst) {
                    if (next_di->c_dec_inst->decode_upper) {
                        next_di->c_dec_inst->decode_upper(next_di,
                            start_sample, end_sample,
                            cmd, fields, n_fields);
                    }
                }
                /* Python decoders receive data via the Python bridge in type_decoder.c */
            }
        }
    }

    return SRD_OK;
}

SRD_API int c_opt_bool(struct srd_decoder_inst *di, const char *key, int defval)
{
    const char *val = c_decoder_get_option_string(di, key, NULL);
    if (!val)
        return defval;
    if (strcmp(val, "yes") == 0 || strcmp(val, "true") == 0 || strcmp(val, "1") == 0)
        return 1;
    if (strcmp(val, "no") == 0 || strcmp(val, "false") == 0 || strcmp(val, "0") == 0)
        return 0;
    return defval;
}
