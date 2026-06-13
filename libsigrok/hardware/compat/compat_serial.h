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

#ifndef LIBSIGROK_COMPAT_SERIAL_H
#define LIBSIGROK_COMPAT_SERIAL_H

/**
 * @file
 *
 * Serial port compat layer for PXView's libsigrok.
 *
 * Provides serial I/O functions using libserialport, enabling
 * serial-based compat drivers (e.g. raspberrypi-pico) to work.
 *
 * This header also declares helper functions that standard sigrok
 * drivers expect but PXView's libsigrok does not provide.
 */

#include <glib.h>
#include <stdint.h>

/* Config keys used by serial compat drivers */
#ifndef SR_CONF_FORCE_DETECT
#define SR_CONF_FORCE_DETECT 40003
#endif

/* SR_CONF_SERIALCOMM is already defined in libsigrok.h as 20001 */

/* Error code for serial timeout - not defined in PXView's libsigrok */
#ifndef SR_ERR_TIMEOUT
#define SR_ERR_TIMEOUT 100
#endif

/* Serial communication availability flag */
#ifdef HAVE_LIBSERIALPORT
#define HAVE_SERIAL_COMM 1
#endif

/**
 * Write data to serial port with blocking I/O.
 *
 * @param serial The serial device instance.
 * @param buf Buffer to write.
 * @param count Number of bytes to write.
 * @param timeout_ms Timeout in milliseconds (0 = non-blocking).
 *
 * @return Number of bytes written, or negative error code.
 */
SR_PRIV int serial_write_blocking(struct sr_serial_dev_inst *serial,
		const void *buf, size_t count, unsigned int timeout_ms);

/**
 * Read data from serial port with blocking I/O.
 *
 * @param serial The serial device instance.
 * @param buf Buffer to read into.
 * @param count Number of bytes to read.
 * @param timeout_ms Timeout in milliseconds (0 = non-blocking).
 *
 * @return Number of bytes read, or negative error code.
 */
SR_PRIV int serial_read_blocking(struct sr_serial_dev_inst *serial,
		void *buf, size_t count, unsigned int timeout_ms);

/**
 * Wait for all output to be transmitted.
 *
 * @param serial The serial device instance.
 *
 * @return SR_OK on success, SR_ERR on failure.
 */
SR_PRIV int serial_drain(struct sr_serial_dev_inst *serial);

/**
 * Calculate a reasonable timeout for a given baudrate and byte count.
 *
 * @param baudrate The serial baudrate.
 * @param bytes Number of bytes to transfer.
 * @param flags Additional flags (unused, for API compat).
 *
 * @return Timeout in milliseconds.
 */
SR_PRIV int serial_timeout(struct sr_serial_dev_inst *serial,
		uint64_t baudrate, int bytes);

/**
 * Add a serial port as a data source to the session event loop.
 *
 * @param serial The serial device instance.
 * @param events Event mask (G_IO_IN, etc.).
 * @param timeout Timeout in ms.
 * @param cb Callback function.
 * @param sdi The device instance.
 *
 * @return SR_OK on success, SR_ERR on failure.
 */
SR_PRIV int serial_source_add(struct sr_serial_dev_inst *serial,
		int events, int timeout,
		sr_receive_data_callback_t cb,
		const struct sr_dev_inst *sdi);

/**
 * Remove a serial port from the session event loop.
 *
 * @param serial The serial device instance.
 *
 * @return SR_OK on success, SR_ERR on failure.
 */
SR_PRIV int serial_source_remove(struct sr_serial_dev_inst *serial);

/**
 * Standard helper to open a serial device from sdi->conn.
 *
 * @param sdi The device instance (sdi->conn must be sr_serial_dev_inst).
 *
 * @return SR_OK on success, SR_ERR on failure.
 */
SR_PRIV int std_serial_dev_open(struct sr_dev_inst *sdi);

/**
 * Standard helper to close a serial device from sdi->conn.
 *
 * @param sdi The device instance (sdi->conn must be sr_serial_dev_inst).
 *
 * @return SR_OK on success, SR_ERR on failure.
 */
SR_PRIV int std_serial_dev_close(struct sr_dev_inst *sdi);

/**
 * Send a DF_TRIGGER packet on the session bus.
 *
 * @param sdi The device instance.
 * @param prefix Log message prefix.
 *
 * @return SR_OK on success, SR_ERR on failure.
 */
SR_PRIV int std_session_send_df_trigger(const struct sr_dev_inst *sdi,
		const char *prefix);

/**
 * Create a GVariant tuple of two uint64 values.
 *
 * @param first First value.
 * @param second Second value.
 *
 * @return GVariant containing (first, second) tuple.
 */
SR_PRIV GVariant *std_gvar_tuple_u64(uint64_t first, uint64_t second);

#endif
