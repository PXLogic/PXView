#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_SETUP_READ = 0,
    ANN_SETUP_WRITE,
    ANN_BULK_READ,
    ANN_BULK_WRITE,
    ANN_ERROR,
    NUM_ANN,
};

enum {
    BINARY_PCAP = 0,
};

enum usb_req_transaction_state {
    TX_IDLE = 0,
    TX_TOKEN_RECEIVED,
    TX_DATA_RECEIVED,
};

enum usb_req_type {
    REQ_NONE = 0,
    REQ_BULK_IN,
    REQ_BULK_OUT,
    REQ_SETUP_IN,
    REQ_SETUP_OUT,
};

#define USB_REQ_MAX_DATA 2048
#define USB_REQ_MAX_PENDING 64

typedef struct {
    int type;                   /* REQ_NONE, REQ_BULK_IN, etc. */
    uint8_t setup_data[8];
    int setup_data_len;
    uint8_t data[USB_REQ_MAX_DATA];
    int data_len;
    int n_fields;
    uint16_t wLength;
    int handshake;              /* 0=none, 1=ACK, 2=NAK, 3=STALL, 4=NYET, 5=timeout */
    uint64_t ss;
    uint64_t es;
    uint64_t ss_data;
    int id;
    int addr;
    int ep;
} usb_req_request;

typedef struct {
    int out_ann;
    int out_binary;
    int out_python;
    int transaction_state;      /* TX_IDLE, TX_TOKEN_RECEIVED, TX_DATA_RECEIVED */
    int transaction_type;       /* 0=IN, 1=OUT, 2=SETUP */
    int transaction_ep;
    int transaction_addr;
    uint8_t transaction_data[USB_REQ_MAX_DATA];
    int transaction_data_len;
    int handshake;
    uint64_t ss_transaction;
    uint64_t es_transaction;
    int request_id;
    int wrote_pcap_header;
    uint64_t samplerate;
    double secs_per_sample;
    int in_request_start;       /* 0=submit, 1=first-ack */
    usb_req_request requests[USB_REQ_MAX_PENDING];
    int num_requests;
} usb_req_state;

static const char *usb_req_inputs[] = {"usb_packet", NULL};
static const char *usb_req_outputs[] = {"usb_request", NULL};
static const char *usb_req_tags[] = {"PC", NULL};

static struct srd_decoder_option usb_req_options[] = {
    {"in_request_start", "dec_usb_request_opt_in_request_start",
     "Start IN requests on", NULL, NULL},
};

static const char *usb_req_ann_labels[][3] = {
    {"", "request-setup-read", "Setup: Device-to-host"},
    {"", "request-setup-write", "Setup: Host-to-device"},
    {"", "request-bulk-read", "Bulk: Device-to-host"},
    {"", "request-bulk-write", "Bulk: Host-to-device"},
    {"", "error", "Unexpected packet"},
};

static const int usb_req_row_setup_classes[] = {ANN_SETUP_READ, ANN_SETUP_WRITE, -1};
static const int usb_req_row_in_classes[] = {ANN_BULK_READ, -1};
static const int usb_req_row_out_classes[] = {ANN_BULK_WRITE, -1};
static const int usb_req_row_errors_classes[] = {ANN_ERROR, -1};
static const struct srd_c_ann_row usb_req_ann_rows[] = {
    {"request-setup", "USB SETUP", usb_req_row_setup_classes, 2},
    {"request-in", "USB BULK IN", usb_req_row_in_classes, 1},
    {"request-out", "USB BULK OUT", usb_req_row_out_classes, 1},
    {"errors", "Errors", usb_req_row_errors_classes, 1},
};

static const struct srd_decoder_binary usb_req_binary[] = {
    {0, "pcap", "PCAP format"},
};

