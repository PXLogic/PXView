/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2022 Shawn Walker <ac0bi00@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#define _GNU_SOURCE

#include <config.h>
#include <errno.h>
#include <glib.h>
#include <math.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "hardware/compat/compat.h"
#include "protocol.h"

SR_PRIV int send_serial_str(struct sr_serial_dev_inst *serial, char *str)
{
	int len = strlen(str);
	if ((len > 15) || (len < 1)) {
		sr_err("ERROR: Serial string len %d invalid ", len);
		return SR_ERR;
	}

	/* 100ms timeout. With USB CDC serial we can't define the timeout based
	 * on link rate, so just pick something large as we shouldn't normally
	 * see them */
	if (serial_write_blocking(serial, str, len, 100) != len) {
		sr_err("ERROR: Serial str write failed");
		return SR_ERR;
	}

	return SR_OK;
}

SR_PRIV int send_serial_char(struct sr_serial_dev_inst *serial, char ch)
{
	char buf[1];
	buf[0] = ch;

	if (serial_write_blocking(serial, buf, 1, 100) != 1) {	/* 100ms */
		sr_err("ERROR: Serial char write failed");
		return SR_ERR;
	}

	return SR_OK;
}

/* Issue a command that expects a string return that is less than 30 characters.
 * Returns the length of string */
int send_serial_w_resp(struct sr_serial_dev_inst *serial, char *str,
	char *resp, size_t cnt)
{
	int num_read, i;
	send_serial_str(serial, str);

	/* Using the serial_read_blocking function when reading a response of
	 * unknown length requires a long worst case timeout to always be taken.
	 * So, instead loop waiting for a first byte, and then a final small delay
	 * for the rest. */
	for (i = 0; i < 1000; i++) {	/* wait up to 1 second in ms increments */
		num_read = serial_read_blocking(serial, resp, cnt, 1);
		if (num_read > 0)
			break;
	}

	/* Since the serial port is USB CDC we can't calculate timeouts based on
	 * baud rate but even if the response is split between two USB transfers,
	 * 10ms should be plenty. */
	num_read += serial_read_blocking(serial, &(resp[num_read]), cnt - num_read,
		10);
	if ((num_read < 1) || (num_read > 30)) {
		sr_err("ERROR: Serial_w_resp failed (%d).", num_read);
		return -1;
	} else
		return num_read;
}

/* Issue a command that expects a single char ack */
SR_PRIV int send_serial_w_ack(struct sr_serial_dev_inst *serial, char *str)
{
	char buf[2];
	int num_read;

	/* In case we have left over transfer from the device, drain them.
	 * These should not exist in normal operation */
	while ((num_read = serial_read_blocking(serial, buf, 2, 10)))
		sr_dbg("swack drops 2 bytes %d %d", buf[0], buf[1]);

	send_serial_str(serial, str);

	/* 1000ms timeout */
	num_read = serial_read_blocking(serial, buf, 1, 1000);

	if ((num_read == 1) && (buf[0] == '*')) {
		return SR_OK;
	} else {
		sr_err("ERROR: Serial_w_ack %s failed (%d).", str, num_read);
		if (num_read)
			sr_err("ack resp char %c d %d", buf[0], buf[0]);
		return SR_ERR;
	}
}

/* Process incoming data stream assuming it is optimized packing of 4 channels
 * or less. */
