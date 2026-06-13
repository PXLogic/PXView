/*
 * decoder_test.c - C decoder test harness for libsigrokdecode
 *
 * Loads a C decoder DLL, feeds it bit-packed logic data, collects
 * annotation output, and compares with expected output.
 *
 * Usage:
 *   decoder_test -d <decoder_name> -t <testdata_dir> [-o <output_dir>]
 *                [--tolerance N] [--generate-only] [--python]
 *
 * Exit codes: 0 = PASS, 1 = FAIL, 2 = error
 *
 * Copyright (C) 2026 DreamSourceLab <support@dreamsourcelab.com>
 * License: GPLv3+
 */

#ifndef CJSON_IMPLEMENTATION
#define CJSON_IMPLEMENTATION
#endif
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#include <glib.h>
#include <libsigrokdecode/libsigrokdecode.h>

/* ------------------------------------------------------------------ */
/*  Annotation collection                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t start_sample;
    uint64_t end_sample;
    int ann_class;
    int ann_type;
    char **texts;  /* NULL-terminated array of strings */
} collected_annotation;

typedef struct {
    collected_annotation *items;
    int count;
    int capacity;
} annotation_list;

static void ann_list_init(annotation_list *list)
{
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void ann_list_ensure(annotation_list *list, int needed)
{
    if (list->count + needed <= list->capacity)
        return;
    int new_cap = list->capacity ? list->capacity * 2 : 64;
    while (new_cap < list->count + needed)
        new_cap *= 2;
    list->items = (collected_annotation *)realloc(list->items,
                    new_cap * sizeof(collected_annotation));
    list->capacity = new_cap;
}

static void ann_list_free(annotation_list *list)
{
    for (int i = 0; i < list->count; i++) {
        if (list->items[i].texts) {
            for (int j = 0; list->items[i].texts[j]; j++)
                free(list->items[i].texts[j]);
            free(list->items[i].texts);
        }
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

/* Callback for srd_pd_output_callback_add */
static void collect_callback(struct srd_proto_data *pdata, void *cb_data)
{
    if (!pdata || !pdata->data) return;

    annotation_list *list = (annotation_list *)cb_data;
    struct srd_proto_data_annotation *ann =
        (struct srd_proto_data_annotation *)pdata->data;

    ann_list_ensure(list, 1);
    collected_annotation *item = &list->items[list->count];
    memset(item, 0, sizeof(*item));

    item->start_sample = pdata->start_sample;
    item->end_sample   = pdata->end_sample;
    item->ann_class    = ann->ann_class;
    item->ann_type     = ann->ann_type;

    /* Deep copy ann_text array */
    int n_texts = 0;
    if (ann->ann_text) {
        while (ann->ann_text[n_texts])
            n_texts++;
    }
    item->texts = (char **)calloc(n_texts + 1, sizeof(char *));
    for (int i = 0; i < n_texts; i++) {
        if (ann->ann_text[i]) {
            /* Apply same @-prefix handling as py_parse_ann_data does for Python decoders:
               If text starts with '@' and the rest is a valid hex string (1-15 chars),
               replace the text with "\n" (ignore flag) and store the hex in str_number_hex. */
            const char *src = ann->ann_text[i];
            if (src[0] == '@') {
                int hlen = (int)strlen(src + 1);
                int valid_hex = (hlen > 0 && hlen < 16);
                if (valid_hex) {
                    for (int h = 1; src[h]; h++) {
                        char c = src[h];
                        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                            valid_hex = 0;
                            break;
                        }
                    }
                }
                if (valid_hex) {
                    item->texts[i] = strdup("\n");
                    continue;
                }
            }
            item->texts[i] = strdup(src);
        } else {
            item->texts[i] = strdup("");
        }
    }
    item->texts[n_texts] = NULL;

    list->count++;
}

/* ------------------------------------------------------------------ */
/*  File I/O helpers                                                   */
/* ------------------------------------------------------------------ */

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)len, f);
    buf[rd] = '\0';
    fclose(f);
    if (out_len) *out_len = rd;
    return buf;
}

