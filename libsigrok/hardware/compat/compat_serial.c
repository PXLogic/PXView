/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2025 Compat Layer Authors
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

#include "compat.h"
#include "compat_serial.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#ifdef HAVE_LIBSERIALPORT
#include <libserialport.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

/**
 * @file
 *
 * Serial port compat layer implementation using libserialport.
 *
 * Provides the serial I/O functions declared in libsigrok-internal.h
 * that PXView's libsigrok has no implementation for. When
 * HAVE_LIBSERIALPORT is defined, uses libserialport for actual I/O.
 * When not defined, provides stubs that return SR_ERR.
 */

/*--- Helper: get sp_port from serial struct --------------------------------*/

#ifdef HAVE_LIBSERIALPORT
static struct sp_port *serial_get_sp(struct sr_serial_dev_inst *serial)
{
	if (!serial)
		return NULL;
	return (struct sp_port *)serial->sp_data;
}
#endif

/*--- serial_open -----------------------------------------------------------*/

SR_PRIV int serial_open(struct sr_serial_dev_inst *serial, int flags)
{
#ifdef HAVE_LIBSERIALPORT
	struct sp_port *port;
	enum sp_mode mode;
	int ret;

	if (!serial || !serial->port)
		return SR_ERR_ARG;

	/* If already open, just return OK */
	if (serial->sp_data)
		return SR_OK;

	ret = sp_get_port_by_name(serial->port, &port);
	if (ret != SP_OK) {
		sr_err("Failed to get serial port '%s': %s",
			serial->port, sp_last_error_message());
		return SR_ERR;
	}

	if (flags & SERIAL_RDWR)
		mode = SP_MODE_READ_WRITE;
	else if (flags & SERIAL_RDONLY)
		mode = SP_MODE_READ;
	else
		mode = SP_MODE_READ_WRITE;

	ret = sp_open(port, mode);
	if (ret != SP_OK) {
		sr_err("Failed to open serial port '%s': %s",
			serial->port, sp_last_error_message());
		sp_free_port(port);
		return SR_ERR;
	}

	serial->sp_data = port;

	/* Apply serialcomm parameters if set */
	if (serial->serialcomm) {
		ret = serial_set_paramstr(serial, serial->serialcomm);
		if (ret != SR_OK) {
			sr_warn("Failed to set serial params '%s' on open.",
				serial->serialcomm);
			/* Non-fatal: port is still open */
		}
	}

	return SR_OK;
#else
	(void)serial;
	(void)flags;
	sr_err("Serial support not compiled in (no libserialport).");
	return SR_ERR;
#endif
}

/*--- serial_close ----------------------------------------------------------*/

SR_PRIV int serial_close(struct sr_serial_dev_inst *serial)
{
#ifdef HAVE_LIBSERIALPORT
	struct sp_port *port;

	if (!serial)
		return SR_ERR_ARG;

	port = serial_get_sp(serial);
	if (!port)
		return SR_OK;

	sp_close(port);
	sp_free_port(port);
	serial->sp_data = NULL;
	serial->fd = -1;

	return SR_OK;
#else
	(void)serial;
	return SR_ERR;
#endif
}

/*--- serial_flush ----------------------------------------------------------*/

SR_PRIV int serial_flush(struct sr_serial_dev_inst *serial)
{
#ifdef HAVE_LIBSERIALPORT
	struct sp_port *port;

	port = serial_get_sp(serial);
	if (!port)
		return SR_ERR;

	if (sp_flush(port, SP_BUF_BOTH) != SP_OK)
		return SR_ERR;

	return SR_OK;
#else
	(void)serial;
	return SR_ERR;
#endif
}

/*--- serial_drain ----------------------------------------------------------*/

SR_PRIV int serial_drain(struct sr_serial_dev_inst *serial)
{
#ifdef HAVE_LIBSERIALPORT
	struct sp_port *port;

	port = serial_get_sp(serial);
	if (!port)
		return SR_ERR;

	if (sp_drain(port) != SP_OK)
		return SR_ERR;

	return SR_OK;
#else
	(void)serial;
	return SR_ERR;
#endif
}