void process_D4(struct sr_dev_inst *sdi, struct dev_context *d)
{
	uint32_t j;
	uint8_t cbyte, cval;
	uint32_t rlecnt = 0;

	while (d->ser_rdptr < d->bytes_avail) {
		cbyte = d->buffer[(d->ser_rdptr)];

		/*RLE only byte */
		if ((cbyte >= 48) && (cbyte <= 127)) {
			rlecnt += (cbyte - 47) * 8;
			d->byte_cnt++;
		} else if (cbyte >= 0x80) {	/* sample with possible rle */
			rlecnt += (cbyte & 0x70) >> 4;
			if (rlecnt) {
				rle_memset(d, rlecnt);
				rlecnt = 0;
			}
			cval = cbyte & 0xF;
			uint32_t didx = (d->cbuf_wrptr) * (d->dig_sample_bytes);
			d->d_data_buf[didx] = cval;

			for (j = 1; j < d->dig_sample_bytes; j++)
				d->d_data_buf[didx+j] = 0;

			d->byte_cnt++;
			sr_spew("Dchan4 rdptr %d wrptr %d bytein 0x%X rle %d cval 0x%X didx %d",
				(d->ser_rdptr) - 1, d->cbuf_wrptr, cbyte, rlecnt, cval, didx);
			d->cbuf_wrptr++;
			rlecnt = 0;
			d->d_last[0] = cval;
		} else {
			if (cbyte == '$') {
				sr_info("D4 Data stream stops with cbyte %d char %c rdidx %d cnt %lu",
					cbyte, cbyte, d->ser_rdptr, d->byte_cnt);
				d->rxstate = RX_STOPPED;
			} else {
				sr_err("D4 Data stream aborts with cbyte %d char %c rdidx %d cnt %lu",
					cbyte, cbyte, d->ser_rdptr, d->byte_cnt);
				d->rxstate = RX_ABORT;
			}
			break;
		}

		(d->ser_rdptr)++;
		if ((rlecnt >= 2000) || \
			((rlecnt + ((d->cbuf_wrptr) <<2 ))) > (d->sample_buf_size - 1024)) {
			sr_spew("D4 preoverflow wrptr %d bufsize %d rlecnt %d\n\r",
				d->cbuf_wrptr, d->sample_buf_size, rlecnt);
			rle_memset(d, rlecnt);
			process_group(sdi, d, d->cbuf_wrptr);
			rlecnt = 0;
		}

	}

	sr_spew("D4 while done rdptr %d", d->ser_rdptr);

	if (rlecnt) {
		sr_spew("Residual D4 slice rlecnt %d", rlecnt);
		rle_memset(d, rlecnt);
	}
	if (d->cbuf_wrptr) {
		sr_spew("Residual D4 data wrptr %d", d->cbuf_wrptr);
		process_group(sdi, d, d->cbuf_wrptr);
	}
}

/* Process incoming data stream and forward to trigger processing with
 * process_group */
