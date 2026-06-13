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

#ifndef LIBSIGROK_COMPAT_H
#define LIBSIGROK_COMPAT_H

/**
 * @file
 *
 * Compat layer header for adapting standard sigrok hardware drivers to
 * PXView's libsigrok API.
 *
 * Standard sigrok drivers should include this header instead of
 * <libsigrok/libsigrok.h> and "libsigrok-internal.h".
 */

/* Include PXView's internal headers */
#include "libsigrok-internal.h"
#include "log.h"

/* Compat sub-headers */
#include "compat_driver.h"
#include "compat_config.h"
#include "compat_helpers.h"
#include "compat_serial.h"

/* Standard sigrok type aliases */
#define sr_dev_driver_std sr_dev_driver

/* Standard sigrok's sr_spew - map to sr_dbg since PXView doesn't have it */
#ifndef sr_spew
#define sr_spew(fmt, args...) sr_dbg(fmt, ## args)
#endif

/* Standard sigrok inst_type values */
enum sr_inst_type {
    SR_INST_USB = 0,
    SR_INST_SERIAL = 1,
    SR_INST_SCPI = 2,
    SR_INST_USER = 3,
    SR_INST_MODBUS = 4,
};

/* Standard sigrok channel type for analog (standard uses 10001, PXView uses 10002) */
#define SR_CHANNEL_ANALOG_STD 10001

/* sr_session_send compat - maps to ds_data_forward */
#define sr_session_send(sdi, pkt) ds_data_forward(sdi, pkt)

/* usb_source_add compat - wraps PXView's sr_session_source_add for USB fd */
SR_PRIV int compat_usb_source_add(struct sr_context *ctx, int timeout,
    sr_receive_data_callback_t cb, const struct sr_dev_inst *sdi);
#define usb_source_add(ctx, timeout, cb, sdi) compat_usb_source_add(ctx, timeout, cb, sdi)

/* sr_channel_group_new compat */
SR_PRIV struct sr_channel_group *compat_sr_channel_group_new(
    struct sr_dev_inst *sdi, const char *name, void *priv);
#define sr_channel_group_new(sdi, name, priv) compat_sr_channel_group_new(sdi, name, priv)

/* sr_parse_probe_names - stub for now */
SR_PRIV void sr_parse_probe_names(const char *str, const char *defaults[],
    int ch_max, struct sr_channel **channels);

/* usb_match_manuf_prod - stub */
SR_PRIV gboolean usb_match_manuf_prod(libusb_device *dev,
    const char *manufacturer, const char *product);

/* usb_get_port_path - stub */
SR_PRIV int usb_get_port_path(libusb_device *dev, char *path, int path_len);

/* Standard sigrok's SR_REGISTER_DEV_DRIVER replacement */
/* In standard sigrok, this macro auto-registers a driver. In PXView compat,
 * we just declare the driver_info struct. Registration is done manually. */
#define SR_REGISTER_DEV_DRIVER(name) \
    extern struct sr_dev_driver name##_driver_info

/* Standard sigrok uses driver->context for private data; PXView uses priv */
#define DRIVER_CONTEXT(drv) ((drv)->priv)

/* Standard sigrok's sr_config_get/set/list use uint32_t key; PXView uses int */
/* These are handled by the wrapper functions in each compat driver */

/* Standard sigrok's dev_acquisition_start doesn't have cb_data */
/* Standard sigrok's dev_acquisition_stop has non-const sdi and no cb_data */
/* These are handled by the wrapper functions in each compat driver */

/* Convenience: standard sigrok channel creation with sdi back-reference */
SR_PRIV struct sr_channel *compat_sr_channel_new(struct sr_dev_inst *sdi,
    int index, int type, gboolean enabled, const char *name);
#define sr_channel_new(sdi, index, type, enabled, name) \
    compat_sr_channel_new(sdi, index, type, enabled, name)

/* Standard sigrok's sr_config_get/set/list use uint32_t key and no ch param;
 * PXView uses int key and has ch param. These compat wrappers bridge the gap.
 * Standard sigrok drivers call sr_config_get/set/list from within their own
 * code with the standard signature, so we redirect via macros. */
SR_PRIV int sr_config_get_compat(const struct sr_dev_driver *driver,
    const struct sr_dev_inst *sdi, const struct sr_channel_group *cg,
    uint32_t key, GVariant **data);