/* Find or create a request for (addr, ep) */
static usb_req_request *usb_req_find_or_create(usb_req_state *s, int addr, int ep)
{
    for (int i = 0; i < s->num_requests; i++) {
        if (s->requests[i].addr == addr && s->requests[i].ep == ep)
            return &s->requests[i];
    }
    if (s->num_requests >= USB_REQ_MAX_PENDING) return NULL;
    usb_req_request *r = &s->requests[s->num_requests++];
    memset(r, 0, sizeof(usb_req_request));
    r->id = s->request_id++;
    r->addr = addr;
    r->ep = ep;
    r->type = REQ_NONE;
    return r;
}

/* Remove a request for (addr, ep) */
static void usb_req_remove(usb_req_state *s, int addr, int ep)
{
    for (int i = 0; i < s->num_requests; i++) {
        if (s->requests[i].addr == addr && s->requests[i].ep == ep) {
            memmove(&s->requests[i], &s->requests[i + 1],
                    (s->num_requests - i - 1) * sizeof(usb_req_request));
            s->num_requests--;
            return;
        }
    }
}

/* Build request summary string */
static void usb_req_summary(usb_req_request *r, char *buf, int buflen)
{
    int pos = 0;
    pos += snprintf(buf + pos, buflen - pos, "[");
    if (r->type == REQ_SETUP_IN || r->type == REQ_SETUP_OUT) {
        for (int i = 0; i < r->setup_data_len && pos < buflen - 10; i++)
            pos += snprintf(buf + pos, buflen - pos, " %02X", r->setup_data[i]);
        pos += snprintf(buf + pos, buflen - pos, " ][");
    }
    for (int i = 0; i < r->data_len && pos < buflen - 10; i++)
        pos += snprintf(buf + pos, buflen - pos, " %02X", r->data[i]);
    const char *hs = "none";
    if (r->handshake == 1) hs = "ACK";
    else if (r->handshake == 2) hs = "NAK";
    else if (r->handshake == 3) hs = "STALL";
    else if (r->handshake == 4) hs = "NYET";
    else if (r->handshake == 5) hs = "timeout";
    pos += snprintf(buf + pos, buflen - pos, " ] : %s", hs);
}

/* Write PCAP global header */
static void usb_req_write_pcap_header(struct srd_decoder_inst *di, usb_req_state *s)
{
    if (s->wrote_pcap_header) return;
    unsigned char hdr[24];
    hdr[0] = 0xa1; hdr[1] = 0xb2; hdr[2] = 0xc3; hdr[3] = 0xd4; /* Magic */
    hdr[4] = 0x00; hdr[5] = 0x02; /* Major version */
    hdr[6] = 0x00; hdr[7] = 0x04; /* Minor version */
    hdr[8] = 0x00; hdr[9] = 0x00; hdr[10] = 0x00; hdr[11] = 0x00; /* Correction */
    hdr[12] = 0x00; hdr[13] = 0x00; hdr[14] = 0x00; hdr[15] = 0x00; /* Accuracy */
    hdr[16] = 0xff; hdr[17] = 0xff; hdr[18] = 0xff; hdr[19] = 0xff; /* Max len */
    hdr[20] = 0x00; hdr[21] = 0x00; hdr[22] = 0x00; hdr[23] = 0xdc; /* LINKTYPE_USB_LINUX_MMAPPED=220 */
    c_put_bin(di, 0, 0, s->out_binary, BINARY_PCAP, 24, hdr);
    s->wrote_pcap_header = 1;
}

/* Handle a completed or started request - output annotations and binary */
static void usb_req_handle_request(struct srd_decoder_inst *di, usb_req_state *s,
                                    usb_req_request *r, int request_start, int request_end)
{
    if (!request_start && !request_end) return;

    usb_req_write_pcap_header(di, s);

    if (request_end) {
        char summary[512];
        usb_req_summary(r, summary, sizeof(summary));
        uint64_t ss = r->ss;
        uint64_t es = r->es;
        uint64_t ss_data = r->ss_data;
        if (s->in_request_start == 0)
            ss_data = ss;

        if (r->type == REQ_SETUP_IN) {
            char buf[600];
            snprintf(buf, sizeof(buf), "SETUP in: %s", summary);
            c_put(di, ss, es, s->out_ann, ANN_SETUP_READ, buf);
        } else if (r->type == REQ_SETUP_OUT) {
            char buf[600];
            snprintf(buf, sizeof(buf), "SETUP out: %s", summary);
            c_put(di, ss, es, s->out_ann, ANN_SETUP_WRITE, buf);
        } else if (r->type == REQ_BULK_IN) {
            char buf[600];
            snprintf(buf, sizeof(buf), "BULK in: %s", summary);
            c_put(di, ss_data, es, s->out_ann, ANN_BULK_READ, buf);
        } else if (r->type == REQ_BULK_OUT) {
            char buf[600];
            snprintf(buf, sizeof(buf), "BULK out: %s", summary);
            c_put(di, ss, es, s->out_ann, ANN_BULK_WRITE, buf);
        }

        usb_req_remove(s, r->addr, r->ep);
    }
}