/*--- serial_write ----------------------------------------------------------*/

SR_PRIV int serial_write(struct sr_serial_dev_inst *serial,
		const void *buf, size_t count)
{
#ifdef HAVE_LIBSERIALPORT
	struct sp_port *port;
	int ret;

	port = serial_get_sp(serial);
	if (!port)
		return SR_ERR;

	ret = sp_blocking_write(port, buf, count, 0);
	if (ret < 0)
		return SR_ERR;

	return ret;
#else
	(void)serial;
	(void)buf;
	(void)count;
	return SR_ERR;
#endif
}

/*--- serial_write_blocking -------------------------------------------------*/

SR_PRIV int serial_write_blocking(struct sr_serial_dev_inst *serial,
		const void *buf, size_t count, unsigned int timeout_ms)
{
#ifdef HAVE_LIBSERIALPORT
	struct sp_port *port;
	int ret;

	port = serial_get_sp(serial);
	if (!port)
		return SR_ERR;

	ret = sp_blocking_write(port, buf, count, timeout_ms);
	if (ret < 0)
		return SR_ERR;

	return ret;
#else
	(void)serial;
	(void)buf;
	(void)count;
	(void)timeout_ms;
	return SR_ERR;
#endif
}

/*--- serial_read -----------------------------------------------------------*/

SR_PRIV int serial_read(struct sr_serial_dev_inst *serial, void *buf,
		size_t count)
{
#ifdef HAVE_LIBSERIALPORT
	struct sp_port *port;
	int ret;

	port = serial_get_sp(serial);
	if (!port)
		return SR_ERR;

	ret = sp_blocking_read(port, buf, count, 0);
	if (ret < 0)
		return SR_ERR;

	return ret;
#else
	(void)serial;
	(void)buf;
	(void)count;
	return SR_ERR;
#endif
}

/*--- serial_read_blocking --------------------------------------------------*/

SR_PRIV int serial_read_blocking(struct sr_serial_dev_inst *serial,
		void *buf, size_t count, unsigned int timeout_ms)
{
#ifdef HAVE_LIBSERIALPORT
	struct sp_port *port;
	int ret;

	port = serial_get_sp(serial);
	if (!port)
		return SR_ERR;

	ret = sp_blocking_read(port, buf, count, timeout_ms);
	if (ret < 0)
		return SR_ERR;

	return ret;
#else
	(void)serial;
	(void)buf;
	(void)count;
	(void)timeout_ms;
	return SR_ERR;
#endif
}

/*--- serial_set_params -----------------------------------------------------*/