static uint8_t *read_binary_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)len);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (out_len) *out_len = rd;
    return buf;
}

static int write_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fwrite(data, 1, len, f);
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Path helpers                                                       */
/* ------------------------------------------------------------------ */

static char *path_join(const char *dir, const char *name)
{
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);
    char *p = (char *)malloc(dlen + 1 + nlen + 1);
    memcpy(p, dir, dlen);
    p[dlen] = '/';
    memcpy(p + dlen + 1, name, nlen + 1);
    return p;
}

#ifdef _WIN32
#include <windows.h>
static char *get_exe_dir(void)
{
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (len == 0) return NULL;
    /* find last backslash */
    char *p = buf + len;
    while (p > buf && *p != '\\' && *p != '/') p--;
    if (p <= buf) return NULL;
    *p = '\0';
    return strdup(buf);
}
#else
static char *get_exe_dir(void)
{
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len < 0) return NULL;
    buf[len] = '\0';
    char *p = strrchr(buf, '/');
    if (!p) return NULL;
    *p = '\0';
    return strdup(buf);
}
#endif

/* ------------------------------------------------------------------ */
/*  JSON → annotation_list comparison                                  */
/* ------------------------------------------------------------------ */

static cJSON *ann_list_to_json(const char *decoder_id, uint64_t samplerate,
                                const annotation_list *list)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "decoder", decoder_id);
    cJSON_AddNumberToObject(root, "samplerate", (double)samplerate);
    cJSON_AddNumberToObject(root, "num_annotations", list->count);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < list->count; i++) {
        const collected_annotation *a = &list->items[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "start_sample", (double)a->start_sample);
        cJSON_AddNumberToObject(obj, "end_sample", (double)a->end_sample);
        cJSON_AddNumberToObject(obj, "ann_class", a->ann_class);
        cJSON_AddNumberToObject(obj, "ann_type", a->ann_type);

        cJSON *texts = cJSON_CreateArray();
        for (int j = 0; a->texts && a->texts[j]; j++)
            cJSON_AddItemToArray(texts, cJSON_CreateString(a->texts[j]));
        cJSON_AddItemToObject(obj, "texts", texts);

        cJSON_AddItemToArray(arr, obj);
    }
    cJSON_AddItemToObject(root, "annotations", arr);
    return root;
}

static const char *get_real_decoder_id(const char *requested_id)
{
    struct srd_decoder *d = srd_decoder_get_by_id(requested_id);
    if (d) return d->id;
    const GSList *l;
    for (l = srd_decoder_list(); l; l = l->next) {
        d = (struct srd_decoder *)l->data;
        if (d->id && g_ascii_strcasecmp(d->id, requested_id) == 0) return d->id;
    }
    return requested_id;
}

static int json_to_ann_list(cJSON *root, annotation_list *list)
{
    ann_list_init(list);

    cJSON *ann_arr = cJSON_GetObjectItem(root, "annotations");
    if (!ann_arr || !cJSON_IsArray(ann_arr)) return -1;

    int n = cJSON_GetArraySize(ann_arr);
    ann_list_ensure(list, n);

    for (int i = 0; i < n; i++) {
        cJSON *obj = cJSON_GetArrayItem(ann_arr, i);
        if (!obj) continue;

        collected_annotation *item = &list->items[list->count];
        memset(item, 0, sizeof(*item));

        cJSON *v;
        v = cJSON_GetObjectItem(obj, "start_sample");
        item->start_sample = v ? (uint64_t)v->valuedouble : 0;
        v = cJSON_GetObjectItem(obj, "end_sample");
        item->end_sample = v ? (uint64_t)v->valuedouble : 0;
        v = cJSON_GetObjectItem(obj, "ann_class");
        item->ann_class = v ? v->valueint : 0;
        v = cJSON_GetObjectItem(obj, "ann_type");
        item->ann_type = v ? v->valueint : 0;

        cJSON *texts = cJSON_GetObjectItem(obj, "texts");
        if (texts && cJSON_IsArray(texts)) {
            int nt = cJSON_GetArraySize(texts);
            item->texts = (char **)calloc(nt + 1, sizeof(char *));
            for (int j = 0; j < nt; j++) {
                cJSON *t = cJSON_GetArrayItem(texts, j);
                item->texts[j] = strdup(cJSON_IsString(t) ? t->valuestring : "");
            }
            item->texts[nt] = NULL;
        }

        list->count++;
    }
    return 0;
}

