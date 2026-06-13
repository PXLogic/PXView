/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2010 Uwe Hermann <uwe@hermann-uwe.de>
 * Copyright (C) 2012 Bert Vermeulen <bert@biot.com>
 * Copyright (C) 2016 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBSIGROKDECODE_LIBSIGROKDECODE_H
#define LIBSIGROKDECODE_LIBSIGROKDECODE_H

#include <glib.h>
#include <log/xlog.h>
#include <stdint.h>

struct srd_c_dll_entry;

#define DECODE_NUM_HEX_MAX_LEN 256

#ifdef __cplusplus
extern "C" {
#endif

struct srd_session {
  int session_id;

  /*
              List of decoder instances.
              srd_decoder_inst* type
      */
  GSList *di_list;

  /* List of frontend callbacks to receive decoder output. */
  GSList *callbacks;
};

/**
 * @file
 *
 * The public libsigrokdecode header file to be used by frontends.
 *
 * This is the only file that libsigrokdecode users (frontends) are supposed
 * to use and include. There are other header files which get installed with
 * libsigrokdecode, but those are not meant to be used directly by frontends.
 *
 * The correct way to get/use the libsigrokdecode API functions is:
 *
 * @code{.c}
 *   #include <libsigrokdecode/libsigrokdecode.h>
 * @endcode
 */

/*
 * All possible return codes of libsigrokdecode functions must be listed here.
 * Functions should never return hardcoded numbers as status, but rather
 * use these enum values. All error codes are negative numbers.
 *
 * The error codes are globally unique in libsigrokdecode, i.e. if one of the
 * libsigrokdecode functions returns a "malloc error" it must be exactly the
 * same return value as used by all other functions to indicate "malloc error".
 * There must be no functions which indicate two different errors via the
 * same return code.
 *
 * Also, for compatibility reasons, no defined return codes are ever removed
 * or reused for different errors later. You can only add new entries and
 * return codes, but never remove or redefine existing ones.
 */

/** Status/error codes returned by libsigrokdecode functions. */
enum srd_error_code {
  SRD_OK = 0,                /**< No error */
  SRD_ERR = -1,              /**< Generic/unspecified error */
  SRD_ERR_MALLOC = -2,       /**< Malloc/calloc/realloc error */
  SRD_ERR_ARG = -3,          /**< Function argument error */
  SRD_ERR_BUG = -4,          /**< Errors hinting at internal bugs */
  SRD_ERR_PYTHON = -5,       /**< Python C API error */
  SRD_ERR_DECODERS_DIR = -6, /**< Protocol decoder path invalid */
  SRD_ERR_TERM_REQ = -7,     /**< Termination requested */

  /*
   * Note: When adding entries here, don't forget to also update the
   * srd_strerror() and srd_strerror_name() functions in error.c.
   */
};

/*
 * Use SRD_API to mark public API symbols, and SRD_PRIV for private symbols.
 *
 * Variables and functions marked 'static' are private already and don't
 * need SRD_PRIV. However, functions which are not static (because they need
 * to be used in other libsigrokdecode-internal files) but are also not
 * meant to be part of the public libsigrokdecode API, must use SRD_PRIV.
 *
 * This uses the 'visibility' feature of gcc (requires gcc >= 4.0).
 *
 * This feature is not available on MinGW/Windows, as it is a feature of
 * ELF files and MinGW/Windows uses PE files.
 *
 * Details: http://gcc.gnu.org/wiki/Visibility
 */

/* Marks public libsigrokdecode API symbols. */
#ifndef _WIN32
#define SRD_API __attribute__((visibility("default")))
#else
#define SRD_API
#endif

/* Marks private, non-public libsigrokdecode symbols (not part of the API). */
#ifndef _WIN32
#define SRD_PRIV __attribute__((visibility("hidden")))
#else
#define SRD_PRIV
#endif

/*
 * When adding an output type, don't forget to...
 *   - expose it to PDs in controller.c:PyInit_sigrokdecode()
 *   - add a check in module_sigrokdecode.c:Decoder_put()
 *   - add a debug string in type_decoder.c:OUTPUT_TYPES
 */
enum srd_output_type {
  SRD_OUTPUT_ANN,
  SRD_OUTPUT_PROTO,
  SRD_OUTPUT_BINARY,
  SRD_OUTPUT_META,
  SRD_OUTPUT_LOGIC,
};
/* Backward compatibility alias */
#define SRD_OUTPUT_PYTHON SRD_OUTPUT_PROTO

enum srd_term_type {
  SRD_TERM_HIGH,
  SRD_TERM_LOW,
  SRD_TERM_RISING_EDGE,
  SRD_TERM_FALLING_EDGE,
  SRD_TERM_EITHER_EDGE,
  SRD_TERM_NO_EDGE,
  SRD_TERM_SKIP,
};

struct srd_term {
  int type;
  int channel;
  uint64_t num_samples_to_skip;
  uint64_t num_samples_already_skipped;
};

enum c_field_type {
    C_FIELD_U8 = 0, C_FIELD_U16, C_FIELD_U32, C_FIELD_U64,
    C_FIELD_I8, C_FIELD_I16, C_FIELD_I32, C_FIELD_I64,
    C_FIELD_F64, C_FIELD_STR, C_FIELD_BYTES,
    C_FIELD_SENTINEL = 0xFF  /* internal: c_proto() end-of-args marker */
};

typedef struct {
    uint8_t type;
    union {
        uint8_t  u8;  uint16_t u16; uint32_t u32; uint64_t u64;
        int8_t   i8;  int16_t  i16; int32_t  i32; int64_t  i64;
        double   f64;
        const char *str;
        struct { const uint8_t *data; uint32_t len; } bytes;
    };
} c_field;

/* c_field constructor macros — 1:1 with Python data types */
#define C_U8(v)    ((c_field){.type=C_FIELD_U8, .u8=(uint8_t)(v)})
#define C_U16(v)   ((c_field){.type=C_FIELD_U16, .u16=(uint16_t)(v)})
#define C_U32(v)   ((c_field){.type=C_FIELD_U32, .u32=(uint32_t)(v)})
#define C_U64(v)   ((c_field){.type=C_FIELD_U64, .u64=(uint64_t)(v)})
#define C_I8(v)    ((c_field){.type=C_FIELD_I8, .i8=(int8_t)(v)})
#define C_I16(v)   ((c_field){.type=C_FIELD_I16, .i16=(int16_t)(v)})
#define C_I32(v)   ((c_field){.type=C_FIELD_I32, .i32=(int32_t)(v)})
#define C_I64(v)   ((c_field){.type=C_FIELD_I64, .i64=(int64_t)(v)})
#define C_F64(v)   ((c_field){.type=C_FIELD_F64, .f64=(double)(v)})
#define C_STR(v)   ((c_field){.type=C_FIELD_STR, .str=(const char*)(v)})
#define C_BYTES(d,n) ((c_field){.type=C_FIELD_BYTES, .bytes={.data=(d),.len=(n)}})
#define C_END        ((c_field){.type=C_FIELD_SENTINEL})  /* c_proto() sentinel */

/* c_wait() condition macros — 1:1 with Python self.wait() conditions */
#define _CW_ENC(_cw_t, _cw_c)  ((int)((_cw_t) << 16 | ((_cw_c) & 0xFFFF)))
#define CW_H(ch)   _CW_ENC(SRD_TERM_HIGH, ch)
#define CW_L(ch)   _CW_ENC(SRD_TERM_LOW, ch)
#define CW_R(ch)   _CW_ENC(SRD_TERM_RISING_EDGE, ch)
#define CW_F(ch)   _CW_ENC(SRD_TERM_FALLING_EDGE, ch)
#define CW_E(ch)   _CW_ENC(SRD_TERM_EITHER_EDGE, ch)
#define CW_N(ch)   _CW_ENC(SRD_TERM_NO_EDGE, ch)
#define CW_SKIP(n) _CW_ENC(SRD_TERM_SKIP, -1), (uint64_t)(n)
#define CW_OR  (-1)
#define CW_END (-2)
/* Short aliases for convenience in decoder code (C only) */
#ifndef __cplusplus
#define H(ch)   CW_H(ch)
#define L(ch)   CW_L(ch)
#define R(ch)   CW_R(ch)
#define F(ch)   CW_F(ch)
#define E(ch)   CW_E(ch)
#define N(ch)   CW_N(ch)
#define SKIP(n) CW_SKIP(n)
#define OR      CW_OR
#define END     CW_END
#endif

/* Quick access macros — 1:1 with Python self.samplenum / self.matched */
#define di_samplenum(di)   ((di)->abs_cur_samplenum)
#define di_matched(di)     ((di)->match_array)

enum srd_configkey {
  SRD_CONF_SAMPLERATE = 10000,
};

enum srd_channel_type {
  SRD_CHANNEL_COMMON = -1,
  SRD_CHANNEL_SCLK,
  SRD_CHANNEL_SDATA,
  SRD_CHANNEL_ADATA,
};

extern char decoders_path[256];

struct srd_decoder {
  /** The decoder ID. Must be non-NULL and unique for all decoders. */
  char *id;

  /** The (short) decoder name. Must be non-NULL. */
  char *name;

  /** The (long) decoder name. Must be non-NULL. */
  char *longname;

  /** A (short, one-line) description of the decoder. Must be non-NULL. */
  char *desc;

  /**
   * The license of the decoder. Valid values: "gplv2+", "gplv3+".
   * Other values are currently not allowed. Must be non-NULL.
   */
  char *license;

  /** List of possible decoder input IDs. */
  GSList *inputs;

  /** List of possible decoder output IDs. */
  GSList *outputs;

  /** List of tags associated with this decoder. */
  GSList *tags;

  /** List of channels required by this decoder. */
  GSList *channels;

  /** List of optional channels for this decoder. */
  GSList *opt_channels;

  /**
   * List of NULL-terminated char[], containing descriptions of the
   * supported annotation output.
   */
  GSList *annotations;
  GSList *ann_types;

  /**
   * List of annotation rows (row items: id, description, and a list
   * of annotation classes belonging to this row).
   */
  GSList *annotation_rows;

  /**
   * List of NULL-terminated char[], containing descriptions of the
   * supported binary output.
   */
  GSList *binary;

  /** List of decoder options. */
  GSList *options;

  /** Python module. */
  void *py_mod;

  /** sigrokdecode.Decoder class. */
  void *py_dec;

  gboolean is_c_decoder;
  struct srd_c_decoder *c_dec;
};

enum srd_initial_pin {
  SRD_INITIAL_PIN_LOW,
  SRD_INITIAL_PIN_HIGH,
  SRD_INITIAL_PIN_SAME_AS_SAMPLE0,
};

/**
 * Structure which contains information about one protocol decoder channel.
 * For example, I2C has two channels, SDA and SCL.
 */
struct srd_channel {
  /** The ID of the channel. Must be non-NULL. */
  char *id;
  /** The name of the channel. Must not be NULL. */
  char *name;
  /** The description of the channel. Must not be NULL. */
  char *desc;
  /** The index of the channel, i.e. its order in the list of channels. */
  int order;
  /** The type of the channel, such us: sclk/sdata/.../others */
  int type;
  /** The language text soruce id. */
  char *idn;
};

struct srd_decoder_option {
  char *id;
  char *idn;
  char *desc;
  GVariant *def;
  GSList *values;
};

struct srd_decoder_annotation_row {
  char *id;
  char *desc;
  GSList *ann_classes;
};

struct srd_decoder_binary {
  int bin_class;
  const char *id;
  const char *desc;
};

struct srd_decoder_inst;

struct srd_decoder_runtime {
  int (*wait)(struct srd_decoder_inst *di, GSList *condition_list,
              uint64_t *samplenum, uint64_t *matched);
  uint8_t (*get_pin)(struct srd_decoder_inst *di, int ch, uint64_t samplenum);
  void *(*get_private)(struct srd_decoder_inst *di);
  void (*set_private)(struct srd_decoder_inst *di, void *data);
};

struct srd_decoder_inst {
  struct srd_decoder *decoder;
  struct srd_session *sess;
  void *py_inst;
  void *py_pinvalues; /* is a python duple type, like (1,0,255,255)*/
  char *inst_id;
  GSList *pd_output; /* srd_pd_output* type */
  int dec_num_channels;
  int *dec_channelmap;
  GSList *next_di;

  /** List of conditions a PD wants to wait for.
   *  Type is srd_term* of GSList*
   */
  GSList *condition_list;

  /** Array of booleans denoting which conditions matched. */
  uint64_t match_array;

  /** Absolute start sample number. */
  uint64_t abs_start_samplenum;

  /** Absolute end sample number. */
  uint64_t abs_end_samplenum;

  /** Pointer to the buffer/chunk of input samples. */
  const uint8_t **inbuf;

  /** Pointer to the buffer/chunk of input const blocks. */
  const uint8_t *inbuf_const;

  /** Length (in bytes) of the input sample buffer. */
  uint64_t inbuflen;

  /** Absolute current samplenumber. */
  uint64_t abs_cur_samplenum;

  /** Absolute current sample matched conditions. */
  gboolean abs_cur_matched;

  /** Array of "old" (previous sample) pin values.
   *  Type of uint8_t
   */
  GArray *old_pins_array;

  /** Handle for this PD stack's worker thread. */
  GThread *thread_handle;

  /** Indicates whether new samples are available for processing. */
  gboolean got_new_samples;

  /** Indicates whether the worker thread has handled all samples. */
  gboolean handled_all_samples;

  /** Requests termination of wait() and decode(). */
  gboolean want_wait_terminate;

  /** First entry of wait(). */
  gboolean first_pos;

  /** skip zero flag. */
  gboolean skip_zero;

  /** Indicates the current state of the decoder stack. */
  int decoder_state;

  GCond got_new_samples_cond;
  GCond handled_all_samples_cond;
  GMutex data_mutex;

  char *python_proc_error;

  /** the task normal ends flag */
  int is_task_stop_signal;

  gboolean is_c_inst;
  struct srd_c_decoder *c_dec_inst;
  uint8_t *c_pin_cache;
  uint64_t c_pin_cache_samplenum;
  uint64_t c_pin_cache_inbuf_serial;
  void *user_data;
  char *error_message;
  uint64_t samplerate;
  uint64_t last_samplenum;
  GHashTable *c_options;
  const struct srd_decoder_runtime *runtime;
};

#define SRD_C_DECODER_API_VERSION 4
#define SRD_C_DECODER_API_MIN_VERSION 4

#ifdef _WIN32
#define SRD_C_DECODER_EXPORT __declspec(dllexport)
#else
#define SRD_C_DECODER_EXPORT __attribute__((visibility("default")))
#endif

typedef struct srd_c_decoder *(*srd_c_decoder_entry_func)(void);
typedef int (*srd_c_decoder_api_version_func)(void);

struct srd_c_ann_row {
  const char *id;
  const char *desc;
  const int *ann_classes;
  int num_ann_classes;
};

struct srd_c_decoder {
  const char *id;
  const char *name;
  const char *longname;
  const char *desc;
  const char *license;

  const struct srd_channel *channels;
  int num_channels;
  const struct srd_channel *optional_channels;
  int num_optional_channels;
  const struct srd_decoder_option *options;
  int num_options;

  int num_annotations;
  const char *(*ann_labels)[3];
  int num_annotation_rows;
  const struct srd_c_ann_row *annotation_rows;

  const char **inputs;
  int num_inputs;
  const char **outputs;
  int num_outputs;
  const struct srd_decoder_binary *binary;
  int num_binary;
  const char **tags;
  int num_tags;

  size_t state_size;  /* C_DECODER_STATE auto-sets this */

  void (*reset)(struct srd_decoder_inst *di);
  void (*start)(struct srd_decoder_inst *di);
  void (*decode)(struct srd_decoder_inst *di);
  void (*end)(struct srd_decoder_inst *di);
  void (*metadata)(struct srd_decoder_inst *di, int key, uint64_t value);
  void (*destroy)(struct srd_decoder_inst *di);
  void (*decode_upper)(struct srd_decoder_inst *di,
                       uint64_t start_sample, uint64_t end_sample,
                       const char *cmd, const c_field *fields, int n_fields);
};

struct srd_pd_output {
  int pdo_id;
  int output_type;
  struct srd_decoder_inst *di;
  char *proto_id;
  /* Only used for OUTPUT_META. */
  const GVariantType *meta_type;
  char *meta_name;
  char *meta_descr;
};

struct srd_proto_data {
  uint64_t start_sample;
  uint64_t end_sample;
  struct srd_pd_output *pdo;
  void *data;
};
struct srd_proto_data_annotation {
  int ann_class;
  int ann_type;
  char str_number_hex[DECODE_NUM_HEX_MAX_LEN]; // numerical value hex format
                                               // string
  long long numberic_value;
  char **ann_text; // text string lines
};
struct srd_proto_data_binary {
  int bin_class;
  uint64_t size;
  const unsigned char *data;
};
struct srd_proto_data_meta {
  int key;
  GVariant *value;
};
struct srd_proto_data_logic {
  uint32_t channel_mask;
  int num_channels;
  const uint8_t *values;
};

typedef void (*srd_pd_output_callback)(struct srd_proto_data *pdata,
                                       void *cb_data);

struct srd_pd_callback {
  int output_type;
  srd_pd_output_callback cb;
  void *cb_data;
};

/* srd.c */
SRD_API int srd_init(const char *path);
SRD_API int srd_exit(void);
SRD_API GSList *srd_searchpaths_get(void);
SRD_API void srd_set_python_home(const wchar_t *path);

/* session.c */
SRD_API int srd_session_new(struct srd_session **sess);
SRD_API int srd_session_start(struct srd_session *sess, char **error);
SRD_API int srd_session_metadata_set(struct srd_session *sess, int key,
                                     GVariant *data);
SRD_API int srd_session_send(struct srd_session *sess,
                             uint64_t abs_start_samplenum,
                             uint64_t abs_end_samplenum, const uint8_t **inbuf,
                             const uint8_t *inbuf_const, uint64_t inbuflen,
                             char **error);
SRD_API int srd_session_terminate_reset(struct srd_session *sess);
SRD_API int srd_session_destroy(struct srd_session *sess);
SRD_API int srd_pd_output_callback_add(struct srd_session *sess,
                                       int output_type,
                                       srd_pd_output_callback cb,
                                       void *cb_data);

SRD_API int srd_session_end(struct srd_session *sess, char **error);

/* decoder.c */
SRD_API const GSList *srd_decoder_list(void);
SRD_API struct srd_decoder *srd_decoder_get_by_id(const char *id);
SRD_API int srd_decoder_load(const char *name);
SRD_API char *srd_decoder_doc_get(const struct srd_decoder *dec);
SRD_API int srd_decoder_unload(struct srd_decoder *dec);
SRD_API int srd_decoder_load_all(void);
SRD_API int srd_decoder_unload_all(void);

/* instance.c */
SRD_API int srd_inst_option_set(struct srd_decoder_inst *di,
                                GHashTable *options);
SRD_API int srd_inst_channel_set_all(struct srd_decoder_inst *di,
                                     GHashTable *channels);
SRD_API struct srd_decoder_inst *
srd_inst_new(struct srd_session *sess, const char *id, GHashTable *options);
SRD_API int srd_inst_stack(struct srd_session *sess,
                           struct srd_decoder_inst *di_from,
                           struct srd_decoder_inst *di_to);
SRD_API struct srd_decoder_inst *srd_inst_find_by_id(struct srd_session *sess,
                                                     const char *inst_id);
SRD_API int srd_inst_initial_pins_set_all(struct srd_decoder_inst *di,
                                          GArray *initial_pins);

/* log.c */
/**
 * Use a shared context, and drop the private log context
 */
SRD_API void srd_log_set_context(xlog_context *ctx);

/**
 * Set the private log context level
 */
SRD_API void srd_log_level(int level);

/* error.c */
SRD_API const char *srd_strerror(int error_code);
SRD_API const char *srd_strerror_name(int error_code);

/* version.c */
SRD_API int srd_package_version_major_get(void);
SRD_API int srd_package_version_minor_get(void);
SRD_API int srd_package_version_micro_get(void);
SRD_API const char *srd_package_version_string_get(void);
SRD_API int srd_lib_version_current_get(void);
SRD_API int srd_lib_version_revision_get(void);
SRD_API int srd_lib_version_age_get(void);
SRD_API const char *srd_lib_version_string_get(void);

SRD_API int srd_c_decoder_register(struct srd_c_decoder *dec);
SRD_API int srd_c_decoder_load_all(void);
SRD_API int srd_c_decoder_path_set(const char *path);
SRD_API int srd_c_decoder_path_add(const char *path);
SRD_API void srd_c_decoder_paths_clear(void);
SRD_API int srd_c_decoder_load(const char *dll_path);
SRD_API int srd_c_decoder_unload(const char *decoder_id);
SRD_API const GSList *srd_c_dll_registry_get(void);
SRD_API const struct srd_c_dll_entry *
srd_c_dll_info_get(const char *decoder_id);

struct srd_c_annotation {
  int ann_class;
  int ann_type;
  char **ann_text;
  char str_number_hex[DECODE_NUM_HEX_MAX_LEN];
  long long numberic_value;
};

SRD_API int c_decoder_put(struct srd_decoder_inst *di, uint64_t start_sample,
                          uint64_t end_sample, int output_id,
                          struct srd_c_annotation *ann);
SRD_API int c_decoder_put_binary(struct srd_decoder_inst *di,
                                 uint64_t start_sample, uint64_t end_sample,
                                 int output_id, int bin_class, uint64_t size,
                                 const unsigned char *data);
SRD_API int c_decoder_put_logic(struct srd_decoder_inst *di,
                                uint64_t start_sample, uint64_t end_sample,
                                int output_id, uint32_t channel_mask,
                                const uint8_t *values, int num_channels);
SRD_API int c_decoder_wait(struct srd_decoder_inst *di, GSList *condition_list,
                           uint64_t *samplenum, uint64_t *matched);
SRD_API int c_decoder_has_channel(struct srd_decoder_inst *di, int ch);
SRD_API int c_decoder_register_output(struct srd_decoder_inst *di,
                                      int output_type, const char *proto_id);
SRD_API int
c_decoder_register_output_meta(struct srd_decoder_inst *di, int output_type,
                               const char *proto_id, const char *meta_type,
                               const char *meta_name, const char *meta_descr);
SRD_API int c_decoder_put_meta_int(struct srd_decoder_inst *di,
                                   uint64_t start_sample, uint64_t end_sample,
                                   int output_id, int64_t value);
SRD_API int c_decoder_put_meta_double(struct srd_decoder_inst *di,
                                      uint64_t start_sample,
                                      uint64_t end_sample, int output_id,
                                      double value);
SRD_API uint64_t c_decoder_get_samplerate(struct srd_decoder_inst *di);
SRD_API uint64_t c_decoder_get_last_samplenum(struct srd_decoder_inst *di);
SRD_API int64_t c_decoder_get_option_int(struct srd_decoder_inst *di,
                                         const char *key, int64_t defval);
SRD_API double c_decoder_get_option_double(struct srd_decoder_inst *di,
                                           const char *key, double defval);
SRD_API const char *c_decoder_get_option_string(struct srd_decoder_inst *di,
                                                const char *key,
                                                const char *defval);
SRD_API void *c_decoder_get_private(struct srd_decoder_inst *di);
SRD_API void c_decoder_set_private(struct srd_decoder_inst *di, void *data);

#define C_ANN_PUT(di, ss, es, out_id, cls, ...)                                \
  do {                                                                         \
    const char *_txts[] = {__VA_ARGS__, NULL};                                 \
    struct srd_c_annotation _ann = {cls, 0, (char **)_txts, "", 0};            \
    c_decoder_put(di, ss, es, out_id, &_ann);                                  \
  } while (0)

#define C_ANN_PUT_TYPE(di, ss, es, out_id, cls, tp, ...)                       \
  do {                                                                         \
    const char *_txts[] = {__VA_ARGS__, NULL};                                 \
    struct srd_c_annotation _ann = {cls, tp, (char **)_txts, "", 0};           \
    c_decoder_put(di, ss, es, out_id, &_ann);                                  \
  } while (0)

#define C_ANN_PUT_VAL(di, ss, es, out_id, cls, val, ...)                       \
  do {                                                                         \
    const char *_txts[] = {__VA_ARGS__, NULL};                                 \
    struct srd_c_annotation _ann = {cls, 0, (char **)_txts, "",                \
                                    (long long)(val)};                         \
    snprintf(_ann.str_number_hex, DECODE_NUM_HEX_MAX_LEN, "0x%X",              \
             (unsigned int)(val));                                             \
    c_decoder_put(di, ss, es, out_id, &_ann);                                  \
  } while (0)

/* v4 convenience aliases for annotation output */
#define c_put(di, ss, es, out_id, cls, ...)    C_ANN_PUT(di, ss, es, out_id, cls, __VA_ARGS__)
#define c_put_v(di, ss, es, out_id, cls, val, ...) C_ANN_PUT_VAL(di, ss, es, out_id, cls, val, __VA_ARGS__)
#define c_put_t(di, ss, es, out_id, cls, tp, ...)  C_ANN_PUT_TYPE(di, ss, es, out_id, cls, tp, __VA_ARGS__)

/* v4 variadic declarative condition wait — replaces c_cond_* builder pattern */
SRD_API int c_wait(struct srd_decoder_inst *di, ...);

/* v4 pin access — replaces c_decoder_get_pin */
SRD_API uint8_t c_pin(struct srd_decoder_inst *di, int ch);

/* v4 structured protocol output — replaces c_decoder_put_proto */
SRD_API int c_proto(struct srd_decoder_inst *di, uint64_t start_sample,
                    uint64_t end_sample, int output_id,
                    const char *cmd, ...);  /* C_END-terminated c_field args */

/* v4 boolean option — replaces string comparison pattern */
SRD_API int c_opt_bool(struct srd_decoder_inst *di, const char *key, int defval);

/* v4 shortcut API aliases — shorter names for common operations */
#define c_opt_int    c_decoder_get_option_int
#define c_opt_str    c_decoder_get_option_string
#define c_opt_dbl    c_decoder_get_option_double
#define c_has_ch     c_decoder_has_channel
#define c_samplerate c_decoder_get_samplerate
#define c_last_samplenum c_decoder_get_last_samplenum
#define c_init_pin   c_decoder_get_initial_pin
#define c_reg_out    c_decoder_register_output
#define c_reg_meta   c_decoder_register_output_meta
#define c_put_bin    c_decoder_put_binary
#define c_put_logic  c_decoder_put_logic
#define c_put_meta_int c_decoder_put_meta_int
#define c_put_meta_dbl c_decoder_put_meta_double

/* C_DECODER_STATE — auto-generates state struct, reset, and destroy */
#define C_DECODER_STATE(name, fields) \
    typedef struct name##_s fields name##_s; \
    static void name##_reset(struct srd_decoder_inst *di) { \
        name##_s *s = (name##_s *)calloc(1, sizeof(name##_s)); \
        c_decoder_set_private(di, s); \
    } \
    static void name##_destroy(struct srd_decoder_inst *di) { \
        void *p = c_decoder_get_private(di); \
        free(p); \
        c_decoder_set_private(di, NULL); \
    }

/* C_DECODER_DEFINE — auto-generates decoder struct and DLL entry */
#define C_DECODER_DEFINE(dec_name, ...) \
    static struct srd_c_decoder dec_name##_def = { \
        __VA_ARGS__ \
    }; \
    SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void) { \
        return &dec_name##_def; \
    } \
    SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void) { \
        return SRD_C_DECODER_API_VERSION; \
    }

SRD_API uint8_t c_decoder_get_initial_pin(struct srd_decoder_inst *di, int ch);

#include "version.h"

#ifdef __cplusplus
}
#endif

#endif