SR_PRIV int serial_set_params(struct sr_serial_dev_inst *serial, int baudrate,
		int bits, int parity, int stopbits, int flowcontrol, int rts, int dtr)
{
#ifdef HAVE_LIBSERIALPORT
	struct sp_port *port;

	port = serial_get_sp(serial);
	if (!port)
		return SR_ERR;

	if (sp_set_baudrate(port, baudrate) != SP_OK) {
		sr_err("Failed to set baudrate %d.", baudrate);
		return SR_ERR;
	}

	if (sp_set_bits(port, bits) != SP_OK) {
		sr_err("Failed to set data bits %d.", bits);
		return SR_ERR;
	}

	switch (parity) {
	case SERIAL_PARITY_NONE:
		if (sp_set_parity(port, SP_PARITY_NONE) != SP_OK)
			return SR_ERR;
		break;
	case SERIAL_PARITY_EVEN:
		if (sp_set_parity(port, SP_PARITY_EVEN) != SP_OK)
			return SR_ERR;
		break;
	case SERIAL_PARITY_ODD:
		if (sp_set_parity(port, SP_PARITY_ODD) != SP_OK)
			return SR_ERR;
		break;
	default:
		sr_err("Unsupported parity %d.", parity);
		return SR_ERR_ARG;
	}

	if (sp_set_stopbits(port, stopbits) != SP_OK) {
		sr_err("Failed to set stop bits %d.", stopbits);
		return SR_ERR;
	}

	switch (flowcontrol) {
	case 0:
		if (sp_set_flowcontrol(port, SP_FLOWCONTROL_NONE) != SP_OK)
			return SR_ERR;
		break;
	case 1:
		if (sp_set_flowcontrol(port, SP_FLOWCONTROL_RTSCTS) != SP_OK)
			return SR_ERR;
		break;
	case 2:
		if (sp_set_flowcontrol(port, SP_FLOWCONTROL_XONXOFF) != SP_OK)
			return SR_ERR;
		break;
	default:
		sr_err("Unsupported flow control %d.", flowcontrol);
		return SR_ERR_ARG;
	}

	if (sp_set_rts(port, rts ? SP_RTS_ON : SP_RTS_OFF) != SP_OK)
		return SR_ERR;

	if (sp_set_dtr(port, dtr ? SP_DTR_ON : SP_DTR_OFF) != SP_OK)
		return SR_ERR;

	return SR_OK;
#else
	(void)serial;
	(void)baudrate;
	(void)bits;
	(void)parity;
	(void)stopbits;
	(void)flowcontrol;
	(void)rts;
	(void)dtr;
	return SR_ERR;
#endif
}

/*--- serial_set_paramstr ---------------------------------------------------*/

SR_PRIV int serial_set_paramstr(struct sr_serial_dev_inst *serial,
		const char *paramstr)
{
#ifdef HAVE_LIBSERIALPORT
	int baudrate, bits, parity, stopbits, flowcontrol, rts, dtr;
	char **params, **kv;
	guint i;
	int ret;

	if (!serial || !paramstr)
		return SR_ERR_ARG;

	/* Default values */
	baudrate = 9600;
	bits = 8;
	parity = SERIAL_PARITY_NONE;
	stopbits = 1;
	flowcontrol = 0;
	rts = 0;
	dtr = 0;

	params = g_strsplit(paramstr, "/", 0);

	for (i = 0; params[i]; i++) {
		if (i == 0) {
			/* First element: baudrate */
			baudrate = atoi(params[i]);
			if (baudrate <= 0) {
				sr_err("Invalid baudrate '%s'.", params[i]);
				g_strfreev(params);
				return SR_ERR_ARG;
			}
		} else if (i == 1) {
			/* Second element: e.g. "8n1" or "7o2" */
			if (strlen(params[i]) < 3) {
				sr_err("Invalid serial format '%s'.", params[i]);
				g_strfreev(params);
				return SR_ERR_ARG;
			}
			bits = params[i][0] - '0';
			switch (params[i][1]) {
			case 'n': case 'N':
				parity = SERIAL_PARITY_NONE;
				break;
			case 'e': case 'E':
				parity = SERIAL_PARITY_EVEN;
				break;
			case 'o': case 'O':
				parity = SERIAL_PARITY_ODD;
				break;
			default:
				sr_err("Invalid parity '%c'.", params[i][1]);
				g_strfreev(params);
				return SR_ERR_ARG;
			}
			stopbits = params[i][2] - '0';
		} else {
			/* Additional key=value params: dtr=1, rts=0, flow=0 */
			kv = g_strsplit(params[i], "=", 2);
			if (kv[0] && kv[1]) {
				if (g_str_equal(kv[0], "dtr") || g_str_equal(kv[0], "DTR"))
					dtr = atoi(kv[1]);
				else if (g_str_equal(kv[0], "rts") || g_str_equal(kv[0], "RTS"))
					rts = atoi(kv[1]);
				else if (g_str_equal(kv[0], "flow") || g_str_equal(kv[0], "FLOW"))
					flowcontrol = atoi(kv[1]);
			}
			g_strfreev(kv);
		}
	}

	g_strfreev(params);

	ret = serial_set_params(serial, baudrate, bits, parity,
			stopbits, flowcontrol, rts, dtr);

	return ret;
#else
	(void)serial;
	(void)paramstr;
	return SR_ERR;
#endif
}