static int compare_annotations(const annotation_list *actual,
                                const annotation_list *expected,
                                int tolerance)
{
    if (actual->count != expected->count) {
        fprintf(stderr, "FAIL: annotation count mismatch: actual=%d, expected=%d\n",
                actual->count, expected->count);
        return 1;
    }

    for (int i = 0; i < actual->count; i++) {
        const collected_annotation *a = &actual->items[i];
        const collected_annotation *e = &expected->items[i];

        /* Compare start_sample with tolerance */
        if (a->start_sample < e->start_sample) {
            if (e->start_sample - a->start_sample > (uint64_t)tolerance) {
                fprintf(stderr, "FAIL: annotation[%d].start_sample mismatch: "
                        "actual=%llu, expected=%llu (tolerance=%d)\n",
                        i, (unsigned long long)a->start_sample,
                        (unsigned long long)e->start_sample, tolerance);
                return 1;
            }
        } else {
            if (a->start_sample - e->start_sample > (uint64_t)tolerance) {
                fprintf(stderr, "FAIL: annotation[%d].start_sample mismatch: "
                        "actual=%llu, expected=%llu (tolerance=%d)\n",
                        i, (unsigned long long)a->start_sample,
                        (unsigned long long)e->start_sample, tolerance);
                return 1;
            }
        }

        /* Compare end_sample with tolerance */
        if (a->end_sample < e->end_sample) {
            if (e->end_sample - a->end_sample > (uint64_t)tolerance) {
                fprintf(stderr, "FAIL: annotation[%d].end_sample mismatch: "
                        "actual=%llu, expected=%llu (tolerance=%d)\n",
                        i, (unsigned long long)a->end_sample,
                        (unsigned long long)e->end_sample, tolerance);
                return 1;
            }
        } else {
            if (a->end_sample - e->end_sample > (uint64_t)tolerance) {
                fprintf(stderr, "FAIL: annotation[%d].end_sample mismatch: "
                        "actual=%llu, expected=%llu (tolerance=%d)\n",
                        i, (unsigned long long)a->end_sample,
                        (unsigned long long)e->end_sample, tolerance);
                return 1;
            }
        }

        /* Compare ann_class */
        if (a->ann_class != e->ann_class) {
            fprintf(stderr, "FAIL: annotation[%d].ann_class mismatch: "
                    "actual=%d, expected=%d\n", i, a->ann_class, e->ann_class);
            return 1;
        }

        /* Compare ann_type */
        if (a->ann_type != e->ann_type) {
            fprintf(stderr, "FAIL: annotation[%d].ann_type mismatch: "
                    "actual=%d, expected=%d\n", i, a->ann_type, e->ann_type);
            return 1;
        }

        /* Compare texts */
        int a_nt = 0, e_nt = 0;
        while (a->texts && a->texts[a_nt]) a_nt++;
        while (e->texts && e->texts[e_nt]) e_nt++;
        if (a_nt != e_nt) {
            fprintf(stderr, "FAIL: annotation[%d].texts count mismatch: "
                    "actual=%d, expected=%d\n", i, a_nt, e_nt);
            return 1;
        }
        for (int j = 0; j < a_nt; j++) {
            if (strcmp(a->texts[j], e->texts[j]) != 0) {
                fprintf(stderr, "FAIL: annotation[%d].texts[%d] mismatch: "
                        "actual=\"%s\", expected=\"%s\"\n",
                        i, j, a->texts[j], e->texts[j]);
                return 1;
            }
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Command line parsing                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *decoder_name;
    const char *testdata_dir;
    const char *output_dir;
    const char *output_file; /* New option */
    int tolerance;
    int generate_only;
    int python_mode;
} cmdline_args;

static int parse_args(int argc, char **argv, cmdline_args *args)
{
    memset(args, 0, sizeof(*args));
    args->tolerance = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            args->decoder_name = argv[++i];
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            args->testdata_dir = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            args->output_dir = argv[++i];
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            args->output_file = argv[++i];
        } else if (strcmp(argv[i], "--tolerance") == 0 && i + 1 < argc) {
            args->tolerance = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--generate-only") == 0) {
            args->generate_only = 1;
        } else if (strcmp(argv[i], "--python") == 0) {
            args->python_mode = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        }
    }

    if (!args->decoder_name) {
        fprintf(stderr, "Error: -d <decoder_name> is required\n");
        return -1;
    }
    if (!args->testdata_dir) {
        fprintf(stderr, "Error: -t <testdata_dir> is required\n");
        return -1;
    }
    if (!args->output_dir)
        args->output_dir = args->testdata_dir;

    return 0;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s -d <decoder_name> -t <testdata_dir> [-o <output_dir>] "
        "[--tolerance N] [--generate-only] [--python]\n\n"
        "  -d <decoder_name>   Decoder ID (e.g. \"spi_c\" for C, \"spi\" for Python)\n"
        "  -t <testdata_dir>   Directory containing config.json, input.bin, expected.json\n"
        "  -o <output_dir>     Output directory for actual.json (default: same as testdata_dir)\n"
        "  --tolerance N       Sample range tolerance for comparison (default: 0)\n"
        "  --generate-only     Only run decoder and output actual.json, skip comparison\n"
        "  --python            Run Python decoder instead of C decoder\n",
        prog);
}

