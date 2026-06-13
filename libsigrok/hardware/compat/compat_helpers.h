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

#ifndef LIBSIGROK_COMPAT_HELPERS_H
#define LIBSIGROK_COMPAT_HELPERS_H

/**
 * @file
 *
 * Compat helper function declarations.
 *
 * These functions provide standard sigrok API equivalents that are
 * missing from PXView's libsigrok. Implementations are in compat_helpers.c.
 */

#include <glib.h>
#include <stdint.h>

/**
 * Add a USB file descriptor as a session data source.
 * Wraps PXView's sr_session_source_add() for USB device handles.
 *
 * @param ctx The sr_context.
 * @param timeout Timeout in ms.
 * @param cb Callback function for received data.
 * @param sdi The device instance.
 *
 * @return SR_OK on success, SR_ERR on failure.
 */
SR_PRIV int compat_usb_source_add(struct sr_context *ctx, int timeout,
    sr_receive_data_callback_t cb, const struct sr_dev_inst *sdi);

/**
 * Create a new channel group and add it to the device's channel_groups list.
 *
 * @param sdi The device instance.
 * @param name Name of the channel group.
 * @param priv Private data for driver use.
 *
 * @return Pointer to the new channel group, or NULL on error.
 */
SR_PRIV struct sr_channel_group *compat_sr_channel_group_new(
    struct sr_dev_inst *sdi, const char *name, void *priv);

/**
 * Create a new channel with sdi back-reference set.
 * This is the standard sigrok version of sr_channel_new that also
 * sets the sdi back-reference field.
 *
 * @param sdi The device instance that owns this channel.
 * @param index Channel index.
 * @param type Channel type (SR_CHANNEL_LOGIC, SR_CHANNEL_ANALOG_STD, etc.).
 * @param enabled Whether the channel is enabled.
 * @param name Channel name.
 *
 * @return Pointer to the new channel, or NULL on error.
 */
SR_PRIV struct sr_channel *compat_sr_channel_new(struct sr_dev_inst *sdi,
    int index, int type, gboolean enabled, const char *name);

/**
 * Standard sigrok's sr_config_get compat - uint32_t key, no ch parameter.
 * Calls PXView's sr_config_get with (int)key cast and ch=NULL.
 *
 * @param driver The device driver.
 * @param sdi The device instance.
 * @param cg The channel group (may be NULL).
 * @param key The config key (uint32_t in standard sigrok).
 * @param data Output GVariant.
 *
 * @return SR_OK on success, SR_ERR on failure.
 */
SR_PRIV int sr_config_get_compat(const struct sr_dev_driver *driver,
    const struct sr_dev_inst *sdi, const struct sr_channel_group *cg,
    uint32_t key, GVariant **data);

/**
 * Standard sigrok's sr_config_set compat - uint32_t key, no ch parameter.
 * Calls PXView's sr_config_set with (int)key cast and ch=NULL.
 *
 * @param sdi The device instance.
 * @param cg The channel group (may be NULL).
 * @param key The config key (uint32_t in standard sigrok).
 * @param data Input GVariant.
 *
 * @return SR_OK on success, SR_ERR on failure.
 */
SR_PRIV int sr_config_set_compat(const struct sr_dev_inst *sdi,
    const struct sr_channel_group *cg, uint32_t key, GVariant *data);

/**
 * Standard sigrok's sr_config_list compat - uint32_t key, no ch parameter.
 * Calls PXView's sr_config_list with (int)key cast and ch=NULL.
 *
 * @param driver The device driver.
 * @param sdi The device instance.
 * @param cg The channel group (may be NULL).
 * @param key The config key (uint32_t in standard sigrok).
 * @param data Output GVariant.
 *
 * @return SR_OK on success, SR_ERR on failure.
 */
SR_PRIV int sr_config_list_compat(const struct sr_dev_driver *driver,
    const struct sr_dev_inst *sdi, const struct sr_channel_group *cg,
    uint32_t key, GVariant **data);

/**
 * Standard sigrok's sr_dev_inst_new compat - creates a PXView sr_dev_inst
 * and also sets the compat fields (model, inst_type).
 *
 * @param inst_type Instance type (SR_INST_USB, SR_INST_SERIAL, etc.).
 * @param status Device status (SR_ST_NOT_FOUND, SR_ST_INITIALIZING, etc.).
 * @param vendor Vendor name.
 * @param model Model name.
 * @param version Version string.
 *
 * @return Pointer to the new device instance, or NULL on error.
 */