/*--- serial_readline -------------------------------------------------------*/

SR_PRIV int serial_readline(struct sr_serial_dev_inst *serial, char **buf,
		int *buflen, gint64 timeout_ms)
{
#ifdef HAVE_LIBSERIALPORT
	struct sp_port *port;
	int ret, maxlen;
	gint64 endtime;
	char c;

	if (!serial || !buf || !buflen)
		return SR_ERR_ARG;

	port = serial_get_sp(serial);
	if (!port)
		return SR_ERR;

	if (!*buf) {
		*buflen = 128;
		*buf = g_malloc0(*buflen);
	}

	endtime = g_get_monotonic_time() + timeout_ms * 1000;
	maxlen = *buflen - 1;
	(*buf)[0] = '\0';

	int pos = 0;
	while (pos < maxlen) {
		gint64 remaining = (endtime - g_get_monotonic_time()) / 1000;
		if (remaining < 0)
			remaining = 0;

		ret = sp_blocking_read(port, &c, 1, (unsigned int)remaining);
		if (ret == 0)
			break; /* Timeout */
		if (ret < 0)
			return SR_ERR;

		if (c == '\n' || c == '\r') {
			(*buf)[pos] = '\0';
			return pos;
		}

		(*buf)[pos++] = c;
	}

	(*buf)[pos] = '\0';
	return pos;
#else
	(void)serial;
	(void)buf;
	(void)buflen;
	(void)timeout_ms;
	return SR_ERR;
#endif
}

/*--- serial_stream_detect --------------------------------------------------*/

SR_PRIV int serial_stream_detect(struct sr_serial_dev_inst *serial,
		uint8_t *buf, size_t *buflen,
		size_t packet_size, packet_valid_t is_valid,
		uint64_t timeout_ms, int baudrate)
{
#ifdef HAVE_LIBSERIALPORT
	struct sp_port *port;
	gint64 endtime;
	size_t ibuf;
	int ret;

	if (!serial || !buf || !buflen || !is_valid)
		return SR_ERR_ARG;

	port = serial_get_sp(serial);
	if (!port)
		return SR_ERR;

	endtime = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
	ibuf = 0;

	while (ibuf < packet_size) {
		gint64 remaining = (endtime - g_get_monotonic_time()) / 1000;
		if (remaining < 0)
			return SR_ERR_TIMEOUT;

		ret = sp_blocking_read(port, &buf[ibuf], 1,
				(unsigned int)remaining);
		if (ret == 0)
			return SR_ERR_TIMEOUT;
		if (ret < 0)
			return SR_ERR;

		ibuf++;

		if (ibuf >= packet_size) {
			if (is_valid(buf)) {
				*buflen = ibuf;
				return SR_OK;
			}
			/* Shift buffer left by one and try again */
			memmove(buf, buf + 1, ibuf - 1);
			ibuf--;
		}
	}

	return SR_ERR_TIMEOUT;
#else
	(void)serial;
	(void)buf;
	(void)buflen;
	(void)packet_size;
	(void)is_valid;
	(void)timeout_ms;
	(void)baudrate;
	return SR_ERR;
#endif
}

/*--- serial_timeout --------------------------------------------------------*/

SR_PRIV int serial_timeout(struct sr_serial_dev_inst *serial,
		uint64_t baudrate, int bytes)
{
	(void)serial;

	if (baudrate == 0)
		return 1000;

	/* Calculate time to transfer 'bytes' at 'baudrate' bits/sec.
	 * Each byte takes ~10 bits (8 data + start + stop).
	 * Add 20% safety margin, minimum 10ms. */
	int timeout_ms = (int)((uint64_t)bytes * 10000 / baudrate * 12 / 10);
	if (timeout_ms < 10)
		timeout_ms = 10;

	return timeout_ms;
}