/* Handle a completed transfer */
static void usb_req_handle_transfer(struct srd_decoder_inst *di, usb_req_state *s)
{
    int request_started = 0;
    int request_end = (s->handshake == 1 || s->handshake == 3 || s->handshake == 5);
    int ep = s->transaction_ep;
    int addr = s->transaction_addr;

    /* Handle protocol STALLs */
    if (s->transaction_type == 2) { /* SETUP */
        /* Only close existing requests that are SETUP IN or SETUP OUT */
        usb_req_request *existing = usb_req_find_or_create(s, addr, ep);
        if (existing && (existing->type == REQ_SETUP_IN || existing->type == REQ_SETUP_OUT)) {
            existing->es = s->ss_transaction;
            usb_req_handle_request(di, s, existing, 0, 1);
        }
    }

    usb_req_request *r = usb_req_find_or_create(s, addr, ep);
    if (!r) return;

    if (r->type == REQ_NONE) {
        r->ss = s->ss_transaction;
        request_started = 1;
    }

    if (request_end) {
        r->es = s->es_transaction;
        r->handshake = s->handshake;
    }

    /* BULK or INTERRUPT transfer */
    if ((r->type == REQ_NONE || r->type == REQ_BULK_IN) && s->transaction_type == 0) { /* IN */
        r->type = REQ_BULK_IN;
        if (r->data_len == 0 && s->transaction_data_len > 0)
            r->ss_data = s->ss_transaction;
        if (r->data_len + s->transaction_data_len <= USB_REQ_MAX_DATA) {
            memcpy(r->data + r->data_len, s->transaction_data, s->transaction_data_len);
            r->data_len += s->transaction_data_len;
        }
        usb_req_handle_request(di, s, r, request_started, request_end);
    } else if ((r->type == REQ_NONE || r->type == REQ_BULK_OUT) && s->transaction_type == 1) { /* OUT */
        r->type = REQ_BULK_OUT;
        if (s->handshake == 1) { /* ACK */
            if (r->data_len + s->transaction_data_len <= USB_REQ_MAX_DATA) {
                memcpy(r->data + r->data_len, s->transaction_data, s->transaction_data_len);
                r->data_len += s->transaction_data_len;
            }
        }
        usb_req_handle_request(di, s, r, request_started, request_end);
    }
    /* CONTROL, SETUP stage */
    else if (r->type == REQ_NONE && s->transaction_type == 2) { /* SETUP */
        int copy_len = s->transaction_data_len < 8 ? s->transaction_data_len : 8;
        memcpy(r->setup_data, s->transaction_data, copy_len);
        r->setup_data_len = copy_len;
        if (copy_len >= 8)
            r->wLength = r->setup_data[6] | (r->setup_data[7] << 8);
        if (r->setup_data[0] & 0x80) {
            r->type = REQ_SETUP_IN;
            usb_req_handle_request(di, s, r, 1, 0);
        } else {
            r->type = REQ_SETUP_OUT;
            usb_req_handle_request(di, s, r, (r->wLength == 0) ? 1 : 0, 0);
        }
    }
    /* CONTROL, DATA stage */
    else if (r->type == REQ_SETUP_IN && s->transaction_type == 0) { /* IN */
        if (r->data_len + s->transaction_data_len <= USB_REQ_MAX_DATA) {
            memcpy(r->data + r->data_len, s->transaction_data, s->transaction_data_len);
            r->data_len += s->transaction_data_len;
        }
    } else if (r->type == REQ_SETUP_OUT && s->transaction_type == 1) { /* OUT */
        if (s->handshake == 1) { /* ACK */
            if (r->data_len + s->transaction_data_len <= USB_REQ_MAX_DATA) {
                memcpy(r->data + r->data_len, s->transaction_data, s->transaction_data_len);
                r->data_len += s->transaction_data_len;
            }
        }
        if (r->wLength == r->data_len) {
            usb_req_handle_request(di, s, r, 1, 0);
        }
    }
    /* CONTROL, STATUS stage */
    else if (r->type == REQ_SETUP_IN && s->transaction_type == 1) { /* OUT */
        usb_req_handle_request(di, s, r, 0, request_end);
    } else if (r->type == REQ_SETUP_OUT && s->transaction_type == 0) { /* IN */
        usb_req_handle_request(di, s, r, 0, request_end);
    }
}