void process_slice(struct sr_dev_inst *sdi, struct dev_context *devc)
{
	int32_t i;
	uint32_t tmp32, cword;
	uint8_t cbyte;
	uint32_t slice_bytes;

	for (slice_bytes = 1; (slice_bytes < devc->bytes_avail)
		&& (devc->buffer[slice_bytes - 1] >= 0x30); slice_bytes++);

	if (slice_bytes != devc->bytes_avail) {
		cbyte = devc->buffer[slice_bytes - 1];
		slice_bytes--;
		if (cbyte == '$') {
			sr_info("Data stream stops with cbyte %d char %c rdidx %d sbytes %d cnt %lu",
				cbyte, cbyte, devc->ser_rdptr, slice_bytes, devc->byte_cnt);
			devc->rxstate = RX_STOPPED;
		} else {
			sr_err("Data stream aborts with cbyte %d char %c rdidx %d sbytes %d cnt %lu",
				cbyte, cbyte, devc->ser_rdptr, slice_bytes, devc->byte_cnt);
			devc->rxstate = RX_ABORT;
		}
	}

	devc->byte_cnt += slice_bytes - (devc->wrptr);

	sr_spew("process slice avail %d rdptr %d sb %d byte_cnt %" PRIu64 "",
		devc->bytes_avail, devc->ser_rdptr, slice_bytes, devc->byte_cnt);

	while (((devc->ser_rdptr + devc->bytes_per_slice) <= slice_bytes)
		|| ((devc->ser_rdptr < slice_bytes) &&
			(devc->buffer[devc->ser_rdptr] < 0x80))) {

	if (devc->buffer[devc->ser_rdptr] < 0x80) {
		int16_t rlecnt;
		if (devc->buffer[devc->ser_rdptr] <= 79)
			rlecnt = devc->buffer[devc->ser_rdptr] - 47;
		else
			rlecnt = (devc->buffer[devc->ser_rdptr] - 78) * 32;

		sr_info("RLEcnt of %d in %d", rlecnt, devc->buffer[devc->ser_rdptr]);
		if ((rlecnt < 1) || (rlecnt > 1568))
			sr_err("Bad rlecnt val %d in %d",
				rlecnt, devc->buffer[devc->ser_rdptr]);
		else
			rle_memset(devc,rlecnt);

		devc->ser_rdptr++;

	} else {
		cword = 0;
		for (i = 0; i < devc->num_d_channels; i += 7) {
			if (((devc->d_chan_mask) >> i) & 0x7F) {
				cword |= ((devc->buffer[devc->ser_rdptr]) & 0x7F) << i;
				(devc->ser_rdptr)++;
			}
		}
		devc->d_last[0] =  cword        & 0xFF;
		devc->d_last[1] = (cword >> 8)  & 0xFF;
		devc->d_last[2] = (cword >> 16) & 0xFF;
		devc->d_last[3] = (cword >> 24) & 0xFF;

		for (i = 0; i < devc->num_d_channels; i += 8) {
			uint32_t idx = ((devc->cbuf_wrptr) * devc->dig_sample_bytes) +
				(i >> 3);
			devc->d_data_buf[idx] = cword & 0xFF;
			sr_spew("Dchan i %d wrptr %d idx %d char 0x%X cword 0x%X",
				i, devc->cbuf_wrptr, idx, devc->d_data_buf[idx], cword);
			cword >>= 8;
		}

		for (i = 0; i < devc->num_a_channels; i++) {
			if ((devc->a_chan_mask >> i) & 1) {

				tmp32 =
				    devc->buffer[devc->ser_rdptr] - 0x80;
				for(int a=1;a<devc->a_size;a++){
                                    tmp32+=(devc->buffer[(devc->ser_rdptr)+a] - 0x80)<<(7*a);
                                }
				devc->a_data_bufs[i][devc->cbuf_wrptr] =
				    ((float) tmp32 * devc->a_scale[i]) +
				    devc->a_offset[i];
				devc->a_last[i] =
				    devc->a_data_bufs[i][devc->cbuf_wrptr];
				sr_spew
				    ("AChan %d t32 %d value %f wrptr %d rdptr %d sc %f off %f",
				     i, tmp32,
				     devc->
				     a_data_bufs[i][devc->cbuf_wrptr],
				     devc->cbuf_wrptr, devc->ser_rdptr,
				     devc->a_scale[i], devc->a_offset[i]);
				devc->ser_rdptr+=devc->a_size;
			}
		}
		devc->cbuf_wrptr++;
	  }
           if((devc->cbuf_wrptr +2048) >  devc->sample_buf_size){
              sr_spew("Drain large buff %d %d\n\r",devc->cbuf_wrptr,devc->sample_buf_size);
              process_group(sdi, devc, devc->cbuf_wrptr);

           }
	}
	if (devc->cbuf_wrptr){
		process_group(sdi, devc, devc->cbuf_wrptr);
	}

}

/* Send the processed analog values to the session.
 * Adapted for PXView: uses PXView's sr_datafeed_analog struct (probes field
 * instead of meaning->channels, no sr_analog_init/encoding/meaning/spec). */
