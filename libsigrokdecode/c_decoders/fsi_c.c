/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2020 Raptor Engineering, LLC <support@raptorengineering.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "libsigrokdecode.h"
#include <glib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ANN_WARNINGS = 0,
    ANN_START,
    ANN_CYCLE_TYPE,
    ANN_DIRECTION,
    ANN_ADDR,
    ANN_DATA,
    ANN_COMMANDS,
    ANN_CRC,
    ANN_TAR,
    NUM_ANN,
};

enum fsi_state {
    STATE_IDLE,
    STATE_TX_SLAVE_ID,
    STATE_COMMAND,
    STATE_DIRECTION,
    STATE_REL_ADDRESS_SIGN,
    STATE_ADDRESS,
    STATE_DATA_SIZE,
    STATE_TX_DATA,
    STATE_CRC,
    STATE_TAR,
    STATE_RX_SLAVE_ID,
    STATE_RESPONSE,
    STATE_RX_DATA,
    STATE_RX_IPOLL_INTERRUPT_FIELD,
    STATE_RX_IPOLL_DMA_CONTROL_FIELD,
    STATE_BREAK_TAR_QUEUED,
    STATE_BREAK_TAR,
};

enum fsi_command {
    CMD_NONE = 0,
    CMD_ABS_ADR,
    CMD_REL_ADR,
    CMD_SAME_ADR,
    CMD_D_POLL,
    CMD_E_POLL,
    CMD_I_POLL,
    CMD_TERM,
};

enum fsi_response {
    RSP_NONE = 0,
    RSP_ACK_D,
    RSP_ACK,
    RSP_BUSY,
    RSP_ERR_A,
    RSP_ERR_C,
    RSP_I_POLL_RSP,
};

enum fsi_data_size {
    SIZE_BYTE = 0,
    SIZE_HALF_WORD,
    SIZE_WORD,
    SIZE_UNKNOWN,
};

struct fsi_priv {
    int state;
    int tar_cycles;

    /* BREAK detection */
    uint64_t break_start_sample_number;
    int break_counter;
    int fsi_data_break_prev;

    /* Sampling tracking */
    uint64_t samplenum_prev;
    int fsi_data_prev;

    /* CRC */
    int crc_internal;
    int crc_calculating;
    int computed_crc_tx_end;

    /* Response tracking */
    int response_received;
    int valid_response;
    int busy_seq_count;

    /* Address tracking */
    uint64_t prev_address[4];
    int prev_address_valid[4];

    /* Current transaction */
    int tx_slave_id;
    int rx_slave_id;
    int data_count;
    int command_count;
    int command_code;
    int command;
    int valid_command;
    int direction;
    int relative_address_negative;
    uint64_t address;
    uint64_t address_raw;
    int address_length;
    int address_count;
    int data_size;
    int data_length;
    uint64_t data;
    int crc;
    int crc_count;

    /* Response */
    int response_count;
    int response_code;
    int response;

    /* TAR */
    int tar_timer;
    int timeout_counter;

    /* Annotation ranges */
    uint64_t ss_block;
    uint64_t es_block;

    int out_ann;
};

static struct srd_channel fsi_channels[] = {
    {"data", "DATA", "Frame", 0, SRD_CHANNEL_SDATA, "dec_fsi_chan_data"},
    {"clock", "CLOCK", "Clock", 1, SRD_CHANNEL_SCLK, "dec_fsi_chan_clock"},
};

static const char *fsi_ann_labels[][3] = {
    {"", "warnings", "Warnings"},
    {"", "start", "Start"},
    {"", "cycle-type", "Cycle type"},
    {"", "direction", "Direction"},
    {"", "addr", "Address"},
    {"", "data", "Data"},
    {"", "commands", "Commands"},
    {"", "crc", "CRC"},
    {"", "turn-around", "TAR"},
};

static const int fsi_row_data_classes[] = {ANN_START, ANN_CYCLE_TYPE, ANN_DIRECTION, ANN_ADDR, ANN_DATA, ANN_COMMANDS, ANN_CRC, ANN_TAR};
static const int fsi_row_warnings_classes[] = {ANN_WARNINGS};
static const struct srd_c_ann_row fsi_ann_rows[] = {
    {"data", "Data", fsi_row_data_classes, 8},
    {"warnings", "Warnings", fsi_row_warnings_classes, 1},
};