/*--- serial_source_add -----------------------------------------------------*/

SR_PRIV int serial_source_add(struct sr_serial_dev_inst *serial,
		int events, int timeout,
		sr_receive_data_callback_t cb,
		const struct sr_dev_inst *sdi)
{
#ifdef HAVE_LIBSERIALPORT
	struct sp_port *port;
	gintptr poll_obj;

	if (!serial)
		return SR_ERR_ARG;

	port = serial_get_sp(serial);
	if (!port)
		return SR_ERR;

#ifdef _WIN32
	{
		HANDLE hnd;
		if (sp_get_port_handle(port, &hnd) != SP_OK) {
			/* Fallback: use timer-based polling */
			return sr_session_source_add(-1, events, timeout, cb, sdi);
		}
		poll_obj = (gintptr)hnd;
	}
#else
	{
		int fd;
		if (sp_get_port_handle(port, &fd) != SP_OK) {
			/* Fallback: use timer-based polling */
			return sr_session_source_add(-1, events, timeout, cb, sdi);
		}
		poll_obj = (gintptr)fd;
	}
#endif

	return sr_session_source_add(poll_obj, events, timeout, cb, sdi);
#else
	(void)serial;
	(void)events;
	(void)timeout;
	(void)cb;
	(void)sdi;
	return SR_ERR;
#endif
}

/*--- serial_source_remove --------------------------------------------------*/

SR_PRIV int serial_source_remove(struct sr_serial_dev_inst *serial)
{
#ifdef HAVE_LIBSERIALPORT
	struct sp_port *port;
	gintptr poll_obj;

	if (!serial)
		return SR_ERR_ARG;

	port = serial_get_sp(serial);
	if (!port)
		return SR_ERR;

#ifdef _WIN32
	{
		HANDLE hnd;
		if (sp_get_port_handle(port, &hnd) != SP_OK)
			return sr_session_source_remove(-1);
		poll_obj = (gintptr)hnd;
	}
#else
	{
		int fd;
		if (sp_get_port_handle(port, &fd) != SP_OK)
			return sr_session_source_remove(-1);
		poll_obj = (gintptr)fd;
	}
#endif

	return sr_session_source_remove(poll_obj);
#else
	(void)serial;
	return SR_ERR;
#endif
}

/*--- std_serial_dev_open ---------------------------------------------------*/

SR_PRIV int std_serial_dev_open(struct sr_dev_inst *sdi)
{
	struct sr_serial_dev_inst *serial;

	if (!sdi || !sdi->conn)
		return SR_ERR_ARG;

	serial = sdi->conn;
	return serial_open(serial, SERIAL_RDWR);
}

/*--- std_serial_dev_close --------------------------------------------------*/

SR_PRIV int std_serial_dev_close(struct sr_dev_inst *sdi)
{
	struct sr_serial_dev_inst *serial;

	if (!sdi || !sdi->conn)
		return SR_ERR_ARG;

	serial = sdi->conn;
	return serial_close(serial);
}

/*--- std_session_send_df_trigger -------------------------------------------*/

SR_PRIV int std_session_send_df_trigger(const struct sr_dev_inst *sdi,
		const char *prefix)
{
	struct sr_datafeed_packet packet;
	int ret;

	(void)prefix;

	packet.type = SR_DF_TRIGGER;
	packet.status = SR_PKT_OK;
	packet.payload = NULL;

	ret = ds_data_forward(sdi, &packet);
	if (ret < 0)
		sr_err("Failed to send trigger packet.");

	return ret;
}

/*--- std_gvar_tuple_u64 ----------------------------------------------------*/

SR_PRIV GVariant *std_gvar_tuple_u64(uint64_t first, uint64_t second)
{
	GVariant *vals[2];

	vals[0] = g_variant_new_uint64(first);
	vals[1] = g_variant_new_uint64(second);

	return g_variant_new_tuple(vals, 2);
}