int send_analog(struct sr_dev_inst *sdi, struct dev_context *devc,
		uint32_t num_samples, uint32_t offset)
{
	struct sr_datafeed_packet packet;
	struct sr_datafeed_analog analog;
	struct sr_channel *ch;
	uint32_t i;
	float *fptr;

	for (i = 0; i < devc->num_a_channels; i++) {
		if ((devc->a_chan_mask >> i) & 1) {
			ch = devc->analog_groups[i]->channels->data;
			analog.probes = g_slist_append(NULL, ch);
			analog.num_samples = num_samples;
			analog.data = (devc->a_data_bufs[i]) + offset;
			analog.unit_bits = 32; /* float */
			analog.unit_pitch = 0;
			analog.mq = SR_MQ_VOLTAGE;
			analog.unit = SR_UNIT_VOLT;
			analog.mqflags = 0;
			fptr = analog.data;
			sr_spew
			    ("send analog num %d offset %d first %f 2 %f",
			     num_samples, offset, *(devc->a_data_bufs[i]),
			     *fptr);
			packet.type = SR_DF_ANALOG;
			packet.status = SR_PKT_OK;
			packet.payload = &analog;
			ds_data_forward(sdi, &packet);
			g_slist_free(analog.probes);
		}
	}
	return 0;

}

/* Send the ring buffer of pre-trigger analog samples.
 * Adapted for PXView: uses PXView's sr_datafeed_analog struct. */
int send_analog_ring(struct sr_dev_inst *sdi, struct dev_context *devc,
		     uint32_t num_samples)
{
	struct sr_datafeed_packet packet;
	struct sr_datafeed_analog analog;
	struct sr_channel *ch;
	int i;
	uint32_t num_pre, start_pre;
	uint32_t num_post, start_post;
	num_pre =
	    (num_samples >=
	     devc->pretrig_wr_ptr) ? devc->pretrig_wr_ptr : num_samples;
	start_pre = devc->pretrig_wr_ptr - num_pre;
	num_post = num_samples - num_pre;
	start_post = devc->pretrig_entries - num_post;
	sr_spew
	    ("send_analog ring wrptr %u ns %d npre %u spre %u npost %u spost %u",
	     devc->pretrig_wr_ptr, num_samples, num_pre, start_pre,
	     num_post, start_post);
	float *fptr;
	for (i = 0; i < devc->num_a_channels; i++) {
		if ((devc->a_chan_mask >> i) & 1) {
			ch = devc->analog_groups[i]->channels->data;
			analog.probes = g_slist_append(NULL, ch);
			analog.mq = SR_MQ_VOLTAGE;
			analog.unit = SR_UNIT_VOLT;
			analog.mqflags = 0;
			analog.unit_bits = 32; /* float */
			analog.unit_pitch = 0;
			packet.type = SR_DF_ANALOG;
			packet.status = SR_PKT_OK;
			packet.payload = &analog;
			if (num_post) {
				analog.num_samples = num_post;
				analog.data =
				    (devc->a_pretrig_bufs[i]) + start_post;
				for (uint32_t j = 0;
				     j < analog.num_samples; j++) {
					fptr =
					    analog.data +
					    (j * sizeof(float));
				}
				ds_data_forward(sdi, &packet);
			}
			if (num_pre) {
				analog.num_samples = num_pre;
				analog.data =
				    (devc->a_pretrig_bufs[i]) + start_pre;
				sr_dbg("Sending A%d ring buffer newest ",
				       i);
				for (uint32_t j = 0;
				     j < analog.num_samples; j++) {
					fptr =
					    analog.data +
					    (j * sizeof(float));
					sr_spew("RNGDCW%d j %d %f %p", i,
						j, *fptr, (void *) fptr);
				}
				ds_data_forward(sdi, &packet);
			}
			g_slist_free(analog.probes);
			sr_dbg("Sending A%d ring buffer done ", i);
		}
	}
	return 0;

}

/* Given a chunk of slices forward to trigger check or session as appropriate and update state.
 * Adapted for PXView: uses PXView's sr_datafeed_logic struct (with extra fields). */