static const char *fsi_inputs[] = {"logic", NULL};
static const char *fsi_outputs[] = {NULL};
static const char *fsi_tags[] = {"PC", NULL};

static const char *fsi_command_name(int cmd)
{
    switch (cmd) {
    case CMD_ABS_ADR: return "ABS_ADR";
    case CMD_REL_ADR: return "REL_ADR";
    case CMD_SAME_ADR: return "SAME_ADR";
    case CMD_D_POLL: return "D_POLL";
    case CMD_E_POLL: return "E_POLL";
    case CMD_I_POLL: return "I_POLL";
    case CMD_TERM: return "TERM";
    default: return "UNKNOWN";
    }
}

static const char *fsi_response_name(int rsp)
{
    switch (rsp) {
    case RSP_ACK_D: return "ACK_D";
    case RSP_ACK: return "ACK";
    case RSP_BUSY: return "BUSY";
    case RSP_ERR_A: return "ERR_A";
    case RSP_ERR_C: return "ERR_C";
    case RSP_I_POLL_RSP: return "I_POLL_RSP";
    default: return "UNKNOWN";
    }
}

static const char *fsi_data_size_name(int sz)
{
    switch (sz) {
    case SIZE_BYTE: return "BYTE";
    case SIZE_HALF_WORD: return "HALF_WORD";
    case SIZE_WORD: return "WORD";
    default: return "UNKNOWN";
    }
}

static void fsi_putb(struct srd_decoder_inst *di, struct fsi_priv *s, int cls, const char *text)
{
    c_put(di, s->ss_block, s->es_block, s->out_ann, cls, text);
}

static void fsi_putb_fmt(struct srd_decoder_inst *di, struct fsi_priv *s, int cls, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    c_put(di, s->ss_block, s->es_block, s->out_ann, cls, buf);
}

static void fsi_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(struct fsi_priv)));
    }
    struct fsi_priv *s = (struct fsi_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct fsi_priv));
    s->tar_cycles = 3;
    s->out_ann = -1;
    s->command = CMD_NONE;
    s->response = RSP_NONE;
    s->data_size = SIZE_BYTE;
}

static void fsi_start(struct srd_decoder_inst *di)
{
    struct fsi_priv *s = (struct fsi_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "fsi");
}