SR_PRIV struct sr_dev_inst *compat_sr_dev_inst_new(int inst_type, int status,
    const char *vendor, const char *model, const char *version);

/**
 * Standard sigrok's sr_usb_dev_inst_new compat - accepts hdl parameter.
 * PXView's version only takes bus and address; this version also sets
 * the devhdl field from the hdl parameter.
 *
 * @param bus USB bus number.
 * @param address USB device address.
 * @param hdl libusb device handle (may be NULL).
 *
 * @return Pointer to the new USB device instance, or NULL on error.
 */
SR_PRIV struct sr_usb_dev_inst *compat_sr_usb_dev_inst_new(uint8_t bus,
    uint8_t address, struct libusb_device_handle *hdl);

/**
 * Standard sigrok's std_init - creates drv_context and sets driver->priv.
 *
 * @param driver The device driver.
 * @param sr_ctx The sigrok context.
 *
 * @return SR_OK on success.
 */
SR_PRIV int std_init(struct sr_dev_driver *driver, struct sr_context *sr_ctx);

/**
 * Standard sigrok's std_cleanup - frees drv_context and clears instances.
 *
 * @param driver The device driver.
 *
 * @return SR_OK on success, SR_ERR_ARG on invalid arguments.
 */
SR_PRIV int std_cleanup(const struct sr_dev_driver *driver);

/**
 * Standard sigrok's std_dev_list - returns the driver's instance list.
 *
 * @param driver The device driver.
 *
 * @return GSList of device instances.
 */
SR_PRIV GSList *std_dev_list(const struct sr_dev_driver *driver);

/**
 * Standard sigrok's std_dev_clear - clears all device instances.
 *
 * @param driver The device driver.
 *
 * @return SR_OK on success.
 */
SR_PRIV int std_dev_clear(const struct sr_dev_driver *driver);

/**
 * Standard sigrok's std_session_send_df_end - send DF_END packet.
 *
 * @param sdi The device instance.
 * @param prefix Log message prefix.
 *
 * @return SR_OK on success.
 */
SR_PRIV int std_session_send_df_end(const struct sr_dev_inst *sdi,
    const char *prefix);

/**
 * Standard sigrok's std_config_list - handle SR_CONF_DEVICE_OPTIONS etc.
 *
 * @param key The config key.
 * @param data Output GVariant.
 * @param sdi The device instance (may be NULL for scan options).
 * @param cg The channel group (may be NULL).
 * @param scanopts Array of scan option keys.
 * @param num_scanopts Number of scan options.
 * @param drvopts Array of driver option keys.
 * @param num_drvopts Number of driver options.
 * @param devopts Array of device option keys.
 * @param num_devopts Number of device options.
 *
 * @return SR_OK on success, SR_ERR_NA if key not handled.
 */
SR_PRIV int std_config_list(uint32_t key, GVariant **data,
    const struct sr_dev_inst *sdi, const struct sr_channel_group *cg,
    const uint32_t scanopts[], size_t num_scanopts,
    const uint32_t drvopts[], size_t num_drvopts,
    const uint32_t devopts[], size_t num_devopts);

/**
 * Create a GVariant array of samplerates (for SR_CONF_SAMPLERATE).
 *
 * @param samplerates Array of samplerate values.
 * @param count Number of elements.
 *
 * @return GVariant containing the samplerate array.
 */
SR_PRIV GVariant *std_gvar_samplerates(const uint64_t samplerates[],
    size_t count);

/**
 * Create a GVariant array of samplerate steps.
 *
 * @param steps Array of step values.
 * @param count Number of elements.
 *
 * @return GVariant containing the steps array.
 */
SR_PRIV GVariant *std_gvar_samplerate_steps(const uint64_t steps[],
    size_t count);

/**
 * Create a GVariant array of int32 values.
 *
 * @param vals Array of int32 values.
 * @param count Number of elements.
 *
 * @return GVariant containing the array.
 */
SR_PRIV GVariant *std_gvar_array_i32(const int32_t vals[], size_t count);

/**
 * Create a GVariant array of uint32 values.
 *
 * @param vals Array of uint32 values.
 * @param count Number of elements.
 *
 * @return GVariant containing the array.
 */
SR_PRIV GVariant *std_gvar_array_u32(const uint32_t vals[], size_t count);

/**
 * Create a GVariant array of uint64 values.
 *
 * @param vals Array of uint64 values.
 * @param count Number of elements.
 *
 * @return GVariant containing the array.
 */
SR_PRIV GVariant *std_gvar_array_u64(const uint64_t vals[], size_t count);