int process_group(struct sr_dev_inst *sdi, struct dev_context *devc,
		  uint32_t num_slices)
{
	int trigger_offset;
	int pre_trigger_samples;
	size_t num_samples;
	struct sr_datafeed_logic logic;
	struct sr_datafeed_packet packet;
	int i;
	size_t cbuf_wrptr_cpy;
	cbuf_wrptr_cpy = devc->cbuf_wrptr;
	devc->cbuf_wrptr = 0;
	if (devc->trigger_fired) {
		if (devc->limit_samples &&
		    num_slices >
		    devc->limit_samples - devc->sent_samples) {
			num_samples =
			    devc->limit_samples - devc->sent_samples;
		} else {
			num_samples = num_slices;
		}
		if (num_samples > 0) {
			sr_spew("Process_group sending %lu post trig samples dsb %d",
				num_samples, devc->dig_sample_bytes);
			if (devc->num_d_channels) {
				packet.type = SR_DF_LOGIC;
				packet.status = SR_PKT_OK;
				packet.payload = &logic;
				logic.unitsize = devc->dig_sample_bytes;
				logic.length = num_samples * logic.unitsize;
				logic.data = devc->d_data_buf;
				logic.format = 0;
				logic.index = 0;
				logic.order = 0;
				logic.data_error = 0;
				logic.error_pattern = 0;
				ds_data_forward(sdi, &packet);
			}
			send_analog(sdi, devc, num_samples, 0);
		}

		devc->sent_samples += num_samples;
		return 0;

	} else {
		size_t num_ring_samples;
		size_t sptr, eptr;
		size_t numtail, numwrap;
		size_t srcptr;
		trigger_offset = soft_trigger_logic_check(devc->stl, devc->d_data_buf,
			num_slices * devc->dig_sample_bytes, &pre_trigger_samples);

		if (trigger_offset > -1) {
			devc->trigger_fired = TRUE;
			devc->sent_samples += pre_trigger_samples;
			packet.type = SR_DF_LOGIC;
			packet.status = SR_PKT_OK;
			packet.payload = &logic;
			num_samples = num_slices - trigger_offset;

			if (devc->limit_samples && \
				(num_samples > devc->limit_samples - devc->sent_samples))
				num_samples = devc->limit_samples - devc->sent_samples;

			if (num_samples > 0) {
				sr_dbg("Sending post trigger logical remainder of %lu",
					num_samples);
				logic.length = num_samples * devc->dig_sample_bytes;
				logic.unitsize = devc->dig_sample_bytes;
				logic.data = devc->d_data_buf +
					(trigger_offset * devc->dig_sample_bytes);
				logic.format = 0;
				logic.index = 0;
				logic.order = 0;
				logic.data_error = 0;
				logic.error_pattern = 0;
				devc->sent_samples += num_samples;
				ds_data_forward(sdi, &packet);
			}

			size_t new_start, new_end, new_samples, ring_samples;
			new_start = (trigger_offset > (int)devc->pretrig_entries) ?
				trigger_offset - devc->pretrig_entries : 0;

			new_end = MIN(num_slices - 1,
				devc->limit_samples - (pre_trigger_samples - trigger_offset) - 1);

			new_samples = new_end - new_start + 1;

			ring_samples = (pre_trigger_samples > trigger_offset) ?
				pre_trigger_samples - trigger_offset : 0;
			sr_spew("SW trigger float info newstart %zu new_end %zu " \
					"new_samp %zu ring_samp %zu",
				new_start, new_end, new_samples, ring_samples);

			if (ring_samples > 0)
				send_analog_ring(sdi, devc, ring_samples);
			if (new_samples)
				send_analog(sdi, devc, new_samples, new_start);
		} else {
			if ((devc->a_chan_mask) && (devc->pretrig_entries)) {
				num_ring_samples = num_slices > devc->pretrig_entries ?
					devc->pretrig_entries : num_slices;
				sptr = devc->pretrig_wr_ptr;

				eptr = (sptr + num_ring_samples) >= devc-> pretrig_entries ?
					devc->pretrig_entries - 1 : sptr + num_ring_samples - 1;

				numtail = (eptr - sptr) + 1;

				numwrap = (num_ring_samples > numtail) ?
					num_ring_samples - numtail : 0;

				srcptr = cbuf_wrptr_cpy - num_ring_samples;
				sr_spew("RNG num %zu sptr %zu eptr %zu ",
					num_ring_samples, sptr, eptr);

				for (i = 0; i < devc->num_a_channels; i++)
					if ((devc->a_chan_mask >> i) & 1)
						for (uint32_t j = 0; j < numtail; j++)
							devc->a_pretrig_bufs[i][sptr + j] =
								devc->a_data_bufs[i][srcptr + j];

				srcptr += numtail;
				for (i = 0; i < devc->num_a_channels; i++)
					if ((devc->a_chan_mask >> i) & 1)
						for (uint32_t j = 0; j < numwrap; j++)
							devc->a_pretrig_bufs[i][j] =
								devc->a_data_bufs[i][srcptr + j];

				devc->pretrig_wr_ptr = (numwrap) ?
					numwrap : (eptr + 1) % devc->pretrig_entries;
			}
		}
	}

	return 0;
}