/* Look up the default GVariant for a given option id from the decoder definition.
 * Returns NULL if not found. The returned GVariant is owned by the decoder struct. */
static GVariant *lookup_option_default(struct srd_decoder *dec, const char *opt_id)
{
    if (!dec || !opt_id) return NULL;

    /* Check Python-style options (GSList) */
    for (GSList *l = dec->options; l; l = l->next) {
        struct srd_decoder_option *opt = (struct srd_decoder_option *)l->data;
        if (opt->id && strcmp(opt->id, opt_id) == 0)
            return opt->def;
    }

    /* Check C decoder options (array) */
    if (dec->is_c_decoder && dec->c_dec) {
        for (int i = 0; i < dec->c_dec->num_options; i++) {
            if (dec->c_dec->options[i].id && strcmp(dec->c_dec->options[i].id, opt_id) == 0)
                return dec->c_dec->options[i].def;
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    cmdline_args args;
    if (parse_args(argc, argv, &args) != 0) {
        print_usage(argv[0]);
        return 2;
    }

    /* ---- 1. Read config.json ---- */
    char *config_path = path_join(args.testdata_dir, "config.json");
    size_t config_len = 0;
    char *config_text = read_file(config_path, &config_len);
    if (!config_text) {
        fprintf(stderr, "Error: cannot read %s\n", config_path);
        free(config_path);
        return 2;
    }
    free(config_path);

    cJSON *config = cJSON_Parse(config_text);
    if (!config) {
        fprintf(stderr, "Error: failed to parse config.json\n");
        fprintf(stderr, "  Content (first 200 chars): %.200s\n", config_text);
        free(config_text);
        return 2;
    }
    free(config_text);

    const char *decoder_id = args.decoder_name;
    (void)decoder_id; /* also in config, but CLI takes precedence */

    cJSON *j_samplerate = cJSON_GetObjectItem(config, "samplerate");
    uint64_t samplerate = j_samplerate ? (uint64_t)j_samplerate->valuedouble : 1000000;

    cJSON *j_num_channels = cJSON_GetObjectItem(config, "num_channels");
    int num_channels = j_num_channels ? j_num_channels->valueint : 0;

    cJSON *j_sample_count = cJSON_GetObjectItem(config, "sample_count");
    uint64_t sample_count = j_sample_count ? (uint64_t)j_sample_count->valuedouble : 0;

    cJSON *j_needs_upstream = cJSON_GetObjectItem(config, "needs_upstream");
    if (j_needs_upstream && cJSON_IsTrue(j_needs_upstream)) {
        printf("SKIP: decoder requires upstream output\n");
        cJSON_Delete(config);
        return 0;
    }

    cJSON *j_channels = cJSON_GetObjectItem(config, "channels");
    cJSON *j_options  = cJSON_GetObjectItem(config, "options");

    if (num_channels <= 0 || sample_count == 0) {
        fprintf(stderr, "Error: config.json must specify num_channels and sample_count\n");
        cJSON_Delete(config);
        return 2;
    }

    /* ---- 2. Read input.bin ---- */
    char *input_path = path_join(args.testdata_dir, "input.bin");
    size_t input_len = 0;
    uint8_t *input_data = read_binary_file(input_path, &input_len);
    if (!input_data) {
        fprintf(stderr, "Error: cannot read %s\n", input_path);
        free(input_path);
        cJSON_Delete(config);
        return 2;
    }
    free(input_path);

    /* Split into per-channel arrays. */
    uint64_t bytes_per_channel = (sample_count + 7) / 8;
    uint64_t expected_size = bytes_per_channel * (uint64_t)num_channels;

    if (input_len < expected_size) {
        fprintf(stderr, "Error: input.bin too small: got %llu bytes, need %llu\n",
                (unsigned long long)input_len, (unsigned long long)expected_size);
        free(input_data);
        cJSON_Delete(config);
        return 2;
    }

    const uint8_t **inbuf = (const uint8_t **)calloc(num_channels, sizeof(uint8_t *));
    uint8_t *inbuf_const  = (uint8_t *)calloc(num_channels, 1);

    for (int ch = 0; ch < num_channels; ch++) {
        inbuf[ch] = input_data + ch * bytes_per_channel;
        inbuf_const[ch] = 0;
    }

    /* ---- 3. Initialize libsigrokdecode ---- */
    int ret;
    if (args.python_mode) {
        const char *py_dec_path = getenv("PY_DECODERS_PATH");
        char *init_path = NULL;
        if (py_dec_path) init_path = strdup(py_dec_path);
        else {
            char *exe_dir = get_exe_dir();
            if (exe_dir) {
                init_path = g_build_filename(exe_dir, "..", "libsigrokdecode", "decoders", NULL);
                free(exe_dir);
            }
        }
#ifdef _WIN32
        {
            const char *dll_dirs[] = { "D:\\msys64\\mingw64\\bin", NULL };
            for (int di = 0; dll_dirs[di]; di++) {
                wchar_t wpath[MAX_PATH];
                MultiByteToWideChar(CP_UTF8, 0, dll_dirs[di], -1, wpath, MAX_PATH);
                AddDllDirectory(wpath);
            }
        }
#endif
        ret = srd_init(init_path);
        free(init_path);
        if (ret != SRD_OK) {
            fprintf(stderr, "Error: srd_init() failed: %d\n", ret);
            free(inbuf); free(inbuf_const); free(input_data); cJSON_Delete(config);
            return 2;
        }
        /* Load only required python decoders instead of srd_decoder_load_all() */
    } else {
        ret = srd_init(NULL);
        if (ret != SRD_OK) {
            fprintf(stderr, "Error: srd_init() failed: %d\n", ret);
            free(inbuf); free(inbuf_const); free(input_data); cJSON_Delete(config);
            return 2;
        }
        const char *dec_path = getenv("C_DECODERS_PATH");
        if (dec_path) srd_c_decoder_path_add(dec_path);
        else {
            srd_c_decoder_path_add("build.dir/decoders/c_decoders");
            srd_c_decoder_path_add("../build.dir/decoders/c_decoders");
            srd_c_decoder_path_add("../../build.dir/decoders/c_decoders");
        }
        srd_c_decoder_load_all();
    }

    char *mapped_decoder_name = strdup(args.decoder_name);
    if (args.python_mode) {
        size_t slen = strlen(mapped_decoder_name);
        if (slen > 2 && strcmp(mapped_decoder_name + slen - 2, "_c") == 0) {
            mapped_decoder_name[slen - 2] = '\0';
        }
        /* Load main decoder */
        srd_decoder_load(mapped_decoder_name);
        
        /* Load stacked decoders if present */
        cJSON *j_stack = cJSON_GetObjectItem(config, "stack");
        if (j_stack && cJSON_IsArray(j_stack)) {
            for (int i = 0; i < cJSON_GetArraySize(j_stack); i++) {
                cJSON *item = cJSON_GetArrayItem(j_stack, i);
                cJSON *sid_obj = cJSON_GetObjectItem(item, "id");
                if (sid_obj) {
                    const char *sid = sid_obj->valuestring;
                    char *mapped_sid = NULL;
                    if (strlen(sid) > 2 && strcmp(sid + strlen(sid) - 2, "_c") == 0) {
                        mapped_sid = strdup(sid);
                        mapped_sid[strlen(sid) - 2] = '\0';
                    } else {
                        mapped_sid = strdup(sid);
                    }
                    srd_decoder_load(mapped_sid);
                    free(mapped_sid);
                }
            }
        }
    }

    struct srd_decoder *dec = srd_decoder_get_by_id(mapped_decoder_name);
    if (!dec) {
        const GSList *l;
        for (l = srd_decoder_list(); l; l = l->next) {
            struct srd_decoder *d = (struct srd_decoder *)l->data;
            if (d->id && g_ascii_strcasecmp(d->id, mapped_decoder_name) == 0) {
                dec = d;
                break;
            }
        }
    }
    
    if (!dec && args.python_mode) {
        /* Fallback: if we couldn't load it or find it (maybe the folder name is completely different from the ID, e.g. folder 'qspi' but id 'smart_qspi'), 
           we have no choice but to load all decoders. This is slow but only happens for a few decoders. */
        srd_decoder_load_all();
        dec = srd_decoder_get_by_id(mapped_decoder_name);
        if (!dec) {
            const GSList *l;
            for (l = srd_decoder_list(); l; l = l->next) {
                struct srd_decoder *d = (struct srd_decoder *)l->data;
                if (d->id && g_ascii_strcasecmp(d->id, mapped_decoder_name) == 0) {
                    dec = d;
                    break;
                }
            }
        }
    }
    
    if (!dec) {
        fprintf(stderr, "Error: decoder '%s' not found\n", mapped_decoder_name);
        free(mapped_decoder_name);
        free(inbuf); free(inbuf_const); free(input_data); cJSON_Delete(config);
        return 2;
    }

    /* ---- 4. Create session and decoder instance ---- */
    struct srd_session *sess = NULL;
    ret = srd_session_new(&sess);
    if (ret != SRD_OK || !sess) {
        fprintf(stderr, "Error: srd_session_new() failed: %d\n", ret);
        free(inbuf); free(inbuf_const); free(input_data); cJSON_Delete(config);
        return 2;
    }

    GHashTable *opt_hash = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);
    if (j_options && cJSON_IsObject(j_options)) {
        cJSON *opt = j_options->child;
        while (opt) {
            GVariant *gvar = NULL;
            if (cJSON_IsString(opt)) gvar = g_variant_new_string(opt->valuestring);
            else if (cJSON_IsNumber(opt)) {
                double v = opt->valuedouble;
                /* Look up the option's default type from the decoder definition
                 * to determine whether to create int64 or double GVariant.
                 * Without this, whole-number floats like 70.0 would be
                 * incorrectly converted to int64, causing type mismatch errors
                 * with Python decoders that expect double. */
                GVariant *def = lookup_option_default(dec, opt->string);
                if (def && g_variant_is_of_type(def, G_VARIANT_TYPE_DOUBLE))
                    gvar = g_variant_new_double(v);
                else if (def && g_variant_is_of_type(def, G_VARIANT_TYPE_INT64))
                    gvar = g_variant_new_int64((int64_t)v);
                else if (v == (double)(int64_t)v)
                    gvar = g_variant_new_int64((int64_t)v);
                else
                    gvar = g_variant_new_double(v);
            } else if (cJSON_IsBool(opt)) gvar = g_variant_new_boolean(cJSON_IsTrue(opt));
            if (gvar) g_hash_table_insert(opt_hash, g_strdup(opt->string), g_variant_ref_sink(gvar));
            opt = opt->next;
        }
    }

    struct srd_decoder_inst *di = srd_inst_new(sess, get_real_decoder_id(args.decoder_name), opt_hash);
    g_hash_table_destroy(opt_hash);
    if (!di) {
        fprintf(stderr, "Error: srd_inst_new() failed\n");
        srd_session_destroy(sess); free(inbuf); free(inbuf_const); free(input_data); cJSON_Delete(config);
        return 2;
    }

    GHashTable *ch_hash = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);
    if (j_channels && cJSON_IsObject(j_channels)) {
        cJSON *ch = j_channels->child;
        while (ch) {
            if (cJSON_IsNumber(ch)) g_hash_table_insert(ch_hash, g_strdup(ch->string), g_variant_ref_sink(g_variant_new_int32(ch->valueint)));
            ch = ch->next;
        }
    }
    srd_inst_channel_set_all(di, ch_hash);
    g_hash_table_destroy(ch_hash);

    /* Stacking */
    cJSON *j_stack = cJSON_GetObjectItem(config, "stack");
    if (j_stack && cJSON_IsArray(j_stack)) {
        struct srd_decoder_inst *prev_di = NULL;
        for (int i = 0; i < cJSON_GetArraySize(j_stack); i++) {
            cJSON *item = cJSON_GetArrayItem(j_stack, i);
            const char *sid = cJSON_GetObjectItem(item, "id")->valuestring;
            char *mapped_sid = NULL;
            if (args.python_mode && strlen(sid) > 2 && strcmp(sid + strlen(sid) - 2, "_c") == 0) {
                mapped_sid = strdup(sid);
                mapped_sid[strlen(sid) - 2] = '\0';
            } else {
                mapped_sid = strdup(sid);
            }
            struct srd_decoder_inst *s_di = srd_inst_new(sess, get_real_decoder_id(mapped_sid), NULL);
            free(mapped_sid);
            if (!s_di) continue;
            cJSON *sch = cJSON_GetObjectItem(item, "channels");
            if (sch && cJSON_IsObject(sch)) {
                GHashTable *sh = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);
                cJSON *c = sch->child;
                while (c) {
                    if (cJSON_IsNumber(c)) g_hash_table_insert(sh, g_strdup(c->string), g_variant_ref_sink(g_variant_new_int32(c->valueint)));
                    c = c->next;
                }
                srd_inst_channel_set_all(s_di, sh);
                g_hash_table_destroy(sh);
            } else {
                /* Auto-map: map the stack decoder's channels (required + optional)
                 * to input channels in order */
                GHashTable *sh = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);
                int ch_idx = 0;
                /* Map required channels first */
                GSList *ch_list = s_di->decoder->channels;
                while (ch_list && ch_idx < num_channels) {
                    struct srd_channel *ch = (struct srd_channel *)ch_list->data;
                    g_hash_table_insert(sh, g_strdup(ch->id), g_variant_ref_sink(g_variant_new_int32(ch_idx)));
                    ch_list = ch_list->next;
                    ch_idx++;
                }
                /* Then map optional channels */
                ch_list = s_di->decoder->opt_channels;
                while (ch_list && ch_idx < num_channels) {
                    struct srd_channel *ch = (struct srd_channel *)ch_list->data;
                    g_hash_table_insert(sh, g_strdup(ch->id), g_variant_ref_sink(g_variant_new_int32(ch_idx)));
                    ch_list = ch_list->next;
                    ch_idx++;
                }
                srd_inst_channel_set_all(s_di, sh);
                g_hash_table_destroy(sh);
            }
            if (prev_di) {
                ret = srd_inst_stack(sess, prev_di, s_di);
                if (ret != SRD_OK) {
                    fprintf(stderr, "Error: srd_inst_stack() failed for %s -> %s "
                            "(C→Python stacking is not supported in v4 API)\n",
                            prev_di->inst_id, s_di->inst_id);
                    srd_session_destroy(sess); free(inbuf); free(inbuf_const);
                    free(input_data); cJSON_Delete(config);
                    return 2;
                }
            }
            prev_di = s_di;
        }
        if (prev_di) {
            ret = srd_inst_stack(sess, prev_di, di);
            if (ret != SRD_OK) {
                fprintf(stderr, "Error: srd_inst_stack() failed for %s -> %s "
                        "(C→Python stacking is not supported in v4 API)\n",
                        prev_di->inst_id, di->inst_id);
                srd_session_destroy(sess); free(inbuf); free(inbuf_const);
                free(input_data); cJSON_Delete(config);
                return 2;
            }
        }
    }

    annotation_list ann_list;
    ann_list_init(&ann_list);
    srd_pd_output_callback_add(sess, SRD_OUTPUT_ANN, collect_callback, &ann_list);
    srd_session_metadata_set(sess, SRD_CONF_SAMPLERATE, g_variant_new_uint64(samplerate));

    char *error = NULL;
    fprintf(stderr, "Starting session...\n"); fflush(stderr);
    if (srd_session_start(sess, &error) != SRD_OK) {
        fprintf(stderr, "Error: session_start failed: %s\n", error ? error : "unknown");
        srd_session_destroy(sess); free(inbuf); free(inbuf_const); free(input_data); cJSON_Delete(config);
        return 2;
    }

    fprintf(stderr, "Sending %llu samples...\n", (unsigned long long)sample_count); fflush(stderr);
    
    /* We MUST provide pointers for the BOTTOM decoder's channels. */
    struct srd_decoder_inst *bottom_di = di;
    /* Find bottom decoder (the one receiving from frontend) */
    GSList *li;
    for (li = sess->di_list; li; li = li->next) {
        bottom_di = li->data;
        /* In our test, there's usually only one stack root */
        break;
    }

    const uint8_t **c_inbuf = (const uint8_t **)calloc(bottom_di->dec_num_channels, sizeof(uint8_t *));
    uint8_t *c_const = (uint8_t *)calloc(bottom_di->dec_num_channels, 1);
    
    /* Build mapping for bottom_di */
    for (int i = 0; i < bottom_di->dec_num_channels; i++) {
        int in_idx = bottom_di->dec_channelmap[i];
        if (in_idx >= 0 && in_idx < num_channels) {
            c_inbuf[i] = inbuf[in_idx];
        } else {
            c_inbuf[i] = NULL;
            c_const[i] = 0; 
        }
    }

    error = NULL;
    ret = srd_session_send(sess, 0, sample_count - 1, c_inbuf, c_const,
                           sample_count, &error);
    free(c_inbuf); free(c_const);

    srd_session_end(sess, &error);

    cJSON *actual_json = ann_list_to_json(args.decoder_name, samplerate, &ann_list);
    char *actual_text = cJSON_Print(actual_json);

    char *actual_path = NULL;
    if (args.output_file) {
        actual_path = strdup(args.output_file);
    } else {
        actual_path = path_join(args.output_dir, "actual.json");
    }

    if (write_file(actual_path, actual_text, strlen(actual_text)) != 0) {
        fprintf(stderr, "Error: cannot write %s\n", actual_path);
    } else {
        printf("Wrote %s (%d annotations)\n", actual_path, ann_list.count);
    }
    free(actual_path);
    cJSON_free(actual_text);

    int exit_code = 0;
    if (!args.generate_only) {
        char *ep = path_join(args.testdata_dir, "expected.json");
        size_t el; char *et = read_file(ep, &el);
        if (et) {
            cJSON *ej = cJSON_Parse(et); free(et);
            if (ej) {
                annotation_list exl; json_to_ann_list(ej, &exl);
                exit_code = compare_annotations(&ann_list, &exl, args.tolerance);
                if (exit_code == 0) printf("PASS\n");
                ann_list_free(&exl); cJSON_Delete(ej);
            }
        }
        free(ep);
    }

    srd_session_destroy(sess);
    ann_list_free(&ann_list);
    free(inbuf); free(inbuf_const); free(input_data); cJSON_Delete(config);
    return exit_code;
}