/**
 * Create a GVariant min/max/step tuple for ranged values.
 *
 * @param min Minimum value.
 * @param max Maximum value.
 * @param step Step value.
 *
 * @return GVariant containing (min, max, step) tuple.
 */
SR_PRIV GVariant *std_gvar_min_max_step(double min, double max, double step);

/**
 * Create a GVariant for samplerate range with steps.
 *
 * @param min Minimum samplerate.
 * @param max Maximum samplerate.
 * @param steps Array of step values.
 * @param count Number of steps.
 *
 * @return GVariant containing the range specification.
 */
SR_PRIV GVariant *std_gvar_min_max_steps_uint64(uint64_t min, uint64_t max,
    const uint64_t steps[], size_t count);

/**
 * Create a GVariant tuple array from string array.
 *
 * @param strs Array of string pointers.
 * @param count Number of elements.
 *
 * @return GVariant containing the tuple array.
 */
SR_PRIV GVariant *std_gvar_tuple_array(const char *strs[], size_t count);

/**
 * Config get helper for uint64 indexed values.
 *
 * @param sdi The device instance.
 * @param key The config key.
 * @param data Output GVariant.
 * @param vals Array of uint64 values.
 * @param count Number of values.
 *
 * @return SR_OK on success, SR_ERR_ARG on invalid arguments.
 */
SR_PRIV int std_u64_idx(const struct sr_dev_inst *sdi, uint32_t key,
    GVariant **data, const uint64_t vals[], size_t count);

/**
 * Config get helper for string indexed values.
 *
 * @param sdi The device instance.
 * @param key The config key.
 * @param data Output GVariant.
 * @param strs Array of string pointers.
 * @param count Number of values.
 *
 * @return SR_OK on success, SR_ERR_ARG on invalid arguments.
 */
SR_PRIV int std_str_idx(const struct sr_dev_inst *sdi, uint32_t key,
    GVariant **data, const char *const strs[], size_t count);

/**
 * Config get helper for boolean indexed values.
 *
 * @param sdi The device instance.
 * @param key The config key.
 * @param data Output GVariant.
 * @param vals Array of boolean values.
 * @param count Number of values.
 *
 * @return SR_OK on success, SR_ERR_ARG on invalid arguments.
 */
SR_PRIV int std_bool_idx(const struct sr_dev_inst *sdi, uint32_t key,
    GVariant **data, const gboolean vals[], size_t count);

/**
 * Parse probe names from a comma-separated string.
 * Stub implementation.
 *
 * @param str Comma-separated probe names.
 * @param defaults Default probe names.
 * @param ch_max Maximum number of channels.
 * @param channels Array of channel pointers to set names on.
 */
SR_PRIV void sr_parse_probe_names(const char *str, const char *defaults[],
    int ch_max, struct sr_channel **channels);

/**
 * Soft trigger logic - stub implementation for compat drivers.
 * PXView does not have soft trigger support, so these are stubs.
 */
struct sr_trigger; /* Forward declaration */

struct soft_trigger_logic {
    const struct sr_dev_inst *sdi;
    struct sr_trigger *trigger;
    uint64_t pre_trigger_samples;
    int cur_stage;
};

SR_PRIV struct soft_trigger_logic *soft_trigger_logic_new(
    const struct sr_dev_inst *sdi, struct sr_trigger *trigger,
    uint64_t pre_trigger_samples);
SR_PRIV void soft_trigger_logic_free(struct soft_trigger_logic *stl);
SR_PRIV int soft_trigger_logic_check(struct soft_trigger_logic *stl,
    uint8_t *buf, size_t buflen, int *pre_trigger_samples);

/**
 * sr_session_trigger_get stub - PXView does not have trigger support.
 */
struct sr_session; /* Forward declaration */
SR_PRIV struct sr_trigger *sr_session_trigger_get(const struct sr_session *session);

/**
 * Check if a USB device matches the given manufacturer and product strings.
 * Stub implementation.
 *
 * @param dev The libusb device.
 * @param manufacturer Expected manufacturer string.
 * @param product Expected product string.
 *
 * @return TRUE if matches, FALSE otherwise.
 */
SR_PRIV gboolean usb_match_manuf_prod(libusb_device *dev,
    const char *manufacturer, const char *product);

/**
 * Get the USB port path of a device.
 * Stub implementation.
 *
 * @param dev The libusb device.
 * @param path Output buffer for the path string.
 * @param path_len Length of the output buffer.
 *
 * @return SR_OK on success, SR_ERR on failure.
 */
SR_PRIV int usb_get_port_path(libusb_device *dev, char *path, int path_len);

#endif