/* Duplicate previous sample values */
void rle_memset(struct dev_context *devc, uint32_t num_slices)
{
	uint32_t j, k, didx;
	sr_spew("rle_memset vals 0x%X, 0x%X, 0x%X slices %d dsb %d",
		devc->d_last[0], devc->d_last[1], devc->d_last[2],
		num_slices, devc->dig_sample_bytes);

	for (j = 0; j < num_slices; j++) {
		didx = devc->cbuf_wrptr * devc->dig_sample_bytes;
		for (k = 0; k < devc->dig_sample_bytes; k++)
			devc->d_data_buf[didx + k] =  devc->d_last[k];
		devc->cbuf_wrptr++;
	}
}

/* This callback function is mapped from api.c with serial_source_add and is
 * created after a capture has been setup and is responsible for querying the
 * device trigger status, downloading data and forwarding packets */
SR_PRIV int raspberrypi_pico_receive(int fd, int revents, void *cb_data)
{
	struct sr_dev_inst *sdi;
	struct dev_context *devc;
	struct sr_serial_dev_inst *serial;
	int len;
	uint32_t i, bytes_rem, residual_bytes;
	(void) fd;

	if (!(sdi = cb_data))
		return TRUE;

	if (!(devc = sdi->priv))
		return TRUE;

	if (devc->rxstate != RX_ACTIVE) {
		sr_dbg("Reached non active state in receive %d", devc->rxstate);
	}

	if (devc->rxstate == RX_IDLE) {
		sr_dbg("Reached idle state in receive %d", devc->rxstate);
		return FALSE;
	}

	serial = sdi->conn;

	if (!(revents == G_IO_IN || revents == 0))
		return TRUE;

	bytes_rem = devc->serial_buffer_size - devc->wrptr;

	len = serial_read_blocking(serial, &(devc->buffer[devc->wrptr]),
		bytes_rem - 1, 10);
	sr_spew("Entry wrptr %u bytes_rem %u len %d", devc->wrptr, bytes_rem, len);

	if (len > 0) {
		devc->buffer[devc->wrptr + len] = 0;
		sr_dbg("rx string %s#", devc->buffer);
		devc->bytes_avail = (devc->wrptr + len);
		sr_spew("rx len %d bytes_avail %ul sent_samples %ul wrptr %u",
			len, devc->bytes_avail, devc->sent_samples, devc->wrptr);
	} else {
		if (len == 0) {
			return TRUE;
		} else {
			sr_err("ERROR: Negative serial read code %d", len);
			sdi->driver->dev_acquisition_stop(sdi, NULL);
			return FALSE;
		}
	}

	devc->ser_rdptr = 0;
	if (devc->rxstate == RX_ACTIVE) {
		if ((devc->a_chan_mask == 0) \
			&& ((devc->d_chan_mask & 0xFFFFFFF0) == 0))
			process_D4(sdi, devc);
		else
			process_slice(sdi, devc);
	}

	residual_bytes = devc->bytes_avail - devc->ser_rdptr;
	if (residual_bytes) {
		for (i = 0; i < residual_bytes; i++)
			devc->buffer[i] = devc->buffer[i + devc->ser_rdptr];

		devc->ser_rdptr = 0;
		devc->wrptr = residual_bytes;
		sr_spew("Residual shift rdptr %u wrptr %u", devc->ser_rdptr, devc->wrptr);
	} else {
		devc->wrptr = 0;
	}

	if (devc->rxstate == RX_ABORT) {
		sr_err("Ending receive on abort");
		sdi->driver->dev_acquisition_stop(sdi, NULL);
		return FALSE;
	}

	if (devc->rxstate == RX_STOPPED) {
		sr_dbg("Stopped, checking byte_cnt");
		if (devc->buffer[0] != '$') {
			sr_err("ERROR: Stop marker should be byte zero");
			devc->rxstate = RX_ABORT;
			sdi->driver->dev_acquisition_stop(sdi, NULL);
			return FALSE;
		}

		for (i = 1; i < devc->wrptr; i++) {
			if (devc->buffer[i] == '+') {
				devc->buffer[i] = 0;
				uint64_t rxbytecnt;
				rxbytecnt = atol((char*)&(devc->buffer[1]));
				sr_dbg("Byte_cnt check device cnt %lu host cnt %lu",
					rxbytecnt, devc->byte_cnt);
				if (rxbytecnt != devc->byte_cnt)
					sr_err("ERROR: received %lu and counted %lu bytecnts " \
							"don't match, data may be lost",
						rxbytecnt, devc->byte_cnt);

				devc->rxstate = RX_IDLE;

				sdi->driver->dev_acquisition_stop(sdi, NULL);
				return TRUE;
			}
		}

		sr_dbg("Haven't seen byte_cnt + yet");
	}

	if ((devc->sent_samples >= devc->limit_samples) \
		&& (devc->rxstate == RX_ACTIVE)) {
		sr_dbg("Ending: sent %u of limit %lu samples byte_cnt %lu",
			devc->sent_samples, devc->limit_samples, devc->byte_cnt);
		send_serial_char(serial, '+');
	}

	sr_spew("Receive function done: sent %u limit %lu wrptr %u len %d",
		devc->sent_samples, devc->limit_samples, devc->wrptr, len);

	return TRUE;
}