static void fsi_decode(struct srd_decoder_inst *di)
{
    struct fsi_priv *s = (struct fsi_priv *)c_decoder_get_private(di);
    while (1) {
        /* Wait for either clock edge */
        int ret = c_wait(di, CW_E(1), CW_END);
        if (ret != SRD_OK)
            return;

        int data_pin = c_pin(di, 0);
        int clk_pin = c_pin(di, 1);

        /* FSI data is electrically inverted */
        int fsi_data = !data_pin;
        int fsi_clk = clk_pin;
        uint64_t current_sample_number = di_samplenum(di);

        /* Detect BREAK commands (master only, sample on rising clock edge) */
        if (fsi_clk) {
            if (s->fsi_data_break_prev == 1) {
                s->break_counter++;
                if (s->break_counter == 256) {
                    s->ss_block = s->break_start_sample_number;
                    s->es_block = current_sample_number;
                    fsi_putb(di, s, ANN_COMMANDS, "BREAK");
                    s->state = STATE_BREAK_TAR_QUEUED;
                    s->busy_seq_count = 0;
                    s->valid_response = 0;
                    memset(s->prev_address_valid, 0, sizeof(s->prev_address_valid));
                    s->ss_block = current_sample_number;
                }
            } else {
                if (s->break_counter > 256) {
                    s->es_block = current_sample_number;
                    fsi_putb(di, s, ANN_WARNINGS, "BREAK asserted in excess of specification cycles");
                }
                s->break_start_sample_number = current_sample_number;
                s->break_counter = 0;
            }
            s->fsi_data_break_prev = fsi_data;
        }

        /* Master/slave edge selection */
        if ((s->state == STATE_TAR) || (s->state == STATE_RX_SLAVE_ID) ||
            (s->state == STATE_RESPONSE) || (s->state == STATE_RX_DATA) ||
            (s->state == STATE_RX_IPOLL_INTERRUPT_FIELD) ||
            (s->state == STATE_RX_IPOLL_DMA_CONTROL_FIELD) ||
            ((s->state == STATE_CRC) && (s->valid_response))) {
            /* Slave is transmitting, sample on falling clock edge only */
            if (fsi_clk)
                continue;
        } else {
            /* Master is transmitting, sample on rising clock edge only */
            if (!fsi_clk)
                continue;
        }

        /* Transfer state machine */
        switch (s->state) {
        case STATE_IDLE:
            s->crc_internal = 0;
            s->response_received = 0;
            if (s->fsi_data_prev == 1) {
                s->tx_slave_id = 0;
                s->data_count = 2;
                s->ss_block = s->samplenum_prev;
                s->es_block = current_sample_number;
                fsi_putb(di, s, ANN_START, "START");
                s->ss_block = current_sample_number;
                s->crc_calculating = 1;
                s->state = STATE_TX_SLAVE_ID;
            }
            break;

        case STATE_TX_SLAVE_ID:
            s->crc_calculating = 1;
            if (s->data_count > 0) {
                s->tx_slave_id = (s->tx_slave_id >> 1) | (s->fsi_data_prev << 1);
                s->data_count--;
                if (s->data_count == 0) {
                    s->es_block = current_sample_number;
                    fsi_putb_fmt(di, s, ANN_DATA, "Slave ID: 0x%01x", s->tx_slave_id);
                    s->ss_block = current_sample_number;
                    s->command_count = 0;
                    s->command_code = 0;
                    s->command = CMD_NONE;
                    s->valid_command = 0;
                    s->state = STATE_COMMAND;
                }
            }
            break;

        case STATE_COMMAND:
            s->crc_calculating = 1;
            s->command_code = (s->command_code << 1) | s->fsi_data_prev;
            s->command_count++;
            if ((s->command_count == 3) && (s->command_code == 0b100)) {
                s->command = CMD_ABS_ADR;
                s->valid_command = 1;
            } else if ((s->command_count == 3) && (s->command_code == 0b101)) {
                s->command = CMD_REL_ADR;
                s->valid_command = 1;
            } else if ((s->command_count == 2) && (s->command_code == 0b11)) {
                s->command = CMD_SAME_ADR;
                s->valid_command = 1;
            } else if ((s->command_count == 3) && (s->command_code == 0b010)) {
                s->command = CMD_D_POLL;
                s->valid_command = 1;
            } else if ((s->command_count == 3) && (s->command_code == 0b011)) {
                s->command = CMD_E_POLL;
                s->valid_command = 1;
            } else if ((s->command_count == 3) && (s->command_code == 0b001)) {
                s->command = CMD_I_POLL;
                s->valid_command = 1;
            }
            if ((s->command_count > 7) || (s->valid_command)) {
                if (s->command_count == 8) {
                    s->es_block = current_sample_number;
                    fsi_putb_fmt(di, s, ANN_COMMANDS, "Invalid command code: 0x%02x/%d", s->command_code, s->command_count);
                    fsi_putb(di, s, ANN_WARNINGS, "Invalid command code");
                    s->ss_block = current_sample_number;
                    s->state = STATE_IDLE;
                } else {
                    s->es_block = current_sample_number;
                    fsi_putb_fmt(di, s, ANN_COMMANDS, "Command: %s (0x%02x/%d)",
                        fsi_command_name(s->command), s->command_code, s->command_count);
                    s->ss_block = current_sample_number;
                    if (s->command == CMD_ABS_ADR) {
                        s->address_length = 21;
                        s->address_count = 0;
                        s->address = 0;
                        s->state = STATE_DIRECTION;
                    } else if (s->command == CMD_REL_ADR) {
                        s->address_length = 8;
                        s->address_count = 0;
                        s->address = 0;
                        s->state = STATE_DIRECTION;
                    } else if (s->command == CMD_SAME_ADR) {
                        s->address_length = 2;
                        s->address_count = 0;
                        s->address = 0;
                        s->state = STATE_DIRECTION;
                    } else if (s->command == CMD_D_POLL) {
                        s->crc = 0;
                        s->crc_count = 0;
                        s->state = STATE_CRC;
                    } else if (s->command == CMD_E_POLL) {
                        s->crc = 0;
                        s->crc_count = 0;
                        s->state = STATE_CRC;
                    } else if (s->command == CMD_I_POLL) {
                        s->crc = 0;
                        s->crc_count = 0;
                        s->state = STATE_CRC;
                    } else {
                        s->state = STATE_IDLE;
                    }
                }
            }
            break;

        case STATE_DIRECTION:
            s->crc_calculating = 1;
            s->direction = s->fsi_data_prev;
            s->es_block = current_sample_number;
            if (s->direction == 1)
                fsi_putb(di, s, ANN_DIRECTION, "Direction: Read");
            else
                fsi_putb(di, s, ANN_DIRECTION, "Direction: Write");
            s->ss_block = current_sample_number;
            if (s->command == CMD_REL_ADR)
                s->state = STATE_REL_ADDRESS_SIGN;
            else
                s->state = STATE_ADDRESS;
            break;

        case STATE_REL_ADDRESS_SIGN:
            s->crc_calculating = 1;
            s->relative_address_negative = s->fsi_data_prev;
            s->es_block = current_sample_number;
            if (s->relative_address_negative == 1)
                fsi_putb(di, s, ANN_DATA, "Relative address sign: (-)");
            else
                fsi_putb(di, s, ANN_DATA, "Relative address sign: (+)");
            s->ss_block = current_sample_number;
            s->state = STATE_ADDRESS;
            break;

        case STATE_ADDRESS:
            s->crc_calculating = 1;
            s->address = (s->address << 1) | s->fsi_data_prev;
            s->address_count++;
            if (s->address_count >= s->address_length) {
                s->address_raw = s->address;
                if (s->prev_address_valid[s->tx_slave_id]) {
                    if (s->command == CMD_SAME_ADR) {
                        s->address = (s->prev_address[s->tx_slave_id] & ~(uint64_t)0b11) | (s->address_raw & 0b11);
                    } else if (s->command == CMD_REL_ADR) {
                        if (s->relative_address_negative)
                            s->address = s->prev_address[s->tx_slave_id] - (0x100 - s->address_raw);
                        else
                            s->address = s->prev_address[s->tx_slave_id] + s->address_raw;
                    }
                }
                s->es_block = current_sample_number;
                if ((s->command == CMD_SAME_ADR) || (s->command == CMD_REL_ADR)) {
                    fsi_putb_fmt(di, s, ANN_DATA, "Address: 0x%06x (0x%03x)", (unsigned)s->address, (unsigned)s->address_raw);
                    if (!s->prev_address_valid[s->tx_slave_id])
                        fsi_putb(di, s, ANN_WARNINGS, "Base address for relative address not captured");
                } else {
                    fsi_putb_fmt(di, s, ANN_DATA, "Address: 0x%06x", (unsigned)s->address);
                }
                s->ss_block = current_sample_number;
                s->state = STATE_DATA_SIZE;
            }
            break;

        case STATE_DATA_SIZE:
            s->crc_calculating = 1;
            if (s->direction && ((s->address_raw & 3) == 3) && s->fsi_data_prev) {
                /* TERM command detected */
                s->command_code = 0b111111;
                s->command_count = 6;
                s->command = CMD_TERM;
                s->es_block = current_sample_number;
                fsi_putb_fmt(di, s, ANN_COMMANDS, "Command: %s (0x%02x/%d)",
                    fsi_command_name(s->command), s->command_code, s->command_count);
                s->ss_block = current_sample_number;
                s->direction = 0;
                s->busy_seq_count = 0;
                s->crc = 0;
                s->crc_count = 0;
                s->state = STATE_CRC;
            } else {
                if (s->fsi_data_prev == 0) {
                    s->data_size = SIZE_BYTE;
                } else {
                    if ((s->address_raw & 3) == 1) {
                        s->data_size = SIZE_WORD;
                        s->address = (s->address & ~(uint64_t)3) | 1;
                    } else if ((s->address_raw & 1) == 0) {
                        s->data_size = SIZE_HALF_WORD;
                        s->address = s->address & ~(uint64_t)1;
                    } else {
                        s->data_size = SIZE_UNKNOWN;
                    }
                }
                s->es_block = current_sample_number;
                if (s->data_size == SIZE_UNKNOWN) {
                    fsi_putb(di, s, ANN_WARNINGS, "Data Size: UNKNOWN");
                    s->state = STATE_IDLE;
                } else {
                    fsi_putb_fmt(di, s, ANN_DIRECTION, "Data Size: %s", fsi_data_size_name(s->data_size));
                    if (s->direction == 1) {
                        s->crc = 0;
                        s->crc_count = 0;
                        s->state = STATE_CRC;
                    } else {
                        s->data = 0;
                        s->data_count = 0;
                        if (s->data_size == SIZE_BYTE)
                            s->data_length = 8;
                        else if (s->data_size == SIZE_HALF_WORD)
                            s->data_length = 16;
                        else if (s->data_size == SIZE_WORD)
                            s->data_length = 32;
                        s->state = STATE_TX_DATA;
                    }
                }
                s->ss_block = current_sample_number;
            }
            break;

        case STATE_TX_DATA:
            s->crc_calculating = 1;
            s->data = (s->data << 1) | s->fsi_data_prev;
            s->data_count++;
            if (s->data_count >= s->data_length) {
                s->es_block = current_sample_number;
                if (s->data_size == SIZE_BYTE)
                    fsi_putb_fmt(di, s, ANN_DATA, "Data: 0x%02x", (unsigned)s->data);
                else if (s->data_size == SIZE_HALF_WORD)
                    fsi_putb_fmt(di, s, ANN_DATA, "Data: 0x%04x", (unsigned)s->data);
                else
                    fsi_putb_fmt(di, s, ANN_DATA, "Data: 0x%08x", (unsigned)s->data);
                s->ss_block = current_sample_number;
                s->crc = 0;
                s->crc_count = 0;
                s->state = STATE_CRC;
            }
            break;

        case STATE_CRC:
            if (s->crc_count == 0) {
                s->computed_crc_tx_end = s->crc_internal;
            }
            s->crc_calculating = 1;
            s->crc = (s->crc << 1) | s->fsi_data_prev;
            s->crc_count++;
            if (s->crc_count >= 4) {
                s->es_block = current_sample_number;
                if (s->crc == s->computed_crc_tx_end) {
                    fsi_putb_fmt(di, s, ANN_CRC, "CRC: 0x%01x (GOOD)", s->crc);
                    if (s->response_received) {
                        if (((s->command == CMD_ABS_ADR) || (s->command == CMD_REL_ADR) || (s->command == CMD_SAME_ADR))
                            && ((s->response == RSP_ACK_D) || (s->response == RSP_ACK))) {
                            s->prev_address[s->tx_slave_id] = s->address;
                            s->prev_address_valid[s->tx_slave_id] = 1;
                        }
                    }
                } else {
                    fsi_putb_fmt(di, s, ANN_CRC, "CRC: 0x%01x (BAD)", s->crc);
                    fsi_putb(di, s, ANN_WARNINGS, "Bad CRC");
                }
                s->ss_block = current_sample_number;
                s->tar_timer = 0;
                s->state = STATE_TAR;
                s->timeout_counter = 0;
            }
            break;

        case STATE_BREAK_TAR_QUEUED:
            s->tar_timer = 0;
            s->state = STATE_BREAK_TAR;
            break;

        case STATE_BREAK_TAR:
            s->tar_timer++;
            if (s->tar_timer > s->tar_cycles) {
                s->crc_calculating = 0;
                s->crc_internal = 0;
                s->es_block = current_sample_number;
                fsi_putb(di, s, ANN_TAR, "TAR");
                s->ss_block = current_sample_number;
                s->state = STATE_IDLE;
            }
            break;

        case STATE_TAR:
            s->crc_calculating = 0;
            s->crc_internal = 0;
            s->tar_timer++;
            if (s->tar_timer > s->tar_cycles) {
                if (s->response_received == 1) {
                    s->response_received = 0;
                    if (s->rx_slave_id == s->tx_slave_id) {
                        if (s->response == RSP_BUSY)
                            s->busy_seq_count++;
                        else
                            s->busy_seq_count = 0;
                        s->state = STATE_IDLE;
                    }
                }
                if (s->timeout_counter == 0) {
                    s->es_block = s->samplenum_prev;
                    fsi_putb(di, s, ANN_TAR, "TAR");
                    s->ss_block = current_sample_number;
                }
                if (s->fsi_data_prev == 1) {
                    s->crc_calculating = 1;
                    s->rx_slave_id = 0;
                    s->data_count = 2;
                    if (s->state == STATE_IDLE) {
                        s->state = STATE_TX_SLAVE_ID;
                    } else {
                        s->state = STATE_RX_SLAVE_ID;
                    }
                    s->ss_block = s->samplenum_prev;
                    s->es_block = current_sample_number;
                    fsi_putb(di, s, ANN_START, "START");
                    s->ss_block = current_sample_number;
                } else {
                    s->timeout_counter++;
                    if (s->timeout_counter >= 256) {
                        s->es_block = current_sample_number;
                        fsi_putb(di, s, ANN_TAR, "Response timeout");
                        fsi_putb(di, s, ANN_WARNINGS, "Response timeout");
                        s->state = STATE_IDLE;
                    }
                }
            }
            break;

        case STATE_RX_SLAVE_ID:
            s->crc_calculating = 1;
            s->response_received = 1;
            if (s->data_count > 0) {
                s->rx_slave_id = (s->rx_slave_id >> 1) | (s->fsi_data_prev << 1);
                s->data_count--;
                if (s->data_count == 0) {
                    s->es_block = current_sample_number;
                    fsi_putb_fmt(di, s, ANN_DATA, "Slave ID: 0x%01x", s->rx_slave_id);
                    if (s->rx_slave_id != s->tx_slave_id)
                        fsi_putb(di, s, ANN_WARNINGS, "Slave ID does not match active transaction");
                    s->ss_block = current_sample_number;
                    s->response_count = 0;
                    s->response_code = 0;
                    s->response = RSP_NONE;
                    s->valid_response = 0;
                    s->state = STATE_RESPONSE;
                }
            }
            break;

        case STATE_RESPONSE:
            s->crc_calculating = 1;
            s->response_code = (s->response_code << 1) | s->fsi_data_prev;
            s->response_count++;
            if ((s->command == CMD_I_POLL) && (s->rx_slave_id == s->tx_slave_id) &&
                (s->response_count == 1) && (s->response_code == 0b0)) {
                s->response = RSP_I_POLL_RSP;
                s->valid_response = 1;
            } else if ((s->response_count == 2) && (s->response_code == 0b00)) {
                if (s->direction == 1)
                    s->response = RSP_ACK_D;
                else
                    s->response = RSP_ACK;
                s->valid_response = 1;
            } else if ((s->response_count == 2) && (s->response_code == 0b01)) {
                s->response = RSP_BUSY;
                s->valid_response = 1;
            } else if ((s->response_count == 2) && (s->response_code == 0b10)) {
                s->response = RSP_ERR_A;
                s->valid_response = 1;
            } else if ((s->response_count == 2) && (s->response_code == 0b11)) {
                s->response = RSP_ERR_C;
                s->valid_response = 1;
            }
            if ((s->response_count > 2) || (s->valid_response)) {
                if (s->response_count == 8) {
                    s->es_block = current_sample_number;
                    fsi_putb_fmt(di, s, ANN_COMMANDS, "Invalid response code: 0x%02x/%d", s->response_code, s->response_count);
                    fsi_putb(di, s, ANN_WARNINGS, "Invalid response code");
                    s->ss_block = current_sample_number;
                    s->state = STATE_IDLE;
                } else {
                    s->es_block = current_sample_number;
                    fsi_putb_fmt(di, s, ANN_COMMANDS, "Response: %s (0x%02x/%d)",
                        fsi_response_name(s->response), s->response_code, s->response_count);
                    s->ss_block = current_sample_number;
                    if (s->response == RSP_ACK_D) {
                        s->data = 0;
                        s->data_count = 0;
                        if (s->data_size == SIZE_BYTE)
                            s->data_length = 8;
                        else if (s->data_size == SIZE_HALF_WORD)
                            s->data_length = 16;
                        else if (s->data_size == SIZE_WORD)
                            s->data_length = 32;
                        s->state = STATE_RX_DATA;
                    } else if (s->response == RSP_ACK) {
                        s->crc = 0;
                        s->crc_count = 0;
                        s->state = STATE_CRC;
                    } else if (s->response == RSP_BUSY) {
                        s->crc = 0;
                        s->crc_count = 0;
                        s->state = STATE_CRC;
                    } else if (s->response == RSP_ERR_A) {
                        s->crc = 0;
                        s->crc_count = 0;
                        s->state = STATE_CRC;
                    } else if (s->response == RSP_ERR_C) {
                        s->crc = 0;
                        s->crc_count = 0;
                        s->state = STATE_CRC;
                    } else if (s->response == RSP_I_POLL_RSP) {
                        s->data = 0;
                        s->data_count = 0;
                        s->data_length = 2;
                        s->state = STATE_RX_IPOLL_INTERRUPT_FIELD;
                    } else {
                        s->state = STATE_IDLE;
                    }
                }
            }
            break;

        case STATE_RX_DATA:
            s->crc_calculating = 1;
            s->data = (s->data << 1) | s->fsi_data_prev;
            s->data_count++;
            if (s->data_count >= s->data_length) {
                s->es_block = current_sample_number;
                fsi_putb_fmt(di, s, ANN_DATA, "Data: 0x%08x", (unsigned)s->data);
                s->ss_block = current_sample_number;
                s->crc = 0;
                s->crc_count = 0;
                s->state = STATE_CRC;
            }
            break;

        case STATE_RX_IPOLL_INTERRUPT_FIELD:
            s->crc_calculating = 1;
            s->data = (s->data << 1) | s->fsi_data_prev;
            s->data_count++;
            if (s->data_count >= s->data_length) {
                s->es_block = current_sample_number;
                fsi_putb_fmt(di, s, ANN_DATA, "Interrupt Field: 0x%01x", (unsigned)s->data);
                s->ss_block = current_sample_number;
                s->data = 0;
                s->data_count = 0;
                s->data_length = 3;
                s->state = STATE_RX_IPOLL_DMA_CONTROL_FIELD;
            }
            break;

        case STATE_RX_IPOLL_DMA_CONTROL_FIELD:
            s->crc_calculating = 1;
            s->data = (s->data << 1) | s->fsi_data_prev;
            s->data_count++;
            if (s->data_count >= s->data_length) {
                s->es_block = current_sample_number;
                fsi_putb_fmt(di, s, ANN_DATA, "DMA Control Field: 0x%01x", (unsigned)s->data);
                s->ss_block = current_sample_number;
                s->crc = 0;
                s->crc_count = 0;
                s->state = STATE_CRC;
            }
            break;

        default:
            break;
        }

        /* CRC calculation - Galois LFSR, polynomial 0x7 (MSB first) */
        {
            int crc_prev = s->crc_internal;
            int crc_feedback = 0;
            if (s->crc_calculating) {
                crc_feedback = (((crc_prev >> 3) & 1) ^ s->fsi_data_prev) & 1;
            }
            if (s->crc_calculating) {
                s->crc_internal = (s->crc_internal & ~(1 << 0)) | ((crc_feedback & 1) << 0);
                s->crc_internal = (s->crc_internal & ~(1 << 1)) | ((((crc_prev & 1) ^ crc_feedback) & 1) << 1);
                s->crc_internal = (s->crc_internal & ~(1 << 2)) | (((((crc_prev >> 1) & 1) ^ crc_feedback) & 1) << 2);
                s->crc_internal = (s->crc_internal & ~(1 << 3)) | ((((crc_prev >> 2) & 1) & 1) << 3);
            }
        }

        s->fsi_data_prev = fsi_data;
        s->samplenum_prev = current_sample_number;
    }
}

static void fsi_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder fsi_c_decoder = {
    .id = "fsi_c",
    .name = "FSI(C)",
    .longname = "Flexible Service Interface (C)",
    .desc = "Protocol for FSI devices on Raptor OpenPOWER systems. (C implementation)",
    .license = "gplv2+",
    .channels = fsi_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = fsi_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = fsi_ann_rows,
    .inputs = fsi_inputs,
    .num_inputs = 1,
    .outputs = fsi_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = fsi_tags,
    .num_tags = 1,
    .reset = fsi_reset,
    .start = fsi_start,
    .decode = fsi_decode,
    .destroy = fsi_destroy,
    .state_size = 0,
    .metadata = NULL,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &fsi_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}