static void usb_req_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    usb_req_state *s = (usb_req_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "PACKET") != 0) return;
    if (!fields || n_fields < 2) return;

    /* Parse packet category and name from usb_packet_c output.
       Format from usb_packet_c:
       TOKEN:    C_STR("TOKEN"), C_STR(pid_name), C_U8(addr), C_U8(ep)
       DATA:     C_STR("DATA"), C_STR(pid_name), C_BYTES(data, len)
       HANDSHAKE: C_STR("HANDSHAKE"), C_STR(pid_name)
       SPECIAL:  C_STR("SPECIAL"), C_STR(pid_name)
    */
    if (fields[0].type != C_FIELD_STR) return;
    const char *pcategory = fields[0].str;
    if (fields[1].type != C_FIELD_STR) return;
    const char *pname = fields[1].str;

    if (strcmp(pcategory, "TOKEN") == 0) {
        if (strcmp(pname, "SOF") == 0) return;

        if (s->transaction_state == TX_TOKEN_RECEIVED) {
            /* Timeout detection */
            uint64_t timeout = s->es_transaction;
            uint64_t duration = s->es_transaction - s->ss_transaction;
            timeout += duration / 2;
            if (start_sample > timeout) {
                s->es_transaction = timeout;
                s->handshake = 5; /* timeout */
                usb_req_handle_transfer(di, s);
                s->transaction_state = TX_IDLE;
            }
        }

        if (s->transaction_state != TX_IDLE) {
            char buf[256];
            snprintf(buf, sizeof(buf), "ERR: received %s token in state %s", pname,
                s->transaction_state == TX_IDLE ? "IDLE" :
                s->transaction_state == TX_TOKEN_RECEIVED ? "TOKEN RECEIVED" :
                s->transaction_state == TX_DATA_RECEIVED ? "DATA RECEIVED" : "UNKNOWN");
            c_put(di, start_sample, end_sample, s->out_ann, ANN_ERROR, buf);
            return;
        }

        /* Parse addr and ep from fields */
        int addr = 0, ep = 0;
        if (n_fields >= 4 && fields[2].type == C_FIELD_U8 && fields[3].type == C_FIELD_U8) {
            addr = fields[2].u8;
            ep = fields[3].u8;
        }

        s->transaction_data_len = 0;
        s->ss_transaction = start_sample;
        s->es_transaction = end_sample;
        s->transaction_state = TX_TOKEN_RECEIVED;
        s->transaction_ep = ep;
        s->transaction_addr = addr;

        if (strcmp(pname, "IN") == 0) {
            s->transaction_type = 0; /* IN */
            if (ep > 0) s->transaction_ep = ep + 0x80;
        } else if (strcmp(pname, "OUT") == 0) {
            s->transaction_type = 1; /* OUT */
        } else if (strcmp(pname, "SETUP") == 0) {
            s->transaction_type = 2; /* SETUP */
        }

    } else if (strcmp(pcategory, "DATA") == 0) {
        if (s->transaction_state != TX_TOKEN_RECEIVED) {
            char buf[256];
            snprintf(buf, sizeof(buf), "ERR: received %s token in state %s", pname,
                s->transaction_state == TX_IDLE ? "IDLE" :
                s->transaction_state == TX_TOKEN_RECEIVED ? "TOKEN RECEIVED" :
                s->transaction_state == TX_DATA_RECEIVED ? "DATA RECEIVED" : "UNKNOWN");
            c_put(di, start_sample, end_sample, s->out_ann, ANN_ERROR, buf);
            return;
        }

        /* Parse data bytes from C_BYTES field */
        if (n_fields >= 3 && fields[2].type == C_FIELD_BYTES) {
            int data_len_pkt = fields[2].bytes.len;
            int copy_len = data_len_pkt < USB_REQ_MAX_DATA ? data_len_pkt : USB_REQ_MAX_DATA;
            if (copy_len > 0) {
                memcpy(s->transaction_data, fields[2].bytes.data, copy_len);
            }
            s->transaction_data_len = copy_len;
        }
        s->transaction_state = TX_DATA_RECEIVED;

    } else if (strcmp(pcategory, "HANDSHAKE") == 0) {
        if (s->transaction_state != TX_TOKEN_RECEIVED && s->transaction_state != TX_DATA_RECEIVED) {
            char buf[256];
            snprintf(buf, sizeof(buf), "ERR: received %s token in state %s", pname,
                s->transaction_state == TX_IDLE ? "IDLE" :
                s->transaction_state == TX_TOKEN_RECEIVED ? "TOKEN RECEIVED" :
                s->transaction_state == TX_DATA_RECEIVED ? "DATA RECEIVED" : "UNKNOWN");
            c_put(di, start_sample, end_sample, s->out_ann, ANN_ERROR, buf);
            return;
        }

        if (strcmp(pname, "ACK") == 0) s->handshake = 1;
        else if (strcmp(pname, "NAK") == 0) s->handshake = 2;
        else if (strcmp(pname, "STALL") == 0) s->handshake = 3;
        else if (strcmp(pname, "NYET") == 0) s->handshake = 4;
        else s->handshake = 0;

        s->transaction_state = TX_IDLE;
        s->es_transaction = end_sample;
        usb_req_handle_transfer(di, s);

    } else if (strcmp(pcategory, "SPECIAL") == 0) {
        if (strcmp(pname, "PRE") == 0) return;
        /* Ignore other special packets */
    }
}