SR_PRIV int sr_config_set_compat(const struct sr_dev_inst *sdi,
    const struct sr_channel_group *cg, uint32_t key, GVariant *data);
SR_PRIV int sr_config_list_compat(const struct sr_dev_driver *driver,
    const struct sr_dev_inst *sdi, const struct sr_channel_group *cg,
    uint32_t key, GVariant **data);
/* Note: no macros for sr_config_get/set/list here because PXView's own
 * sr_config_get/set/list have different signatures (extra ch param, int key).
 * Standard sigrok drivers should call the _compat versions directly, or
 * each compat driver's wrapper handles the translation at the callback level.
 * If a standard driver calls sr_config_get/set/list internally, it must use
 * the _compat names explicitly. */

/* Standard sigrok's sr_dev_inst_new takes inst_type instead of mode.
 * This compat version creates a PXView sr_dev_inst and sets compat fields. */
SR_PRIV struct sr_dev_inst *compat_sr_dev_inst_new(int inst_type, int status,
    const char *vendor, const char *model, const char *version);
#define sr_dev_inst_new(inst_type, status, vendor, model, version) \
    compat_sr_dev_inst_new(inst_type, status, vendor, model, version)

/* Standard sigrok's sr_usb_dev_inst_new takes an extra hdl parameter.
 * PXView's version only takes bus and address. This compat version also
 * sets the devhdl field. */
SR_PRIV struct sr_usb_dev_inst *compat_sr_usb_dev_inst_new(uint8_t bus,
    uint8_t address, struct libusb_device_handle *hdl);
#define sr_usb_dev_inst_new(bus, address, hdl) \
    compat_sr_usb_dev_inst_new(bus, address, hdl)

/* Standard sigrok's std_init helper - creates drv_context and sets driver->priv */
SR_PRIV int std_init(struct sr_dev_driver *driver, struct sr_context *sr_ctx);

/* Standard sigrok's std_cleanup helper */
SR_PRIV int std_cleanup(const struct sr_dev_driver *driver);

/* Standard sigrok's std_dev_list helper */
SR_PRIV GSList *std_dev_list(const struct sr_dev_driver *driver);

/* Standard sigrok's std_dev_clear helper */
SR_PRIV int std_dev_clear(const struct sr_dev_driver *driver);

/* Standard sigrok's std_session_send_df_header */
SR_PRIV int std_session_send_df_header(const struct sr_dev_inst *sdi,
    const char *prefix);

/* Standard sigrok's std_session_send_df_end */
SR_PRIV int std_session_send_df_end(const struct sr_dev_inst *sdi,
    const char *prefix);

/* Standard sigrok's STD_CONFIG_LIST macro equivalent */
SR_PRIV int std_config_list(uint32_t key, GVariant **data,
    const struct sr_dev_inst *sdi, const struct sr_channel_group *cg,
    const uint32_t scanopts[], size_t num_scanopts,
    const uint32_t drvopts[], size_t num_drvopts,
    const uint32_t devopts[], size_t num_devopts);

/* GVariant helper functions (standard sigrok style) */
SR_PRIV GVariant *std_gvar_samplerates(const uint64_t samplerates[],
    size_t count);
SR_PRIV GVariant *std_gvar_samplerate_steps(const uint64_t steps[],
    size_t count);
SR_PRIV GVariant *std_gvar_array_i32(const int32_t vals[], size_t count);
SR_PRIV GVariant *std_gvar_array_u32(const uint32_t vals[], size_t count);
SR_PRIV GVariant *std_gvar_array_u64(const uint64_t vals[], size_t count);
SR_PRIV GVariant *std_gvar_min_max_step(double min, double max, double step);
SR_PRIV GVariant *std_gvar_min_max_steps_uint64(uint64_t min, uint64_t max,
    const uint64_t steps[], size_t count);
SR_PRIV GVariant *std_gvar_tuple_array(const char *strs[], size_t count);

/* Config get helpers (standard sigrok style) */
SR_PRIV int std_u64_idx(const struct sr_dev_inst *sdi, uint32_t key,
    GVariant **data, const uint64_t vals[], size_t count);
SR_PRIV int std_str_idx(const struct sr_dev_inst *sdi, uint32_t key,
    GVariant **data, const char *const strs[], size_t count);
SR_PRIV int std_bool_idx(const struct sr_dev_inst *sdi, uint32_t key,
    GVariant **data, const gboolean vals[], size_t count);

#endif