/* Read device specific information from the device */
SR_PRIV int raspberrypi_pico_get_dev_cfg(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	struct sr_serial_dev_inst *serial;
	char *cmd, response[20];
	gchar **tokens;
	unsigned int i;
	int ret, num_tokens;

	devc = sdi->priv;
	sr_dbg("At get_dev_cfg");
	serial = sdi->conn;
	for (i = 0; i < devc->num_a_channels; i++) {
		cmd = g_strdup_printf("a%d\n", i);
		ret = send_serial_w_resp(serial, cmd, response, 20);
		if (ret <= 0) {
			sr_err("ERROR: No response from device for analog channel query");
			return SR_ERR;
		}
		response[ret] = 0;
		tokens = NULL;
		tokens = g_strsplit(response, "x", 0);
		num_tokens = g_strv_length(tokens);

		if (num_tokens == 2) {
			devc->a_scale[i] = ((float) atoi(tokens[0])) / 1000000.0;
			devc->a_offset[i] = ((float) atoi(tokens[1])) / 1000000.0;
			sr_dbg("A%d scale %f offset %f response #%s# tokens #%s# #%s#",
				i, devc->a_scale[i], devc->a_offset[i],
				response, tokens[0], tokens[1]);
		} else {
			sr_err("ERROR: Ascale read c%d got unparseable response %s tokens %d",
				i, response, num_tokens);
			devc->a_scale[i] = 0.0257;
			devc->a_offset[i] = 0.0;
		}

		g_strfreev(tokens);
		g_free(cmd);
	}

	return SR_OK;
}