static void usb_req_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(usb_req_state)));
    }
    usb_req_state *s = (usb_req_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(usb_req_state));
    s->transaction_state = TX_IDLE;
}

static void usb_req_start(struct srd_decoder_inst *di)
{
    usb_req_state *s = (usb_req_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "usb_request");
    s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "pcap");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PYTHON, "usb_request");

    const char *irs = c_opt_str(di, "in_request_start", "submit");
    s->in_request_start = (irs && strcmp(irs, "first-ack") == 0) ? 1 : 0;
}

static void usb_req_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void usb_req_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder usb_request_c_decoder = {
    .id = "usb_request_c",
    .name = "USB request(C)",
    .longname = "Universal Serial Bus (LS/FS) transaction/request (C)",
    .desc = "USB (low-speed/full-speed) transaction/request protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = usb_req_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = usb_req_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = usb_req_ann_rows,
    .inputs = usb_req_inputs,
    .num_inputs = 1,
    .outputs = usb_req_outputs,
    .num_outputs = 1,
    .binary = usb_req_binary,
    .num_binary = 1,
    .tags = usb_req_tags,
    .num_tags = 1,
    .reset = usb_req_reset,
    .start = usb_req_start,
    .decode = usb_req_decode,
    .destroy = usb_req_destroy,
    .decode_upper = usb_req_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    usb_req_options[0].def = g_variant_new_string("submit");
    GSList *irs_vals = NULL;
    irs_vals = g_slist_append(irs_vals, g_variant_new_string("submit"));
    irs_vals = g_slist_append(irs_vals, g_variant_new_string("first-ack"));
    usb_req_options[0].values = irs_vals;

    return &usb_request_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}